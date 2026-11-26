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
	struct FrameSize
	{
		u32 width = 0;
		u32 height = 0;
	};

	/// Largest frame the session accepts and the frontend is told to expect: the 1080i display
	/// mode, the largest GS video mode, at the internal resolution multiplier.
	constexpr FrameSize GetMaxFrameSize(u32 upscale_multiplier)
	{
		return {1920 * upscale_multiplier, 1080 * upscale_multiplier};
	}

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

	/// The image of a completed frame: the GS composite output at its own size, drawn into a
	/// frontend output slot, with the display aspect ratio the GS presents it at.
	struct Frame
	{
		/// Number of the frame request this frame completed, counted from 1 per session.
		u64 sequence = 0;
		/// The output slot holding the image and the generation of its outputs; see
		/// LibretroOutput::Image.
		u32 slot = 0;
		u32 generation = 0;
		u32 width = 0;
		u32 height = 0;
		float aspect_ratio = 0.0f;
	};

	/// Creates the CPU thread if needed and keeps the boot parameters for Start(); nothing is
	/// booted yet. Frames of the session may be as large as GetMaxFrameSize() allows for
	/// `upscale_multiplier`. Stop() discards parameters that were never started.
	bool Prepare(VMBootParameters boot_params, u32 upscale_multiplier, Error* error);

	/// Opens the GS on the CPU thread before the VM boots, so that the GS device exists while
	/// the frontend sets up the graphics context it shares. Returns once the GS opened or failed.
	bool OpenGS(Error* error);

	/// Boots the prepared VM. Returns after the boot either succeeded or was completely unwound.
	bool Start(Error* error);

	/// Closes the GS opened by OpenGS(). Only valid while no VM session is active; does nothing
	/// when the GS is not open.
	void CloseGS();

	/// What one frame request produced.
	struct FrameResult
	{
		/// The VM ran to its next vsync. False when there is no active session or a paused VM
		/// completed the request without running, leaving the input unapplied apart from its
		/// device changes.
		bool advanced = false;
		/// A new image was published since the previous request; `frame` describes it.
		bool new_frame = false;
		Frame frame;
		/// Emulated vsync frequency in Hz when the latest frame completed, image or not; 0 while
		/// the session has not completed a frame in an initialized video mode.
		double vsync_frequency = 0.0;
	};

	/// Requests one frame with the given input, drawn into `output_slot`: the VM advances to its
	/// next vsync and no further. Returns once that frame has been completed and published, with
	/// its result. When the VM advanced, the audio of the frames completed since the previous
	/// such request is moved into `audio`; each completion contributes exactly the frames SPU2
	/// had committed by its vsync. Otherwise `audio` is left untouched.
	FrameResult RunFrame(const InputSnapshot& input, u32 output_slot, LibretroAudio::Samples* audio);

	/// Resets the VM of the active session on the CPU thread. Returns true once the reset has
	/// actually happened, leaving the VM running or paused as before and waiting for the next
	/// frame request; false when there is no active session or the VM refused, for instance
	/// while a memory card is being written, in which case the session continues unchanged.
	bool Reset();

	/// Outcome of a state operation.
	enum class StateResult
	{
		Done,
		/// Refused before anything changed; the session continues unchanged.
		Rejected,
		/// A load failed after it had started changing the VM. The VM is left stopped at the
		/// point the load began and must not run again; the session has to be ended.
		Failed,
	};

	/// Runs `work` on the CPU thread at the point where save states are taken and restored:
	/// outside VM execution, after the frame the VM last completed, with the VU and GS threads
	/// idle. A running VM leaves execution at its next safe point first; no frame is run for it.
	/// The VM keeps its running or paused state and runs again only on a new frame request.
	/// After a load that is Done, the session drops output and input of the replaced state:
	/// its unpresented frame and audio, the held host input levels, and armed Coin/Test edges.
	StateResult RunStateOperation(bool loads, const std::function<StateResult(Error*)>& work, Error* error);

	/// Identifies the VM state between changes: it changes whenever the VM completes a frame,
	/// a state load or a reset succeeds, or a session starts or ends. Read and changed under the
	/// session lock; work run by RunStateOperation() reads it while the VM cannot change.
	u64 GetStateGeneration();

	/// Draws opaque black into `output_slot` without running the VM, for a frame request that
	/// produced no image while nothing has been shown yet. Returns false, leaving `frame`
	/// untouched, without an active session or when the image cannot be drawn.
	bool DrawBlackFrame(u32 output_slot, Frame* frame);

	/// Stops the active VM, if any, and waits until it has shut down. Discards prepared boot
	/// parameters and returns the frame limit to native resolution; the GS stays open until
	/// CloseGS().
	void Stop();

	/// Stops the VM, closes the GS and ends the CPU thread.
	void Shutdown();

	/// While the process terminates, after the system has ended every other thread: releases
	/// the handles of the CPU and GS threads so their static objects can be destroyed. Takes no
	/// lock, waits for nothing, allocates nothing and logs nothing.
	void ForgetTerminatedThreads();

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
