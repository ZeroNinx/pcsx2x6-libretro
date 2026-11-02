// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#include "LibretroAudio.h"
#include "LibretroHost.h"
#include "LibretroInput.h"
#include "LibretroSession.h"
#include "LibretroVulkan.h"
#include "libretro.h"

#include "VMManager.h"

#include "common/Console.h"
#include "common/Error.h"

#include "svnrev.h"

#include <array>
#include <cstring>
#include <string>
#include <utility>
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

	// The last frame of the current session; its size is 0 before the first frame.
	LibretroSession::Frame s_presented_frame;
	// The audio of the frames completed by the current retro_run.
	LibretroAudio::Samples s_frame_audio;

	// The AV information the frontend has accepted for the current content. It starts from the
	// initial values with each content, and only a successful update changes it.
	retro_system_av_info s_accepted_av = {};
	// The last update the frontend rejected, so the same rejection is reported only once.
	retro_system_av_info s_rejected_av = {};
	bool s_update_rejected = false;

	// Whether the frontend repeats the previous frame when given none.
	bool s_can_dupe = false;
	bool s_missing_dupe_reported = false;

	// Set when the frontend's context goes away while the VM runs. RetroArch also destroys its
	// context right before retro_unload_game, so only a later retro_run, or a context_reset
	// within the same session, reveals a lost session.
	bool s_context_lost = false;

	// Set once a failure of the loaded content has been reported, so that one failure is
	// reported once however many callbacks observe it.
	bool s_session_failed = false;
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
	// Assigning an empty vector keeps its capacity; exchanging releases the audio memory.
	std::vector<s16>().swap(s_frame_audio.data);
	LibretroHost::ReleaseFonts();
	Console.WriteLn("Core deinitialized");
	LibretroHost::ShutdownLogging();
	s_initialized = false;
}

// Frontend thread, from context_reset: boots the prepared VM on the frontend's Vulkan context.
static void ContextFailed(const char* reason);
static bool ContextReady()
{
	// A context created again after the session lost its context cannot boot the VM again: the
	// boot parameters were consumed when it first started.
	if (s_context_lost)
	{
		ContextFailed(nullptr);
		return false;
	}

	Error error;
	if (!LibretroSession::Start(&error))
	{
		ContextFailed(error.GetDescription().c_str());
		return false;
	}

	// The session has parsed the content, so its control layout is known.
	LibretroInput::DescribeInputs(s_environment);
	LibretroHost::DrainLogMessages();
	return true;
}

// Frontend thread: the content cannot run, although retro_load_game already succeeded. The
// failure is shown once and the frontend is asked to end the session; nothing runs meanwhile.
// A lost context explains whatever follows from it, so it is reported instead of `reason`.
static void ContextFailed(const char* reason)
{
	LibretroSession::Stop();
	LibretroSession::CloseGS();
	if (!std::exchange(s_session_failed, true))
	{
		LibretroHost::ReportLoadFailure(s_context_lost ?
				"The frontend's Vulkan context was lost; the content has to be loaded again." : reason);
		s_environment(RETRO_ENVIRONMENT_SHUTDOWN, nullptr);
	}
	s_context_lost = false;
	LibretroHost::DrainLogMessages();
}

// Frontend thread: the frontend's context is going away; nothing may still run on its device.
static void ContextLost()
{
	s_context_lost = LibretroSession::IsActive();
	LibretroSession::Stop();
	LibretroSession::CloseGS();
	LibretroHost::DrainLogMessages();
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

	// The GS opens when the frontend creates the Vulkan device, and the VM boots once the
	// frontend's context is ready, both after this returns.
	if (!LibretroSession::Prepare(std::move(boot_params), &error))
	{
		LibretroHost::ReportLoadFailure(error.GetDescription());
		return false;
	}

	if (!LibretroVulkan::Register(s_environment, {&ContextReady, &ContextFailed, &ContextLost}))
	{
		LibretroSession::Stop();
		LibretroHost::ReportLoadFailure("This core needs a frontend Vulkan video driver with context negotiation v2.");
		return false;
	}

	s_can_dupe = false;
	if (!s_environment(RETRO_ENVIRONMENT_GET_CAN_DUPE, &s_can_dupe))
		s_can_dupe = false;
	s_missing_dupe_reported = false;
	s_context_lost = false;
	s_session_failed = false;

	LibretroHost::DrainLogMessages();
	return true;
}

bool retro_load_game_special(unsigned game_type, const retro_game_info* info, size_t num_info)
{
	return false;
}

void retro_unload_game(void)
{
	s_context_lost = false;
	s_session_failed = false;
	LibretroSession::Stop();
	LibretroSession::CloseGS();
	s_presented_frame = {};
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
		s_presented_frame = {};
		LibretroVulkan::ClearOutput();
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
	if (s_presented_frame.width != 0)
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

// Hands the latest frame to the frontend. Without a new one, the frontend repeats the previous
// image, or is given the previous image again when it cannot repeat frames itself.
static void PresentFrame(bool new_frame)
{
	if (new_frame)
	{
		LibretroVulkan::SetOutputImage(s_presented_frame.slot);
		s_video_refresh(RETRO_HW_FRAME_BUFFER_VALID, s_presented_frame.width, s_presented_frame.height, 0);
	}
	else if (s_can_dupe)
	{
		s_video_refresh(nullptr, s_presented_frame.width, s_presented_frame.height, 0);
	}
	else if (s_presented_frame.width != 0)
	{
		LibretroVulkan::SetOutputImage(s_presented_frame.slot);
		s_video_refresh(RETRO_HW_FRAME_BUFFER_VALID, s_presented_frame.width, s_presented_frame.height, 0);
	}
	else if (!std::exchange(s_missing_dupe_reported, true))
	{
		Console.Error("The frontend cannot repeat frames, and there is no image to present yet");
	}
}

void retro_run(void)
{
	// After a reported failure nothing runs until the frontend ends the session.
	if (s_session_failed)
		return;

	// The VM stopped with the frontend's context, and the frontend still runs the content.
	if (s_context_lost)
	{
		ContextFailed(nullptr);
		return;
	}

	if (LibretroVulkan::OutputFailed())
	{
		ContextFailed("An output image could not be created on the frontend's Vulkan device.");
		return;
	}

	// The frame is drawn into the image of the frontend's current sync index. A paused VM
	// completes the request without running; the frame it presents again has no audio, and
	// audio still queued from earlier frames waits until the VM runs again.
	const u32 output_slot = LibretroVulkan::AcquireOutputSlot();
	const bool advanced = LibretroSession::RunFrame(SampleInput(), output_slot);

	bool new_frame = LibretroSession::TakeFrame(&s_presented_frame);

	// Nothing has been shown since the content started or was reset, and the VM drew nothing,
	// for instance while it is paused: black is shown instead.
	if (!new_frame && s_presented_frame.width == 0 && !LibretroVulkan::OutputFailed())
		new_frame = LibretroSession::DrawBlackFrame(output_slot, &s_presented_frame);

	// An output image that could not be prepared ends the session before anything is presented.
	if (LibretroVulkan::OutputFailed())
	{
		ContextFailed("An output image could not be created on the frontend's Vulkan device.");
		return;
	}

	if (advanced)
		LibretroSession::TakeAudio(&s_frame_audio);
	AnnounceOutputChanges();

	// An AV update can make the frontend recreate its video driver within this call, which
	// destroys the context the frame was drawn on; nothing of the frame is delivered then.
	if (s_context_lost || s_session_failed)
	{
		if (s_context_lost)
			ContextFailed(nullptr);
		return;
	}

	PresentFrame(new_frame);
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
