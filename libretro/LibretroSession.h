// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <functional>
#include <string>

class Error;
struct VMBootParameters;

/// Owns the CPU thread and the VM session it runs. libretro callbacks only publish
/// requests and wait for their acknowledgement; VM state changes happen on the CPU thread.
namespace LibretroSession
{
	/// Creates the CPU thread if needed and boots the VM. Returns after the boot either
	/// succeeded or was completely unwound.
	bool Start(VMBootParameters boot_params, Error* error);

	/// Lets the VM run to its next vsync and waits for it.
	void RunFrame();

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
