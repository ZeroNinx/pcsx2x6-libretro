// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "libretro.h"

#include <string>
#include <vector>

/// The core options: stable keys and values, their English and Simplified Chinese
/// presentation, and registration through the newest interface the frontend reports.
namespace LibretroOptions
{
	inline constexpr const char* BIOS_KEY = "pcsx2x6_bios";
	inline constexpr const char* BIOS_AUTO = "auto";
	inline constexpr const char* REGION_BYPASS_KEY = "pcsx2x6_region_check_bypass";
	inline constexpr const char* VOLUME_KEY = "pcsx2x6_volume";
	inline constexpr const char* MTVU_KEY = "pcsx2x6_mtvu";
	inline constexpr const char* UPSCALE_KEY = "pcsx2x6_upscale";
	/// Reserved: registered once a renderer besides Vulkan passes acceptance.
	inline constexpr const char* RENDERER_KEY = "pcsx2x6_renderer";

	/// Output volume in percent when the option is unavailable.
	inline constexpr unsigned DEFAULT_VOLUME = 100;

	/// Largest internal resolution multiplier offered; 1 is the native resolution.
	inline constexpr unsigned MAX_UPSCALE_MULTIPLIER = 4;

	enum class MtvuSetting
	{
		Automatic,
		Enabled,
		Disabled,
	};

	/// Registers every option. `bios_images` are the BIOS filenames offered besides
	/// Automatic. Failures are expected while the frontend probes core information with
	/// an environment that rejects every call.
	void Register(retro_environment_t environment, const std::vector<std::string>& bios_images);

	/// Values read from the frontend, falling back to the defaults.
	std::string GetBios(retro_environment_t environment);
	bool GetRegionBypass(retro_environment_t environment);
	unsigned GetVolume(retro_environment_t environment);
	MtvuSetting GetMtvu(retro_environment_t environment);
	unsigned GetUpscaleMultiplier(retro_environment_t environment);

	/// Text in the frontend's language for a reload-required change made while content runs.
	const char* GetReloadNotice(retro_environment_t environment);
} // namespace LibretroOptions
