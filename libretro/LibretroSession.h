// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "LibretroAudio.h"

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
	/// Largest frame the session accepts and the frontend is told to expect: the 1080i
	/// display mode, the largest GS video mode, at the fixed upscale multiplier of 1.
	constexpr u32 MAX_FRAME_WIDTH = 1920;
	constexpr u32 MAX_FRAME_HEIGHT = 1080;

	/// Number of frontend input ports sampled for each frame.
	constexpr u32 INPUT_PORTS = 2;

	/// Input the frontend sampled for one frame request. Each port holds one bit per
	/// RETRO_DEVICE_ID_JOYPAD_* button, whether the frontend has a device on it, and whether
	/// the frontend assigned the port's device since the previous request.
	struct InputSnapshot
	{
		std::array<u16, INPUT_PORTS> joypad_buttons = {};
		std::array<bool, INPUT_PORTS> connected = {};
		std::array<bool, INPUT_PORTS> device_changed = {};
	};

	/// The image of a completed frame: the GS composite output at its own size, in XRGB8888,
	/// with the display aspect ratio the GS presents it at.
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
	};

	/// Creates the CPU thread if needed and keeps the boot parameters for Start(); nothing is
	/// booted yet. Stop() discards parameters that were never started.
	bool Prepare(VMBootParameters boot_params, Error* error);

	/// Opens the GS on the CPU thread before the VM boots, so that the GS device exists while
	/// the frontend sets up the graphics context it shares. Returns once the GS opened or failed.
	bool OpenGS(Error* error);

	/// Boots the prepared VM. Returns after the boot either succeeded or was completely unwound.
	bool Start(Error* error);

	/// Closes the GS opened by OpenGS(). Only valid while no VM session is active; does nothing
	/// when the GS is not open.
	void CloseGS();

	/// Requests one frame with the given input: the VM advances to its next vsync and no
	/// further. Returns once that frame has been completed and published: true when the VM
	/// advanced, false when there is no active session or a paused VM completed the request
	/// without running, leaving the input unapplied apart from its device changes.
	bool RunFrame(const InputSnapshot& input);

	/// Resets the VM of the active session on the CPU thread. Returns true once the reset has
	/// actually happened, leaving the VM running or paused as before and waiting for the next
	/// frame request; false when there is no active session or the VM refused, for instance
	/// while a memory card is being written, in which case the session continues unchanged.
	bool Reset();

	/// Exchanges the frame published since the previous call with `frame`. Returns false,
	/// leaving `frame` untouched, when no new frame was published.
	bool TakeFrame(Frame* frame);

	/// Emulated vsync frequency in Hz when the latest frame completed, image or not; 0 while
	/// the session has not completed a frame in an initialized video mode.
	double GetVsyncFrequency();

	/// Moves the audio of the frames completed since the previous call into `samples`. Each
	/// completion contributes exactly the frames SPU2 had committed by its vsync.
	void TakeAudio(LibretroAudio::Samples* samples);

	/// Stops the active VM, if any, and waits until it has shut down. Discards prepared boot
	/// parameters; the GS stays open until CloseGS().
	void Stop();

	/// Stops the VM, closes the GS and ends the CPU thread.
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
