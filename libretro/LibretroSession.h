// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <functional>
#include <string>
#include <vector>

class Error;
struct VMBootParameters;

/// Owns the CPU thread and the VM session it runs. libretro callbacks only publish
/// requests and wait for their acknowledgement; VM state changes happen on the CPU thread.
namespace LibretroSession
{
	/// Size of the XRGB8888 frames the session publishes.
	constexpr unsigned FRAME_WIDTH = 640;
	constexpr unsigned FRAME_HEIGHT = 480;

	/// Creates the CPU thread if needed and boots the VM. Returns after the boot either
	/// succeeded or was completely unwound.
	bool Start(VMBootParameters boot_params, Error* error);

	/// Lets the VM run to its next vsync and waits for it. The frame of that vsync is
	/// published before this returns.
	void RunFrame();

	/// Exchanges the frame published since the previous call with `pixels`. Returns false,
	/// leaving `pixels` untouched, when no new frame was published.
	bool TakeFrame(std::vector<u32>* pixels);

	/// Stops the active VM, if any, and waits until it has shut down.
	void Stop();

	/// Stops the VM and ends the CPU thread.
	void Shutdown();

	/// Returns true if a VM session is active.
	bool IsActive();

	/// Runs work on the CPU thread. Requests made while no CPU thread exists are dropped.
	void RunOnCPUThread(std::function<void()> function, bool block);

	/// Asks the CPU thread to stop the VM at its next safe point. During startup, the
	/// request cancels the boot once initialization returns.
	void RequestStop();

	/// Services the CPU thread at each emulated vsync. Waits there until the frontend
	/// grants the next frame or a stop is requested.
	void PumpCPUThread();
} // namespace LibretroSession
