// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#include "LibretroAudio.h"
#include "LibretroHost.h"
#include "LibretroInput.h"
#include "LibretroSession.h"
#include "libretro.h"

#include "VMManager.h"

#include "common/Console.h"
#include "common/Error.h"

#include "svnrev.h"

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	constexpr const char* CORE_NAME = "PCSX2x6";
	constexpr const char* CORE_VERSION = PCSX2X6_LIBRETRO_VERSION " (" GIT_REV "; PCSX2x6 " PCSX2X6_BASE_REVISION ")";
	constexpr const char* CONTENT_EXTENSIONS = "acgame";

	// Reported until a session delivers its own geometry and timing.
	constexpr u32 INITIAL_WIDTH = 640;
	constexpr u32 INITIAL_HEIGHT = 480;
	constexpr float INITIAL_ASPECT_RATIO = 4.0f / 3.0f;
	constexpr double NOMINAL_FPS = 59.94;
	constexpr double SAMPLE_RATE = 48000.0;

	retro_environment_t s_environment = nullptr;
	retro_video_refresh_t s_video_refresh = nullptr;
	retro_audio_sample_batch_t s_audio_batch = nullptr;
	retro_input_poll_t s_input_poll = nullptr;
	retro_input_state_t s_input_state = nullptr;
	bool s_initialized = false;

	// The device the frontend assigned to each input port; a RetroPad until told otherwise.
	std::array<bool, LibretroSession::INPUT_PORTS> s_port_connected = {true, true};
	// Set when the frontend assigns a port's device and cleared when the next input sample
	// carries it, so a disconnect and reconnect between two frames is not lost.
	std::array<bool, LibretroSession::INPUT_PORTS> s_port_device_changed = {};

	// The last frame of the current session; it has no pixels before the first frame.
	LibretroSession::Frame s_presented_frame;
	// The audio of the frames completed by the current retro_run.
	LibretroAudio::Samples s_frame_audio;
	// Presented while the session has not delivered a frame yet, at the initial geometry.
	std::array<u32, INITIAL_WIDTH * INITIAL_HEIGHT> s_black_frame = {};

	// The AV information the frontend has accepted for the current content. It starts from the
	// initial values with each content, and only a successful update changes it.
	retro_system_av_info s_accepted_av = {};
	// The last update the frontend rejected, so the same rejection is reported only once.
	retro_system_av_info s_rejected_av = {};
	bool s_update_rejected = false;
} // namespace

void retro_set_environment(retro_environment_t cb)
{
	// Frontends probe core information by calling this on the module again, which reaches
	// an already initialized core with an environment that only serves the probe.
	// RetroArch does this during a session. The core keeps the environment it was
	// initialized with until deinit.
	if (s_initialized)
		return;

	s_environment = cb;
	LibretroHost::SetEnvironment(cb);
	LibretroInput::DescribeControllers(cb);
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
	s_video_refresh = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
	s_audio_batch = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
	s_input_poll = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
	s_input_state = cb;
}

unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

void retro_get_system_info(retro_system_info* info)
{
	std::memset(info, 0, sizeof(*info));
	info->library_name = CORE_NAME;
	info->library_version = CORE_VERSION;
	info->valid_extensions = CONTENT_EXTENSIONS;
	info->need_fullpath = true;
	info->block_extract = true;
}

// Starts the AV information over for new content; the session reports its own values once
// it runs.
static void ResetOutputState()
{
	std::memset(&s_accepted_av, 0, sizeof(s_accepted_av));
	s_accepted_av.geometry.base_width = INITIAL_WIDTH;
	s_accepted_av.geometry.base_height = INITIAL_HEIGHT;
	s_accepted_av.geometry.max_width = LibretroSession::MAX_FRAME_WIDTH;
	s_accepted_av.geometry.max_height = LibretroSession::MAX_FRAME_HEIGHT;
	s_accepted_av.geometry.aspect_ratio = INITIAL_ASPECT_RATIO;
	s_accepted_av.timing.fps = NOMINAL_FPS;
	s_accepted_av.timing.sample_rate = SAMPLE_RATE;
	s_update_rejected = false;
}

void retro_get_system_av_info(retro_system_av_info* info)
{
	*info = s_accepted_av;
}

void retro_init(void)
{
	ResetOutputState();
	LibretroAudio::Install();
	s_initialized = true;
	LibretroHost::InitializeLogging();
	Console.WriteLnFmt("Core initialized: {}", CORE_VERSION);
	LibretroHost::DrainLogMessages();
}

void retro_deinit(void)
{
	LibretroSession::Shutdown();
	LibretroAudio::Uninstall();
	LibretroAudio::ResetQueue();
	// Assigning an empty vector keeps its capacity; exchanging releases the frame memory.
	std::vector<u32>().swap(s_presented_frame.pixels);
	std::vector<s16>().swap(s_frame_audio.data);
	LibretroHost::ReleaseFonts();
	Console.WriteLn("Core deinitialized");
	LibretroHost::ShutdownLogging();
	s_initialized = false;
}

bool retro_load_game(const retro_game_info* game)
{
	ResetOutputState();
	LibretroAudio::ResetQueue();

	Error error;
	VMBootParameters boot_params;
	if (!LibretroHost::PrepareContent(game ? game->path : nullptr, &boot_params, &error))
	{
		LibretroHost::ReportLoadFailure(error.GetDescription());
		return false;
	}

	retro_pixel_format pixel_format = RETRO_PIXEL_FORMAT_XRGB8888;
	if (!s_environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &pixel_format))
	{
		LibretroHost::ReportLoadFailure("The frontend does not support the XRGB8888 pixel format.");
		return false;
	}

	if (!LibretroSession::Start(std::move(boot_params), &error))
	{
		LibretroHost::ReportLoadFailure(error.GetDescription());
		return false;
	}

	// The session has parsed the content, so its control layout is known.
	LibretroInput::DescribeInputs(s_environment);

	LibretroHost::DrainLogMessages();
	return true;
}

bool retro_load_game_special(unsigned game_type, const retro_game_info* info, size_t num_info)
{
	return false;
}

void retro_unload_game(void)
{
	LibretroSession::Stop();
	s_presented_frame.pixels.clear();
	s_frame_audio.data.clear();
	LibretroAudio::ResetQueue();
	ResetOutputState();
	LibretroHost::DrainLogMessages();
}

void retro_reset(void)
{
	// A refused reset leaves the session, and what it presents, unchanged.
	if (LibretroSession::Reset())
	{
		// Nothing from before the reset is presented or played afterwards; until the reset VM
		// completes an image, the frontend sees a black frame.
		s_presented_frame.pixels.clear();
		s_frame_audio.data.clear();
		LibretroAudio::DiscardQueued();
	}
	LibretroHost::DrainLogMessages();
}

// Samples every RetroPad button of each port. The frontend requires one poll per run
// before any state query.
static LibretroSession::InputSnapshot SampleInput()
{
	LibretroSession::InputSnapshot input;
	s_input_poll();
	for (u32 port = 0; port < LibretroSession::INPUT_PORTS; port++)
	{
		input.connected[port] = s_port_connected[port];
		input.device_changed[port] = s_port_device_changed[port];
		s_port_device_changed[port] = false;
		if (!input.connected[port])
			continue;

		for (u32 button = 0; button <= RETRO_DEVICE_ID_JOYPAD_R3; button++)
		{
			if (s_input_state(port, RETRO_DEVICE_JOYPAD, 0, button))
				input.joypad_buttons[port] |= static_cast<u16>(1u << button);
		}
	}
	return input;
}

static bool IsSameGeometry(const retro_game_geometry& first, const retro_game_geometry& second)
{
	return first.base_width == second.base_width && first.base_height == second.base_height &&
		   first.aspect_ratio == second.aspect_ratio;
}

// The emulator's frame rates have single precision; finer differences are not changes.
static bool IsSameFrameRate(double first, double second)
{
	return static_cast<float>(first) == static_cast<float>(second);
}

// Tells the frontend about geometry and timing changes before it receives the output they
// apply to. Timing changes need the full AV information; size and aspect alone do not, as
// every frame stays within the maximum size reported from the start.
static void AnnounceOutputChanges()
{
	retro_system_av_info target = s_accepted_av;
	if (!s_presented_frame.pixels.empty())
	{
		target.geometry.base_width = s_presented_frame.width;
		target.geometry.base_height = s_presented_frame.height;
		target.geometry.aspect_ratio = s_presented_frame.aspect_ratio;
	}

	const double session_fps = LibretroSession::GetVsyncFrequency();
	if (session_fps > 0.0)
		target.timing.fps = session_fps;
	if (s_frame_audio.sample_rate > 0)
		target.timing.sample_rate = s_frame_audio.sample_rate;

	const bool timing_changed = !IsSameFrameRate(target.timing.fps, s_accepted_av.timing.fps) ||
								target.timing.sample_rate != s_accepted_av.timing.sample_rate;
	if (!timing_changed && IsSameGeometry(target.geometry, s_accepted_av.geometry))
		return;

	const bool accepted = timing_changed ? s_environment(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &target) :
										   s_environment(RETRO_ENVIRONMENT_SET_GEOMETRY, &target.geometry);
	if (accepted)
	{
		// A geometry update carries no timing, so only a full update changes the accepted rate.
		if (timing_changed)
			s_accepted_av = target;
		else
			s_accepted_av.geometry = target.geometry;
		s_update_rejected = false;
		return;
	}

	// The update is retried with the next frame; frames still carry their own size meanwhile.
	if (s_update_rejected && IsSameGeometry(target.geometry, s_rejected_av.geometry) &&
		IsSameFrameRate(target.timing.fps, s_rejected_av.timing.fps) &&
		target.timing.sample_rate == s_rejected_av.timing.sample_rate)
	{
		return;
	}

	if (timing_changed)
		Console.ErrorFmt("The frontend rejected the output change to {}x{} at {} Hz with {} Hz audio",
			target.geometry.base_width, target.geometry.base_height, target.timing.fps, target.timing.sample_rate);
	else
		Console.ErrorFmt("The frontend rejected the geometry change to {}x{} at aspect ratio {}",
			target.geometry.base_width, target.geometry.base_height, target.geometry.aspect_ratio);
	s_rejected_av = target;
	s_update_rejected = true;
}

void retro_run(void)
{
	// A paused VM completes the request without running; the frame it presents again has no
	// audio, and audio still queued from earlier frames waits until the VM runs again.
	const bool advanced = LibretroSession::RunFrame(SampleInput());

	// Without a new frame, the previous one of this session is presented again.
	LibretroSession::TakeFrame(&s_presented_frame);
	if (advanced)
		LibretroSession::TakeAudio(&s_frame_audio);
	AnnounceOutputChanges();
	if (s_presented_frame.pixels.empty())
		s_video_refresh(s_black_frame.data(), INITIAL_WIDTH, INITIAL_HEIGHT, INITIAL_WIDTH * sizeof(u32));
	else
		s_video_refresh(s_presented_frame.pixels.data(), s_presented_frame.width, s_presented_frame.height,
			s_presented_frame.pitch);
	if (advanced)
		LibretroAudio::Submit(s_audio_batch, s_frame_audio, static_cast<u32>(s_accepted_av.timing.sample_rate));
	LibretroHost::DrainLogMessages();
}

size_t retro_serialize_size(void)
{
	return 0;
}

bool retro_serialize(void* data, size_t size)
{
	return false;
}

bool retro_unserialize(const void* data, size_t size)
{
	return false;
}

void retro_cheat_reset(void)
{
}

void retro_cheat_set(unsigned index, bool enabled, const char* code)
{
	Console.Warning("Cheats are not supported");
}

unsigned retro_get_region(void)
{
	return RETRO_REGION_NTSC;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
	if (port >= LibretroSession::INPUT_PORTS)
		return;

	// Only devices the frontend reports are seen; a physical unplug it does not report reads
	// as released buttons on a still connected port.
	s_port_device_changed[port] = true;
	switch (device & RETRO_DEVICE_MASK)
	{
		case RETRO_DEVICE_NONE:
			s_port_connected[port] = false;
			break;

		case RETRO_DEVICE_JOYPAD:
			s_port_connected[port] = true;
			break;

		default:
			Console.WarningFmt("Port {} was given unsupported device {}; it is treated as disconnected", port + 1, device);
			s_port_connected[port] = false;
			break;
	}
}

void* retro_get_memory_data(unsigned id)
{
	return nullptr;
}

size_t retro_get_memory_size(unsigned id)
{
	return 0;
}
