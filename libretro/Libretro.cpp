// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#include "LibretroHost.h"
#include "LibretroSession.h"
#include "libretro.h"

#include "common/Console.h"
#include "common/Error.h"

#include "svnrev.h"

#include <array>
#include <cstring>
#include <string>

namespace
{
	constexpr const char* CORE_NAME = "PCSX2x6";
	constexpr const char* CORE_VERSION = GIT_REV " (PCSX2x6 " PCSX2X6_BASE_REVISION ")";
	constexpr const char* CONTENT_EXTENSIONS = "acgame";

	constexpr unsigned NATIVE_WIDTH = 640;
	constexpr unsigned NATIVE_HEIGHT = 448;
	constexpr double NOMINAL_FPS = 59.94;
	constexpr double SAMPLE_RATE = 48000.0;

	retro_environment_t s_environment = nullptr;
	retro_video_refresh_t s_video_refresh = nullptr;

	// Presented until the core delivers emulated frames; it does not show VM output.
	std::array<u32, NATIVE_WIDTH * NATIVE_HEIGHT> s_placeholder_frame = {};
} // namespace

void retro_set_environment(retro_environment_t cb)
{
	s_environment = cb;
	LibretroHost::SetEnvironment(cb);
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
}

void retro_set_input_poll(retro_input_poll_t cb)
{
}

void retro_set_input_state(retro_input_state_t cb)
{
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

void retro_get_system_av_info(retro_system_av_info* info)
{
	std::memset(info, 0, sizeof(*info));
	info->geometry.base_width = NATIVE_WIDTH;
	info->geometry.base_height = NATIVE_HEIGHT;
	info->geometry.max_width = NATIVE_WIDTH;
	info->geometry.max_height = NATIVE_HEIGHT;
	info->geometry.aspect_ratio = 4.0f / 3.0f;
	info->timing.fps = NOMINAL_FPS;
	info->timing.sample_rate = SAMPLE_RATE;
}

void retro_init(void)
{
	LibretroHost::InitializeLogging();
	Console.WriteLnFmt("Core initialized: {}", CORE_VERSION);
	LibretroHost::DrainLogMessages();
}

void retro_deinit(void)
{
	LibretroSession::Shutdown();
	LibretroHost::ReleaseFonts();
	Console.WriteLn("Core deinitialized");
	LibretroHost::ShutdownLogging();
}

bool retro_load_game(const retro_game_info* game)
{
	Error error;
	std::string boot_filename;
	if (!LibretroHost::PrepareContent(game ? game->path : nullptr, &boot_filename, &error))
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

	if (!LibretroSession::Start(std::move(boot_filename), &error))
	{
		LibretroHost::ReportLoadFailure(error.GetDescription());
		return false;
	}

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
	LibretroHost::DrainLogMessages();
}

void retro_reset(void)
{
}

void retro_run(void)
{
	LibretroSession::RunFrame();
	s_video_refresh(s_placeholder_frame.data(), NATIVE_WIDTH, NATIVE_HEIGHT, NATIVE_WIDTH * sizeof(u32));
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
}

void* retro_get_memory_data(unsigned id)
{
	return nullptr;
}

size_t retro_get_memory_size(unsigned id)
{
	return 0;
}
