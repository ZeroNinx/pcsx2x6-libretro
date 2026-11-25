// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "LibretroOutput.h"

/// The D3D11 backend. The frontend creates its own D3D11 device; the GS renders on a device of
/// its own on the same adapter and on its own thread, and hands each image to the frontend
/// through a pair of keyed-mutex shared textures. The frontend side opens them on the
/// frontend's device and presents them as the standard D3D11 interface v1 expects: a shader
/// resource view in pixel shader slot 0 of the frontend's immediate context. The functions
/// implement LibretroOutput for this backend.
namespace LibretroD3D11
{
	/// Frontend thread, during retro_load_game: asks the frontend for a D3D11 context. Returns
	/// false when the frontend cannot provide one.
	bool Register(retro_environment_t environment, const LibretroOutput::ContextCallbacks& callbacks);

	/// See LibretroOutput::Unregister().
	void Unregister();

	/// See LibretroOutput::DetachFrontend().
	void DetachFrontend();

	/// The frontend announces a recreated video driver through context_reset, so there is
	/// nothing to take over between frames.
	LibretroOutput::InterfaceState RefreshInterface();

	/// Frontend thread: the output slot the frontend does not hold.
	u32 AcquireOutputSlot();

	/// See LibretroOutput::ClearOutput().
	void ClearOutput();

	/// Frontend thread: takes `image` over from the GS and binds it for the next video refresh.
	bool Present(const LibretroOutput::Image& image);

	/// See LibretroOutput::OutputFailed().
	bool OutputFailed();

	/// See LibretroOutput::RenderOutput().
	LibretroOutput::Result RenderOutput(u32 slot, LibretroOutput::Image* image);

	/// See LibretroOutput::RenderBlackOutput().
	LibretroOutput::Result RenderBlackOutput(u32 slot, LibretroOutput::Image* image);

	/// See LibretroOutput::ReleaseOutputs().
	void ReleaseOutputs();
} // namespace LibretroD3D11
