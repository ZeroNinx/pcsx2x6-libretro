// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#include "LibretroVulkan.h"
#include "LibretroSession.h"

#include "GS/GS.h"
#include "GS/Renderers/Vulkan/GSDeviceVK.h"
#include "GS/Renderers/Vulkan/GSTextureVK.h"

#include "common/Console.h"
#include "common/Error.h"

#include "libretro_vulkan.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace
{
	retro_environment_t s_environment = nullptr;
	LibretroVulkan::ContextCallbacks s_callbacks = {};

	// The context the GS device runs on. It outlives every device created on it.
	GSDeviceVK::ExternalContext s_context;

	// The frontend's interface while its context is ready. Held from locking the shared queue
	// until unlocking it, so the interface cannot be retired while the GS uses the queue.
	std::mutex s_interface_mutex;
	const retro_hw_render_interface_vulkan* s_interface = nullptr;
	bool s_early_queue_use_reported = false;

	// Set while a frontend callback waits for the GS: the frontend is blocked in that call, so
	// the GS may use the queue without the frontend's lock, which may not exist yet.
	std::atomic<bool> s_frontend_blocked{false};

	// Marks a frontend callback that waits for the GS.
	struct FrontendBlocked
	{
		FrontendBlocked() { s_frontend_blocked.store(true, std::memory_order_release); }
		~FrontendBlocked() { s_frontend_blocked.store(false, std::memory_order_release); }
	};
} // namespace

static void LockQueue()
{
	s_interface_mutex.lock();
	if (s_interface)
		s_interface->lock_queue(s_interface->handle);
	else if (!s_frontend_blocked.load(std::memory_order_acquire) && !std::exchange(s_early_queue_use_reported, true))
		Console.Error("The GS used the shared graphics queue while the frontend's context was not ready");
}

static void UnlockQueue()
{
	if (s_interface)
		s_interface->unlock_queue(s_interface->handle);
	s_interface_mutex.unlock();
}

static void SetInterface(const retro_hw_render_interface_vulkan* iface)
{
	std::unique_lock lock(s_interface_mutex);
	s_interface = iface;
	s_early_queue_use_reported = false;
}

static const VkApplicationInfo* GetApplicationInfo()
{
	static const VkApplicationInfo info = {VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "PCSX2x6", 0, "PCSX2x6", 0,
		VK_API_VERSION_1_1};
	return &info;
}

// The instance the frontend creates carries the extensions the GS needs, and the GS loads
// every Vulkan function through the frontend's loader.
static VkInstance CreateInstance(PFN_vkGetInstanceProcAddr get_instance_proc_addr, const VkApplicationInfo* app,
	retro_vulkan_create_instance_wrapper_t create_instance_wrapper, void* opaque)
{
	s_context.instance = VK_NULL_HANDLE;
	s_context.physical_device = VK_NULL_HANDLE;
	s_context.get_instance_proc_addr = get_instance_proc_addr;

	// The wrapper and its opaque data are only valid during this call.
	s_context.instance = GSDeviceVK::CreateExternalInstance(get_instance_proc_addr,
		[create_instance_wrapper, opaque](const VkInstanceCreateInfo& create_info) {
			return create_instance_wrapper(opaque, &create_info);
		},
		&s_context.instance_extensions);
	if (s_context.instance == VK_NULL_HANDLE)
		Console.Error("The Vulkan instance for the GS could not be created");
	return s_context.instance;
}

// Negotiation v1 entry point. The frontend only checks that it exists before preferring
// create_device2; the core supports nothing older than v2.
static bool CreateDeviceV1(retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu,
	VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char** required_device_extensions,
	unsigned num_required_device_extensions, const char** required_device_layers, unsigned num_required_device_layers,
	const VkPhysicalDeviceFeatures* required_features)
{
	Console.Error("The frontend requested a Vulkan device through context negotiation v1, which this core does not "
				  "support; v2 is required");
	return false;
}

// Opens the GS, which creates its device through the frontend's wrapper, and returns that
// device for the frontend to share.
static bool CreateDevice2(retro_vulkan_context* context, VkInstance instance, VkPhysicalDevice gpu,
	VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
	retro_vulkan_create_device_wrapper_t create_device_wrapper, void* opaque)
{
	if (instance == VK_NULL_HANDLE || instance != s_context.instance)
	{
		Console.Error("The frontend requested a Vulkan device on an instance the core did not create");
		return false;
	}

	const FrontendBlocked frontend_blocked;

	// Every attempt loads the functions again through the resolver of this request: a device
	// that failed to open reset them.
	s_context.get_instance_proc_addr = get_instance_proc_addr;
	s_context.physical_device = gpu;
	s_context.device_handed_over = false;

	// The wrapper and its opaque data are only valid during this call, which waits for the GS.
	s_context.create_device = [create_device_wrapper, opaque](VkPhysicalDevice physical_device,
								  const VkDeviceCreateInfo& create_info) {
		return create_device_wrapper(physical_device, opaque, &create_info);
	};
	GSDeviceVK::SetExternalContext(&s_context);

	Error error;
	const bool opened = LibretroSession::OpenGS(&error);
	s_context.create_device = nullptr;
	if (!opened)
	{
		Console.ErrorFmt("The Vulkan device for the GS could not be created: {}", error.GetDescription());
		GSDeviceVK::SetExternalContext(nullptr);
		return false;
	}

	const GSDeviceVK* const device = GSDeviceVK::GetInstance();
	const VkPhysicalDevice physical_device = device->GetPhysicalDevice();
	const u32 queue_family = device->GetGraphicsQueueFamilyIndex();

	// The frontend presents with the queue it is given, so the GS queue must reach its surface.
	VkBool32 present_supported = VK_TRUE;
	if (surface != VK_NULL_HANDLE &&
		(vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, queue_family, surface, &present_supported) != VK_SUCCESS ||
			!present_supported))
	{
		Console.Error("The GS graphics queue cannot present to the frontend's surface");
		LibretroSession::CloseGS();
		GSDeviceVK::SetExternalContext(nullptr);
		return false;
	}

	context->gpu = physical_device;
	context->device = device->GetDevice();
	context->queue = device->GetGraphicsQueue();
	context->queue_family_index = queue_family;
	context->presentation_queue = context->queue;
	context->presentation_queue_family_index = queue_family;
	s_context.device_handed_over = true;
	Console.WriteLnFmt("The GS shares the frontend's Vulkan device on {}", device->GetName());
	return true;
}

static void ContextReset()
{
	const FrontendBlocked frontend_blocked;
	const retro_hw_render_interface_vulkan* iface = nullptr;
	if (!s_environment(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &iface) || !iface ||
		iface->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN ||
		iface->interface_version != RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION)
	{
		s_callbacks.context_failed("The frontend provided no usable Vulkan render interface.");
		return;
	}

	// Only the device the GS created is shared; a device the frontend made on its own is not.
	const GSDeviceVK* const device = GSDeviceVK::GetInstance();
	if (!device || iface->device != device->GetDevice())
	{
		s_callbacks.context_failed("The frontend's Vulkan context does not use the device the GS created.");
		return;
	}

	SetInterface(iface);
	if (!s_callbacks.context_ready())
		SetInterface(nullptr);
}

// The frontend is about to destroy its context and the device. Nothing may still run on it.
static void ContextDestroy()
{
	s_callbacks.context_lost();
	SetInterface(nullptr);
	GSDeviceVK::SetExternalContext(nullptr);
}

bool LibretroVulkan::Register(retro_environment_t environment, const ContextCallbacks& callbacks)
{
	s_environment = environment;
	s_callbacks = callbacks;
	s_context.lock_queue = &LockQueue;
	s_context.unlock_queue = &UnlockQueue;

	retro_hw_render_context_negotiation_interface support = {RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN, 0};
	if (!environment(RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT, &support) ||
		support.interface_version < 2)
	{
		Console.ErrorFmt("The frontend supports Vulkan context negotiation up to v{}; v2 is required",
			support.interface_version);
		return false;
	}

	static retro_hw_render_callback hw_render = {};
	hw_render.context_type = RETRO_HW_CONTEXT_VULKAN;
	hw_render.version_major = 1;
	hw_render.version_minor = 1;
	hw_render.context_reset = &ContextReset;
	hw_render.context_destroy = &ContextDestroy;
	if (!environment(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
	{
		Console.Error("The frontend cannot provide a Vulkan context");
		return false;
	}

	static retro_hw_render_context_negotiation_interface_vulkan negotiation = {};
	negotiation.interface_type = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN;
	negotiation.interface_version = 2;
	negotiation.get_application_info = &GetApplicationInfo;
	negotiation.create_device = &CreateDeviceV1;
	// RetroArch calls destroy_device when it tears down its video driver, which can happen after
	// the core is unloaded; context_destroy already released everything on the device.
	negotiation.destroy_device = nullptr;
	negotiation.create_instance = &CreateInstance;
	negotiation.create_device2 = &CreateDevice2;
	if (!environment(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, &negotiation))
	{
		Console.Error("The frontend does not accept the Vulkan context negotiation interface");
		return false;
	}

	return true;
}

const retro_hw_render_interface_vulkan* LibretroVulkan::GetInterface()
{
	std::unique_lock lock(s_interface_mutex);
	return s_interface;
}

namespace
{
	// The sync indices a frontend can report: one bit each in get_sync_index_mask().
	constexpr u32 MAX_OUTPUT_SLOTS = 32;

	// Size of the black image shown before the GS has one of its own.
	constexpr u32 BLACK_IMAGE_WIDTH = 640;
	constexpr u32 BLACK_IMAGE_HEIGHT = 480;
	// Packed as SetClearColor() expects: red in the low byte, alpha in the high byte. The output
	// is opaque like the drawn images, whose alpha the conversion shader sets to 1.
	constexpr u32 OPAQUE_BLACK = 0xFF000000u;

	// The image drawn for one frontend sync index. The frontend may keep presenting an earlier
	// image description, so each slot keeps its description at a fixed address.
	struct OutputSlot
	{
		std::unique_ptr<GSTextureVK> texture;
		retro_vulkan_image image = {};
	};

	// Owned by the GS thread; the frontend thread only reads a description while the GS is idle.
	std::array<OutputSlot, MAX_OUTPUT_SLOTS> s_output_slots;

	// Frontend thread: the sync index mask the slots were made for.
	u32 s_sync_index_mask = 0;

	// Set by the frontend thread, consumed by the GS thread.
	std::atomic<bool> s_rebuild_outputs{false};
	std::atomic<bool> s_black_until_image{true};

	// Set by the GS thread when an output image cannot be prepared; the session cannot go on.
	std::atomic<bool> s_output_failed{false};
} // namespace

u32 LibretroVulkan::AcquireOutputSlot()
{
	const retro_hw_render_interface_vulkan* const iface = GetInterface();
	if (!iface)
	{
		Console.Error("An output slot was requested without the frontend's Vulkan context");
		return 0;
	}

	// A changed mask means the frontend's device is idle, so every slot can be recreated.
	const u32 mask = iface->get_sync_index_mask(iface->handle);
	if (mask != s_sync_index_mask)
	{
		s_sync_index_mask = mask;
		s_rebuild_outputs.store(true, std::memory_order_release);
	}

	const u32 index = iface->get_sync_index(iface->handle);
	iface->wait_sync_index(iface->handle);
	if (index >= MAX_OUTPUT_SLOTS || !(mask & (1u << index)))
	{
		Console.ErrorFmt("The frontend reported sync index {} outside its mask {:#x}", index, mask);
		return 0;
	}

	return index;
}

void LibretroVulkan::ClearOutput()
{
	s_black_until_image.store(true, std::memory_order_release);
}

// GS thread: (re)creates the texture of a slot for an image of the given size. A replaced
// texture is destroyed once the GPU has finished the work submitted before.
static bool PrepareOutputSlot(OutputSlot& slot, u32 width, u32 height)
{
	if (slot.texture && slot.texture->GetWidth() == static_cast<int>(width) &&
		slot.texture->GetHeight() == static_cast<int>(height))
		return true;

	slot.texture.reset();

	// The frontend may sample the image with an sRGB view, which an 8-bit format needs a mutable
	// format for; a render target can be drawn into, sampled and copied from as the frontend requires.
	slot.texture = GSTextureVK::Create(GSTexture::Type::RenderTarget, GSTexture::Format::Color,
		static_cast<int>(width), static_cast<int>(height), 1, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
	if (!slot.texture)
	{
		Console.ErrorFmt("The {}x{} output image could not be created", width, height);
		return false;
	}

	slot.image.image_view = slot.texture->GetView();
	slot.image.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	slot.image.create_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, slot.texture->GetImage(),
		VK_IMAGE_VIEW_TYPE_2D, slot.texture->GetVkFormat(),
		{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
			VK_COMPONENT_SWIZZLE_IDENTITY},
		{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	return true;
}

// GS thread: draws the current output image into a slot, or opaque black when `has_image` is
// false, and submits the work without waiting for the GPU.
static LibretroVulkan::OutputResult DrawOutput(u32 slot_index, bool has_image, u32 image_width, u32 image_height)
{
	using LibretroVulkan::OutputResult;

	OutputSlot& slot = s_output_slots[slot_index];
	if (!PrepareOutputSlot(slot, image_width, image_height))
	{
		s_output_failed.store(true, std::memory_order_release);
		return OutputResult::Failed;
	}

	// The slot is redrawn completely. The frontend may have changed the layout of what it was
	// given, so the old contents are discarded rather than transitioned from a tracked layout;
	// that transition waits for the frontend's reads through its colour attachment barrier.
	GSDeviceVK* const device = GSDeviceVK::GetInstance();
	GSTextureVK* const texture = slot.texture.get();
	device->EndRenderPass();
	texture->OverrideImageLayout(GSTextureVK::Layout::Undefined);
	if (has_image)
	{
		texture->SetState(GSTexture::State::Invalidated);
		GSDrawDisplayImage(texture);
	}
	else
	{
		// Cleared to opaque black through the render pass load operation, not a transfer.
		const GSVector4i rect(0, 0, static_cast<int>(image_width), static_cast<int>(image_height));
		device->ClearRenderTarget(texture, OPAQUE_BLACK);
		device->OMSetRenderTargets(texture, nullptr, rect);
		device->BeginRenderPassForStretchRect(texture, rect, rect, false);
	}
	device->EndRenderPass();

	// Colour writes become visible to the frontend's fragment shader reads, and the work is
	// submitted now so that it precedes the frontend's use of the image on the shared queue.
	texture->TransitionToLayout(GSTextureVK::Layout::ShaderReadOnly);
	device->ExecuteCommandBuffer(false);
	return OutputResult::Drawn;
}

LibretroVulkan::OutputResult LibretroVulkan::RenderOutput(u32 slot_index, u32* width, u32* height)
{
	if (s_rebuild_outputs.exchange(false, std::memory_order_acq_rel))
		ReleaseOutputs();

	u32 image_width = 0, image_height = 0;
	const bool has_image = GSGetDisplayImageSize(&image_width, &image_height);
	if (!has_image)
	{
		// Before the first image, and after a reset, black replaces whatever was shown before.
		if (!s_black_until_image.load(std::memory_order_acquire))
			return OutputResult::Unchanged;
		image_width = BLACK_IMAGE_WIDTH;
		image_height = BLACK_IMAGE_HEIGHT;
	}

	const OutputResult result = DrawOutput(slot_index, has_image, image_width, image_height);
	if (result == OutputResult::Drawn && has_image)
		s_black_until_image.store(false, std::memory_order_release);
	*width = image_width;
	*height = image_height;
	return result;
}

LibretroVulkan::OutputResult LibretroVulkan::RenderBlackOutput(u32 slot_index, u32* width, u32* height)
{
	if (s_rebuild_outputs.exchange(false, std::memory_order_acq_rel))
		ReleaseOutputs();

	*width = BLACK_IMAGE_WIDTH;
	*height = BLACK_IMAGE_HEIGHT;
	return DrawOutput(slot_index, false, BLACK_IMAGE_WIDTH, BLACK_IMAGE_HEIGHT);
}

void LibretroVulkan::ReleaseOutputs()
{
	for (OutputSlot& slot : s_output_slots)
	{
		slot.texture.reset();
		slot.image = {};
	}
	s_black_until_image.store(true, std::memory_order_release);
	s_output_failed.store(false, std::memory_order_release);
}

bool LibretroVulkan::OutputFailed()
{
	return s_output_failed.load(std::memory_order_acquire);
}

void LibretroVulkan::SetOutputImage(u32 slot)
{
	const retro_hw_render_interface_vulkan* const iface = GetInterface();
	if (!iface)
	{
		Console.Error("An output image was presented without the frontend's Vulkan context");
		return;
	}

	iface->set_image(iface->handle, &s_output_slots[slot].image, 0, nullptr, iface->queue_index);
}
