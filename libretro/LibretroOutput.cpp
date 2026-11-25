// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#include "LibretroOutput.h"
#include "LibretroVulkan.h"
#ifdef _WIN32
#include "LibretroD3D11.h"
#endif

namespace
{
	// Set on the frontend thread while content loads, before the GS opens, and kept until the
	// next load; the GS thread only reads it while a GS exists.
	LibretroOutput::Backend s_backend = LibretroOutput::Backend::Vulkan;
} // namespace

// Calls `function` of the session's backend with `args`.
#ifdef _WIN32
#define DISPATCH(function, ...) \
	(s_backend == Backend::D3D11 ? LibretroD3D11::function(__VA_ARGS__) : LibretroVulkan::function(__VA_ARGS__))
#else
#define DISPATCH(function, ...) LibretroVulkan::function(__VA_ARGS__)
#endif

using LibretroOutput::Backend;

bool LibretroOutput::Register(retro_environment_t environment, Backend backend, const ContextCallbacks& callbacks)
{
	s_backend = backend;
	return DISPATCH(Register, environment, callbacks);
}

void LibretroOutput::Unregister()
{
#ifdef _WIN32
	// Both backends may hold frontend state from earlier sessions of this initialization.
	LibretroD3D11::Unregister();
#endif
	LibretroVulkan::Unregister();
}

void LibretroOutput::DetachFrontend()
{
#ifdef _WIN32
	if (s_backend == Backend::D3D11)
		LibretroD3D11::DetachFrontend();
#endif
}

LibretroOutput::InterfaceState LibretroOutput::RefreshInterface()
{
	return DISPATCH(RefreshInterface);
}

u32 LibretroOutput::AcquireOutputSlot()
{
	return DISPATCH(AcquireOutputSlot);
}

void LibretroOutput::ClearOutput()
{
	DISPATCH(ClearOutput);
}

bool LibretroOutput::Present(const Image& image)
{
	return DISPATCH(Present, image);
}

bool LibretroOutput::OutputFailed()
{
	return DISPATCH(OutputFailed);
}

LibretroOutput::Result LibretroOutput::RenderOutput(u32 slot, Image* image)
{
	return DISPATCH(RenderOutput, slot, image);
}

LibretroOutput::Result LibretroOutput::RenderBlackOutput(u32 slot, Image* image)
{
	return DISPATCH(RenderBlackOutput, slot, image);
}

void LibretroOutput::ReleaseOutputs()
{
	DISPATCH(ReleaseOutputs);
}
