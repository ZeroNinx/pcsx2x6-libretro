// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#include "LibretroD3D11.h"
#include "LibretroSession.h"

#include "GS/GS.h"
#include "GS/Renderers/DX11/GSDevice11.h"
#include "GS/Renderers/DX11/GSTexture11.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/RedtapeWilCom.h"
#include "common/RedtapeWindows.h"

#include "libretro_d3d11.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

// Output protocol. Each set of output textures ("group") holds OUTPUT_TEXTURES shared textures
// of one image size, each with a keyed mutex:
// - KEY_GS: the GS may draw into it. KEY_FRONTEND: it holds an image for the frontend.
// - The frontend holds at most one texture, from the image it presents; the GS draws only into
//   the other one, then releases it to the frontend.
// - A new image size makes the GS create a new group with the next generation. The frontend
//   opens the new group and takes the new image over before presenting it, releases what it
//   held in the old group, and reports the generation it accepted. The GS destroys older
//   groups only after that report, so a group is never destroyed while the frontend uses it.
// - Only AcquireSync() returning S_OK owns a texture. Each side releases only what it owns.
namespace
{
	constexpr u32 OUTPUT_TEXTURES = 2;
	constexpr UINT64 KEY_GS = 0;
	constexpr UINT64 KEY_FRONTEND = 1;
	// Neither side normally waits: the frames are requested one at a time. A longer wait means
	// the protocol broke, which ends the content.
	constexpr DWORD ACQUIRE_TIMEOUT_MS = 1000;

	// Size of the black image shown before the GS has one of its own.
	constexpr u32 BLACK_IMAGE_WIDTH = 640;
	constexpr u32 BLACK_IMAGE_HEIGHT = 480;
	constexpr float OPAQUE_BLACK[4] = {0.0f, 0.0f, 0.0f, 1.0f};

	retro_environment_t s_environment = nullptr;
	LibretroOutput::ContextCallbacks s_callbacks = {};

	// ---- GS thread
	struct GSGroup
	{
		u32 generation = 0;
		u32 width = 0;
		u32 height = 0;
		std::array<std::unique_ptr<GSTexture11>, OUTPUT_TEXTURES> textures;
		std::array<wil::com_ptr_nothrow<IDXGIKeyedMutex>, OUTPUT_TEXTURES> mutexes;
	};
	// Oldest first; the last one is the current group.
	std::vector<GSGroup> s_gs_groups;
	u32 s_last_generation = 0;

	// ---- Shared between the threads
	// The shared handles of the current group, for the frontend to open.
	struct PublishedGroup
	{
		u32 generation = 0;
		u32 width = 0;
		u32 height = 0;
		std::array<HANDLE, OUTPUT_TEXTURES> handles = {};
	};
	std::mutex s_published_mutex;
	PublishedGroup s_published;
	// The newest generation the frontend has taken over; older groups are no longer used by it.
	std::atomic<u32> s_accepted_generation{0};
	std::atomic<bool> s_black_until_image{true};
	// Set when an output cannot be prepared or handed over; the session cannot go on.
	std::atomic<bool> s_output_failed{false};

	// ---- Frontend thread
	struct FrontendGroup
	{
		u32 generation = 0;
		std::array<wil::com_ptr_nothrow<ID3D11Texture2D>, OUTPUT_TEXTURES> textures;
		std::array<wil::com_ptr_nothrow<IDXGIKeyedMutex>, OUTPUT_TEXTURES> mutexes;
		std::array<wil::com_ptr_nothrow<ID3D11ShaderResourceView>, OUTPUT_TEXTURES> views;
	};
	FrontendGroup s_frontend_group;
	// The texture of s_frontend_group the frontend owns, if any.
	std::optional<u32> s_held_slot;
	// The frontend's device and immediate context, held from the first context_reset until the
	// frontend side is detached.
	wil::com_ptr_nothrow<ID3D11Device> s_frontend_device;
	wil::com_ptr_nothrow<ID3D11DeviceContext> s_frontend_context;
	LUID s_frontend_adapter = {};
	// Set by the first context_reset of the registration; later ones only refresh the interface.
	bool s_context_taken = false;
	// Set while the frontend's context exists: from the first context_reset until context_destroy.
	bool s_context_alive = false;
} // namespace

static bool GetAdapterLuid(ID3D11Device* device, LUID* luid)
{
	wil::com_ptr_nothrow<IDXGIDevice> dxgi_device;
	wil::com_ptr_nothrow<IDXGIAdapter> adapter;
	DXGI_ADAPTER_DESC desc;
	if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(dxgi_device.put()))) ||
		FAILED(dxgi_device->GetAdapter(adapter.put())) || FAILED(adapter->GetDesc(&desc)))
		return false;

	*luid = desc.AdapterLuid;
	return true;
}

static bool IsSameAdapter(const LUID& first, const LUID& second)
{
	return first.LowPart == second.LowPart && first.HighPart == second.HighPart;
}

// Frontend thread: releases the texture the frontend owns back to the GS and forgets the group.
// Returns false when the release fails; the group is forgotten either way.
static bool ReleaseFrontendGroup()
{
	bool released = true;
	if (s_held_slot.has_value())
	{
		const HRESULT hr = s_frontend_group.mutexes[*s_held_slot]->ReleaseSync(KEY_GS);
		if (FAILED(hr))
		{
			Console.ErrorFmt("An output texture could not be released by the frontend side: {:08X}", static_cast<u32>(hr));
			released = false;
		}
		s_held_slot.reset();
	}
	s_frontend_group = {};
	return released;
}

// Frontend thread: opens the textures of the published group `generation` on the frontend's device.
static bool OpenFrontendGroup(u32 generation, u32 width, u32 height, FrontendGroup* group)
{
	PublishedGroup published;
	{
		std::unique_lock lock(s_published_mutex);
		published = s_published;
	}
	if (published.generation != generation || published.width != width || published.height != height)
	{
		Console.ErrorFmt("Output generation {} ({}x{}) was presented, but the GS published generation {} ({}x{})",
			generation, width, height, published.generation, published.width, published.height);
		return false;
	}

	group->generation = generation;
	for (u32 i = 0; i < OUTPUT_TEXTURES; i++)
	{
		HRESULT hr = s_frontend_device->OpenSharedResource(published.handles[i], IID_PPV_ARGS(group->textures[i].put()));
		if (SUCCEEDED(hr))
			hr = group->textures[i]->QueryInterface(IID_PPV_ARGS(group->mutexes[i].put()));
		if (SUCCEEDED(hr))
			hr = s_frontend_device->CreateShaderResourceView(group->textures[i].get(), nullptr, group->views[i].put());
		if (FAILED(hr))
		{
			Console.ErrorFmt("Output texture {} of generation {} could not be opened on the frontend's device: {:08X}", i,
				generation, static_cast<u32>(hr));
			return false;
		}
	}
	return true;
}

static void ContextReset()
{
	const retro_hw_render_interface* header = nullptr;
	if (!s_environment(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &header) || !header ||
		header->interface_type != RETRO_HW_RENDER_INTERFACE_D3D11 ||
		header->interface_version != RETRO_HW_RENDER_INTERFACE_D3D11_VERSION)
	{
		s_callbacks.context_failed("The frontend provided no usable D3D11 render interface.");
		return;
	}
	const auto* iface = reinterpret_cast<const retro_hw_render_interface_d3d11*>(header);

	LUID adapter = {};
	if (!iface->device || !iface->context || !GetAdapterLuid(iface->device, &adapter))
	{
		s_callbacks.context_failed("The frontend's D3D11 interface has no usable device.");
		return;
	}

	// The frontend recreated its video driver and kept its device. What the frontend side holds
	// stays valid, including the texture of a paused image; the VM is not started again.
	if (s_context_taken)
	{
		if (!s_frontend_device)
		{
			// The session already failed and gave up the frontend side.
			Console.Warning("The frontend reset its D3D11 context after the session had ended");
			return;
		}
		if (iface->device != s_frontend_device.get() || !IsSameAdapter(adapter, s_frontend_adapter))
		{
			s_callbacks.context_failed("The frontend recreated its video driver on another D3D11 device; the content has to be loaded again.");
			return;
		}
		s_frontend_context = iface->context;
		s_context_alive = true;
		Console.WriteLn("The frontend recreated its video driver; the GS continues with the same D3D11 device");
		return;
	}

	s_context_taken = true;
	s_context_alive = true;
	s_frontend_device = iface->device;
	s_frontend_context = iface->context;
	s_frontend_adapter = adapter;

	// The GS creates its own device on the frontend's adapter, so that the textures it shares
	// can be opened by the frontend's device.
	GSDevice11::SetRequiredAdapter(&adapter);
	Error error;
	const bool opened = LibretroSession::OpenGS(&error);
	GSDevice11::SetRequiredAdapter(nullptr);
	if (!opened)
	{
		s_callbacks.context_failed(error.GetDescription().c_str());
		return;
	}

	s_callbacks.context_ready();
}

// The frontend is about to destroy its context and device. Nothing may still use them.
static void ContextDestroy()
{
	s_callbacks.context_lost();
	s_context_alive = false;
}

bool LibretroD3D11::Register(retro_environment_t environment, const LibretroOutput::ContextCallbacks& callbacks)
{
	s_environment = environment;
	s_callbacks = callbacks;
	s_context_taken = false;
	s_context_alive = false;

	static retro_hw_render_callback hw_render = {};
	hw_render.context_type = RETRO_HW_CONTEXT_D3D11;
	hw_render.version_major = 11;
	hw_render.version_minor = 0;
	hw_render.context_reset = &ContextReset;
	hw_render.context_destroy = &ContextDestroy;
	// When the frontend recreates its video driver, for instance to switch to full screen, it
	// keeps the device and calls context_reset again instead of destroying the context.
	hw_render.cache_context = true;
	if (!environment(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
	{
		Console.Error("The frontend cannot provide a D3D11 context");
		return false;
	}
	return true;
}

void LibretroD3D11::Unregister()
{
	DetachFrontend();
	if (s_context_alive)
		Console.Warning("The core was deinitialized while the frontend's D3D11 context was still in use");
	GSDevice11::SetRequiredAdapter(nullptr);
	s_environment = nullptr;
	s_callbacks = {};
	s_context_taken = false;
	s_context_alive = false;
	s_frontend_adapter = {};
}

void LibretroD3D11::DetachFrontend()
{
	if (s_frontend_context)
	{
		// The frontend's context must not keep a view of a texture the GS may destroy.
		ID3D11ShaderResourceView* const no_view = nullptr;
		s_frontend_context->PSSetShaderResources(0, 1, &no_view);
	}
	ReleaseFrontendGroup();
	s_frontend_context.reset();
	s_frontend_device.reset();
}

LibretroOutput::InterfaceState LibretroD3D11::RefreshInterface()
{
	return LibretroOutput::InterfaceState::Current;
}

u32 LibretroD3D11::AcquireOutputSlot()
{
	return s_held_slot.has_value() ? (*s_held_slot + 1) % OUTPUT_TEXTURES : 0;
}

void LibretroD3D11::ClearOutput()
{
	s_black_until_image.store(true, std::memory_order_release);
}

bool LibretroD3D11::Present(const LibretroOutput::Image& image)
{
	if (!s_frontend_device || !s_frontend_context || image.slot >= OUTPUT_TEXTURES)
	{
		Console.ErrorFmt("Output slot {} was presented without the frontend's D3D11 context", image.slot);
		s_output_failed.store(true, std::memory_order_release);
		return false;
	}

	const bool new_group = image.generation != s_frontend_group.generation;
	if (new_group || !s_held_slot.has_value() || *s_held_slot != image.slot)
	{
		// A new image: the frontend takes it over before it gives up the one it held.
		FrontendGroup opened;
		if (new_group && !OpenFrontendGroup(image.generation, image.width, image.height, &opened))
		{
			s_output_failed.store(true, std::memory_order_release);
			return false;
		}

		FrontendGroup& target = new_group ? opened : s_frontend_group;
		const HRESULT hr = target.mutexes[image.slot]->AcquireSync(KEY_FRONTEND, ACQUIRE_TIMEOUT_MS);
		if (hr != S_OK)
		{
			Console.ErrorFmt("Output texture {} of generation {} could not be taken over: {:08X}", image.slot,
				image.generation, static_cast<u32>(hr));
			s_output_failed.store(true, std::memory_order_release);
			return false;
		}

		bool released = true;
		if (new_group)
		{
			released = ReleaseFrontendGroup();
			s_frontend_group = std::move(opened);
			s_accepted_generation.store(image.generation, std::memory_order_release);
		}
		else if (s_held_slot.has_value())
		{
			const HRESULT release_hr = s_frontend_group.mutexes[*s_held_slot]->ReleaseSync(KEY_GS);
			if (FAILED(release_hr))
			{
				Console.ErrorFmt("Output texture {} could not be released by the frontend side: {:08X}", *s_held_slot,
					static_cast<u32>(release_hr));
				released = false;
			}
		}
		s_held_slot = image.slot;
		if (!released)
		{
			s_output_failed.store(true, std::memory_order_release);
			return false;
		}
	}

	ID3D11ShaderResourceView* const view = s_frontend_group.views[image.slot].get();
	s_frontend_context->PSSetShaderResources(0, 1, &view);
	return true;
}

bool LibretroD3D11::OutputFailed()
{
	return s_output_failed.load(std::memory_order_acquire);
}

// GS thread: destroys the groups the frontend no longer uses.
static void RetireGroups()
{
	const u32 accepted = s_accepted_generation.load(std::memory_order_acquire);
	while (s_gs_groups.size() > 1 && s_gs_groups.front().generation < accepted)
		s_gs_groups.erase(s_gs_groups.begin());
}

// GS thread: the current group when it has the image size, or a new one.
static GSGroup* GetGroup(u32 width, u32 height)
{
	if (!s_gs_groups.empty() && s_gs_groups.back().width == width && s_gs_groups.back().height == height)
		return &s_gs_groups.back();

	GSDevice11* const device = static_cast<GSDevice11*>(g_gs_device.get());
	GSGroup group;
	group.generation = ++s_last_generation;
	group.width = width;
	group.height = height;
	PublishedGroup published;
	published.generation = group.generation;
	published.width = width;
	published.height = height;

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = GSTexture11::GetDXGIFormat(GSTexture::Format::Color);
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
	for (u32 i = 0; i < OUTPUT_TEXTURES; i++)
	{
		wil::com_ptr_nothrow<ID3D11Texture2D> texture;
		wil::com_ptr_nothrow<IDXGIResource> resource;
		HRESULT hr = device->GetD3DDevice()->CreateTexture2D(&desc, nullptr, texture.put());
		if (SUCCEEDED(hr))
			hr = texture->QueryInterface(IID_PPV_ARGS(group.mutexes[i].put()));
		if (SUCCEEDED(hr))
			hr = texture->QueryInterface(IID_PPV_ARGS(resource.put()));
		if (SUCCEEDED(hr))
			hr = resource->GetSharedHandle(&published.handles[i]);
		if (FAILED(hr))
		{
			Console.ErrorFmt("The {}x{} shared output texture could not be created: {:08X}", width, height, static_cast<u32>(hr));
			return nullptr;
		}
		group.textures[i] = std::make_unique<GSTexture11>(std::move(texture), desc, GSTexture::Type::RenderTarget,
			GSTexture::Format::Color);
		// GSTexture11 creates its render target view on first use without reporting a failure,
		// so it is created and checked here, before the group can be drawn into or published.
		if (!static_cast<ID3D11RenderTargetView*>(*group.textures[i]))
		{
			Console.ErrorFmt("The render target view of the {}x{} shared output texture could not be created", width, height);
			return nullptr;
		}
	}

	s_gs_groups.push_back(std::move(group));
	{
		std::unique_lock lock(s_published_mutex);
		s_published = published;
	}
	return &s_gs_groups.back();
}

// GS thread: takes the texture of `slot` for drawing. A texture released to the frontend whose
// image was never presented, for instance a dropped frame, is taken back without waiting.
static bool AcquireForDrawing(IDXGIKeyedMutex* mutex, u32 generation, u32 slot)
{
	// Only WAIT_TIMEOUT means the texture is merely held under the other key. WAIT_ABANDONED
	// (the other side's device went away while holding it) and errors leave the texture in an
	// unknown state, so they fail at once with their own result.
	constexpr HRESULT TIMED_OUT = static_cast<HRESULT>(WAIT_TIMEOUT);
	HRESULT hr = mutex->AcquireSync(KEY_GS, 0);
	if (hr == TIMED_OUT)
		hr = mutex->AcquireSync(KEY_FRONTEND, 0);
	if (hr == TIMED_OUT)
		hr = mutex->AcquireSync(KEY_GS, ACQUIRE_TIMEOUT_MS);
	if (hr == S_OK)
		return true;

	Console.ErrorFmt("Output texture {} of generation {} could not be taken for drawing: {:08X}", slot, generation,
		static_cast<u32>(hr));
	return false;
}

// GS thread: draws the current output image, or opaque black when `has_image` is false, into
// `slot` of the group for its size and releases it to the frontend.
static LibretroOutput::Result DrawOutput(u32 slot, bool has_image, u32 width, u32 height, LibretroOutput::Image* image)
{
	using LibretroOutput::Result;

	RetireGroups();
	GSGroup* const group = slot < OUTPUT_TEXTURES ? GetGroup(width, height) : nullptr;
	if (!group || !AcquireForDrawing(group->mutexes[slot].get(), group->generation, slot))
	{
		s_output_failed.store(true, std::memory_order_release);
		return Result::Failed;
	}

	GSDevice11* const device = static_cast<GSDevice11*>(g_gs_device.get());
	GSTexture11* const texture = group->textures[slot].get();
	if (has_image)
	{
		texture->SetState(GSTexture::State::Invalidated);
		GSDrawDisplayImage(texture);
	}
	else
	{
		device->GetD3DContext()->ClearRenderTargetView(*texture, OPAQUE_BLACK);
	}
	// The GS keeps no binding of the output, which it may destroy before the next draw.
	device->OMSetRenderTargets(nullptr, nullptr);

	const HRESULT hr = group->mutexes[slot]->ReleaseSync(KEY_FRONTEND);
	if (FAILED(hr))
	{
		Console.ErrorFmt("Output texture {} could not be released to the frontend: {:08X}", slot, static_cast<u32>(hr));
		s_output_failed.store(true, std::memory_order_release);
		return Result::Failed;
	}

	*image = {slot, group->generation, width, height};
	return Result::Drawn;
}

LibretroOutput::Result LibretroD3D11::RenderOutput(u32 slot, LibretroOutput::Image* image)
{
	u32 width = 0, height = 0;
	const bool has_image = GSGetDisplayImageSize(&width, &height);
	if (!has_image)
	{
		// Before the first image, and after a reset, black replaces whatever was shown before.
		if (!s_black_until_image.load(std::memory_order_acquire))
			return LibretroOutput::Result::Unchanged;
		width = BLACK_IMAGE_WIDTH;
		height = BLACK_IMAGE_HEIGHT;
	}

	const LibretroOutput::Result result = DrawOutput(slot, has_image, width, height, image);
	if (result == LibretroOutput::Result::Drawn && has_image)
		s_black_until_image.store(false, std::memory_order_release);
	return result;
}

LibretroOutput::Result LibretroD3D11::RenderBlackOutput(u32 slot, LibretroOutput::Image* image)
{
	return DrawOutput(slot, false, BLACK_IMAGE_WIDTH, BLACK_IMAGE_HEIGHT, image);
}

void LibretroD3D11::ReleaseOutputs()
{
	// The frontend side was detached before the GS closes, so nothing waits for these.
	s_gs_groups.clear();
	{
		std::unique_lock lock(s_published_mutex);
		s_published = {};
	}
	s_accepted_generation.store(0, std::memory_order_release);
	s_black_until_image.store(true, std::memory_order_release);
	s_output_failed.store(false, std::memory_order_release);
}
