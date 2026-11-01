// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "libretro.h"

#include "common/Pcsx2Types.h"

#include <vector>

/// Carries SPU2 output to the frontend. SPU2 writes into a stream that opens no host device;
/// the CPU thread drains what SPU2 committed at each completed frame, and the frontend thread
/// keeps a bounded queue it submits through the batch callback.
namespace LibretroAudio
{
	/// Most audio the frontend queue retains, so a stalled frontend does not build up delay.
	constexpr u32 MAX_QUEUED_MS = 250;

	/// Interleaved signed 16-bit stereo frames at one sample rate.
	struct Samples
	{
		std::vector<s16> data;
		/// 0 while no output stream exists.
		u32 sample_rate = 0;
		/// Frames discarded before reaching the frontend because the sample rate changed.
		u64 discarded_frames = 0;

		u32 GetFrameCount() const { return static_cast<u32>(data.size() / 2); }
	};

	/// Makes SPU2 create its output streams here instead of on a host backend.
	void Install();

	/// Restores the configured SPU2 backend. Only valid while no VM exists.
	void Uninstall();

	/// CPU thread: appends the frames SPU2 has committed since the previous call. Frames of
	/// an earlier sample rate are discarded so that one buffer never mixes rates.
	void DrainCommitted(Samples* samples);

	/// Frontend thread: queues the frames of one completed frame and submits as many queued
	/// frames as the frontend accepts. Audio is only submitted at `accepted_sample_rate`, the
	/// rate the frontend has accepted; audio of another rate stays queued within the limit.
	void Submit(retro_audio_sample_batch_t batch, const Samples& samples, u32 accepted_sample_rate);

	/// Frontend thread: drops queued audio at a content boundary and reports the counts of
	/// the content that ended.
	void ResetQueue();
} // namespace LibretroAudio
