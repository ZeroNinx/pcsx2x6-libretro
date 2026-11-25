// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "libretro.h"

#include "common/Pcsx2Types.h"

/// The graphics backend of a content session: how the GS shares the frontend's graphics context
/// and how its images reach the frontend. The backend is chosen when content loads and stays
/// for the session; every call is forwarded to it.
namespace LibretroOutput
{
	enum class Backend
	{
		/// The GS creates the frontend's Vulkan device through context negotiation and hands
		/// each image to the frontend directly.
		Vulkan,
		/// The GS renders on its own D3D11 device on the frontend's adapter and hands each image
		/// over through shared textures.
		D3D11,
	};

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

	enum class InterfaceState
	{
		/// The frontend still uses the interface the GS has.
		Current,
		/// The frontend recreated its video driver around the same device; the GS now uses the
		/// new interface.
		Replaced,
		/// The frontend's interface cannot be used; the session must end.
		Invalid,
	};

	enum class Result
	{
		/// The output holds the frame's image, or black while there is none to show.
		Drawn,
		/// The GS has no new image; the previous one stays valid.
		Unchanged,
		/// The output could not be prepared; the reason was logged.
		Failed,
	};

	/// An image drawn for the frontend.
	struct Image
	{
		/// The output slot holding it.
		u32 slot = 0;
		/// The set of output resources the slot belongs to. It changes when the backend
		/// recreates its outputs, for instance for another image size; the frontend takes the
		/// new set over before presenting from it.
		u32 generation = 0;
		u32 width = 0;
		u32 height = 0;
	};

	/// Frontend thread, during retro_load_game: asks the frontend for the graphics context of
	/// `backend`. Returns false when the frontend cannot provide one.
	bool Register(retro_environment_t environment, Backend backend, const ContextCallbacks& callbacks);

	/// Frontend thread, from retro_deinit once the GS is closed and the CPU thread has exited:
	/// forgets the frontend's interface and the registration. Objects the frontend owns are not
	/// destroyed.
	void Unregister();

	/// Frontend thread, while the frontend's context is still valid and before the VM stops or
	/// the GS closes: gives up every output resource the frontend side holds, so that nothing
	/// waits for it. Does nothing when nothing is held.
	void DetachFrontend();

	/// Frontend thread, between frames while the GS is idle: takes over the interface of a video
	/// driver the frontend recreated without a context callback.
	InterfaceState RefreshInterface();

	/// Frontend thread, before each frame request: the output slot the frame is drawn into.
	u32 AcquireOutputSlot();

	/// Frontend thread: makes the next frames black until the GS draws a new image, so nothing
	/// from before a reset is shown again.
	void ClearOutput();

	/// Frontend thread, right before the video refresh: makes `image` the frontend's current
	/// image. Returns false when it cannot be handed over; the reason was logged and the
	/// session has to end.
	bool Present(const Image& image);

	/// Frontend thread: whether an output could not be prepared, which ends the session.
	bool OutputFailed();

	/// GS thread: draws the current output image into `slot` at its own size and submits the
	/// work without waiting for the GPU.
	Result RenderOutput(u32 slot, Image* image);

	/// GS thread: draws opaque black into `slot` regardless of the GS output, for a frontend
	/// frame that has no image to show while the VM does not run.
	Result RenderBlackOutput(u32 slot, Image* image);

	/// GS thread: releases every output. Must run before the GS device is destroyed.
	void ReleaseOutputs();
} // namespace LibretroOutput
