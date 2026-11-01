// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <array>
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
	constexpr u32 FRAME_WIDTH = 640;
	constexpr u32 FRAME_HEIGHT = 480;

	/// Largest frame the session accepts and the frontend is told to expect: the 1080i
	/// display mode, the largest GS video mode, at the fixed upscale multiplier of 1.
	constexpr u32 MAX_FRAME_WIDTH = 1920;
	constexpr u32 MAX_FRAME_HEIGHT = 1080;

	/// Number of frontend input ports sampled for each frame.
	constexpr u32 INPUT_PORTS = 2;

	/// Input the frontend sampled for one frame request. Each port holds one bit per
	/// RETRO_DEVICE_ID_JOYPAD_* button.
	struct InputSnapshot
	{
		std::array<u16, INPUT_PORTS> joypad_buttons = {};
	};

	/// A completed frame and the data the frontend needs to present it, all read on the
	/// CPU thread when the frame completed.
	struct Frame
	{
		/// Number of the frame request this frame completed, counted from 1 per session.
		u64 sequence = 0;
		std::vector<u32> pixels;
		u32 width = 0;
		u32 height = 0;
		/// Bytes per row.
		u32 pitch = 0;
		float aspect_ratio = 0.0f;
		/// Emulated vsync frequency in Hz.
		double vertical_frequency = 0.0;
	};

	/// Creates the CPU thread if needed and boots the VM. Returns after the boot either
	/// succeeded or was completely unwound.
	bool Start(VMBootParameters boot_params, Error* error);

	/// Requests one frame with the given input: the VM advances to its next vsync and no
	/// further. Returns once that frame has been completed and published.
	void RunFrame(const InputSnapshot& input);

	/// Exchanges the frame published since the previous call with `frame`. Returns false,
	/// leaving `frame` untouched, when no new frame was published.
	bool TakeFrame(Frame* frame);

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

	/// Services the CPU thread at each emulated vsync: completes the requested frame, then
	/// waits until the frontend requests the next one or a stop is requested.
	void PumpCPUThread();
} // namespace LibretroSession
