// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "libretro.h"

#include "common/Pcsx2Types.h"

struct retro_hw_render_interface_vulkan;

/// The Vulkan context the frontend shares with the GS. The frontend creates the instance and
/// the device through the core's negotiation callbacks, so the GS renders on the device the
/// frontend presents with; the GS opens while the device is created, and the VM boots once the
/// frontend's context is ready.
namespace LibretroVulkan
{
	/// Frontend thread, invoked from the context callbacks.
	struct ContextCallbacks
	{
		/// Boots the VM on the ready context; returns whether it started, having reported any failure.
		bool (*context_ready)();
		/// Reports a context the VM cannot run on and ends the session.
		void (*context_failed)(const char* reason);
		/// Stops the VM and closes the GS while the device is still valid.
		void (*context_lost)();
	};

	/// Frontend thread, during retro_load_game: asks the frontend for a Vulkan context created
	/// through negotiation interface v2. Returns false when the frontend cannot provide one.
	bool Register(retro_environment_t environment, const ContextCallbacks& callbacks);

	/// Frontend thread: the frontend's Vulkan interface while its context is ready; null otherwise.
	const retro_hw_render_interface_vulkan* GetInterface();

	/// Frontend thread, before each frame request: waits as the frontend requires for its current
	/// sync index and returns it. The frame is drawn into the output slot of that index.
	u32 AcquireOutputSlot();

	/// Frontend thread: makes the next frames black until the GS draws a new image, so nothing
	/// from before a reset is shown again.
	void ClearOutput();

	enum class OutputResult
	{
		/// The slot holds the frame's image, or black while there is none to show.
		Drawn,
		/// The GS has no new image; the previous one stays valid.
		Unchanged,
		/// The slot could not be prepared; the reason was logged.
		Failed,
	};

	/// GS thread: draws the current output image into `slot` at its own size and submits the work
	/// without waiting for the GPU. Reports the size of the image drawn.
	OutputResult RenderOutput(u32 slot, u32* width, u32* height);

	/// GS thread: draws opaque black into `slot` regardless of the GS output, for a frontend frame
	/// that has no image to show while the VM does not run.
	OutputResult RenderBlackOutput(u32 slot, u32* width, u32* height);

	/// GS thread: releases every output slot. Must run before the GS device is destroyed.
	void ReleaseOutputs();

	/// Frontend thread: hands the image of `slot` to the frontend for the next video refresh.
	void SetOutputImage(u32 slot);

	/// Frontend thread: whether an output image could not be prepared, which ends the session.
	bool OutputFailed();
} // namespace LibretroVulkan
