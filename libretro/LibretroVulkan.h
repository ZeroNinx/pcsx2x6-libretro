// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "LibretroOutput.h"

struct retro_hw_render_interface_vulkan;

/// The Vulkan context the frontend shares with the GS. The frontend creates the instance and
/// the device through the core's negotiation callbacks, so the GS renders on the device the
/// frontend presents with; the GS opens while the device is created, and the VM boots once the
/// frontend's context is ready. The functions implement LibretroOutput for this backend.
namespace LibretroVulkan
{
	/// Frontend thread, during retro_load_game: asks the frontend for a Vulkan context created
	/// through negotiation interface v2. Returns false when the frontend cannot provide one.
	bool Register(retro_environment_t environment, const LibretroOutput::ContextCallbacks& callbacks);

	/// See LibretroOutput::Unregister().
	void Unregister();

	/// Frontend thread: the frontend's Vulkan interface while its context is ready; null otherwise.
	const retro_hw_render_interface_vulkan* GetInterface();

	/// Frontend thread, between frames while the GS is idle: takes over the interface of a video
	/// driver the frontend recreated. The frontend keeps its context in that case and calls no
	/// context callback, so this is the only place the change shows.
	LibretroOutput::InterfaceState RefreshInterface();

	/// Frontend thread, before each frame request: waits as the frontend requires for its current
	/// sync index and returns it. The frame is drawn into the output slot of that index.
	u32 AcquireOutputSlot();

	/// See LibretroOutput::ClearOutput().
	void ClearOutput();

	/// See LibretroOutput::RenderOutput().
	LibretroOutput::Result RenderOutput(u32 slot, LibretroOutput::Image* image);

	/// See LibretroOutput::RenderBlackOutput().
	LibretroOutput::Result RenderBlackOutput(u32 slot, LibretroOutput::Image* image);

	/// See LibretroOutput::ReleaseOutputs().
	void ReleaseOutputs();

	/// Frontend thread: hands the image of `image.slot` to the frontend for the next video refresh.
	bool Present(const LibretroOutput::Image& image);

	/// See LibretroOutput::OutputFailed().
	bool OutputFailed();
} // namespace LibretroVulkan
