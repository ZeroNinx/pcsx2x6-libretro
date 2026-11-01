// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#include "LibretroAudio.h"

#include "Host/AudioStream.h"
#include "SPU2/spu2.h"

#include "common/Console.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace
{
	// Frames converted per read; bounds the temporary float buffer.
	constexpr u32 READ_BLOCK_FRAMES = 1024;

	class LibretroAudioStream;

	// The stream SPU2 currently owns. SPU2 creates, writes and destroys it on the CPU thread,
	// which is also the only thread that drains it, so a plain observer is enough.
	LibretroAudioStream* s_stream = nullptr;

	class LibretroAudioStream final : public AudioStream
	{
	public:
		LibretroAudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
			: AudioStream(sample_rate, parameters)
		{
			// Without synchronization the stream neither resamples nor stretches, and the
			// frontend paces playback.
			BaseInitialize(&StereoSampleReaderImpl, AudioSynchronizationMode::Disabled);
			s_stream = this;
		}

		~LibretroAudioStream() override
		{
			if (s_stream == this)
				s_stream = nullptr;
		}

		// Reads exactly the buffered frames: asking for more would make the stream pad with
		// silence and treat it as an underrun.
		void Drain(LibretroAudio::Samples* samples)
		{
			for (u32 available = GetBufferedFramesRelaxed(); available > 0;)
			{
				const u32 frames = std::min(available, READ_BLOCK_FRAMES);
				ReadFrames(m_read_buffer.data(), frames);

				const size_t start = samples->data.size();
				samples->data.resize(start + static_cast<size_t>(frames) * NUM_INPUT_CHANNELS);
				for (u32 i = 0; i < frames * NUM_INPUT_CHANNELS; i++)
				{
					const float value = std::clamp(m_read_buffer[i], -1.0f, 1.0f);
					samples->data[start + i] = static_cast<s16>(std::lrint(value * 32767.0f));
				}
				available -= frames;
			}
		}

	private:
		std::array<SampleType, READ_BLOCK_FRAMES * NUM_INPUT_CHANNELS> m_read_buffer;
	};

	std::unique_ptr<AudioStream> CreateStream(u32 sample_rate, const AudioStreamParameters& parameters)
	{
		// The frontend receives stereo, so the stream never expands to more channels.
		AudioStreamParameters stereo_parameters = parameters;
		stereo_parameters.expansion_mode = AudioExpansionMode::Disabled;
		return std::make_unique<LibretroAudioStream>(sample_rate, stereo_parameters);
	}

	// Frontend queue, in submission order, and the counts of the current content.
	std::vector<s16> s_queue;
	u32 s_queue_sample_rate = 0;
	u64 s_produced_frames = 0;
	u64 s_accepted_frames = 0;
	u64 s_dropped_frames = 0;
	bool s_overflow_reported = false;
	bool s_rate_hold_reported = false;
} // namespace

void LibretroAudio::Install()
{
	SPU2::SetOutputStreamFactory(&CreateStream);
}

void LibretroAudio::Uninstall()
{
	SPU2::SetOutputStreamFactory(nullptr);
}

void LibretroAudio::DrainCommitted(Samples* samples)
{
	const u32 sample_rate = s_stream ? s_stream->GetSampleRate() : 0;
	if (samples->sample_rate != sample_rate)
	{
		samples->discarded_frames += samples->GetFrameCount();
		samples->data.clear();
		samples->sample_rate = sample_rate;
	}

	if (s_stream)
		s_stream->Drain(samples);
}

void LibretroAudio::Submit(retro_audio_sample_batch_t batch, const Samples& samples, u32 accepted_sample_rate)
{
	// Frames discarded on the way here were still produced.
	s_produced_frames += samples.discarded_frames;
	s_dropped_frames += samples.discarded_frames;

	// Queued frames of another sample rate are never played with the new ones. Frames without
	// a rate only exist before the first stream, so they cannot change it.
	if (samples.sample_rate != 0 && samples.sample_rate != s_queue_sample_rate)
	{
		s_dropped_frames += s_queue.size() / 2;
		s_queue.clear();
		s_queue_sample_rate = samples.sample_rate;
	}

	s_produced_frames += samples.GetFrameCount();
	s_queue.insert(s_queue.end(), samples.data.begin(), samples.data.end());

	// Beyond the limit, the oldest frames go first, so recovery plays current audio instead
	// of replaying an ever longer delay.
	const size_t max_frames = static_cast<size_t>(s_queue_sample_rate) * MAX_QUEUED_MS / 1000;
	const size_t queued_frames = s_queue.size() / 2;
	if (queued_frames > max_frames)
	{
		const size_t excess = queued_frames - max_frames;
		s_queue.erase(s_queue.begin(), s_queue.begin() + static_cast<ptrdiff_t>(excess * 2));
		s_dropped_frames += excess;
		if (!s_overflow_reported)
		{
			Console.WarningFmt("Audio is not being delivered; dropping the oldest frames beyond {} ms", MAX_QUEUED_MS);
			s_overflow_reported = true;
		}
	}

	// Played at another rate, the audio would change pitch and speed, so it waits until the
	// frontend accepts its rate.
	if (s_queue_sample_rate != accepted_sample_rate)
	{
		if (!s_queue.empty() && !s_rate_hold_reported)
		{
			Console.WarningFmt("Holding {} Hz audio until the frontend accepts that sample rate", s_queue_sample_rate);
			s_rate_hold_reported = true;
		}
		return;
	}
	s_rate_hold_reported = false;

	// A partial or zero acceptance leaves the rest queued for the next frame.
	size_t submitted = 0;
	while (submitted < s_queue.size() / 2)
	{
		const size_t remaining = s_queue.size() / 2 - submitted;
		const size_t accepted = std::min(batch(s_queue.data() + submitted * 2, remaining), remaining);
		if (accepted == 0)
			break;
		submitted += accepted;
	}
	s_queue.erase(s_queue.begin(), s_queue.begin() + static_cast<ptrdiff_t>(submitted * 2));
	s_accepted_frames += submitted;
	// One report per backlog: it ends once the frontend has taken everything queued.
	if (s_queue.empty())
		s_overflow_reported = false;
}

void LibretroAudio::ResetQueue()
{
	if (s_produced_frames > 0 || s_dropped_frames > 0)
	{
		Console.WriteLnFmt("Audio: {} frames produced, {} accepted, {} dropped, {} still queued", s_produced_frames,
			s_accepted_frames, s_dropped_frames, s_queue.size() / 2);
	}

	s_queue.clear();
	s_queue_sample_rate = 0;
	s_produced_frames = 0;
	s_accepted_frames = 0;
	s_dropped_frames = 0;
	s_overflow_reported = false;
	s_rate_hold_reported = false;
}
