// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#include "LibretroOptions.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace
{
	// One text per presentation language. Keys, stored values, BIOS filenames and patch
	// names are never translated.
	struct Text
	{
		const char* english;
		const char* chinese;
	};

	struct ValueText
	{
		const char* value;
		Text label;
	};

	constexpr const char* CATEGORY_SYSTEM = "system";
	constexpr const char* CATEGORY_VIDEO = "video";
	constexpr const char* CATEGORY_AUDIO = "audio";

	struct CategoryText
	{
		const char* key;
		Text desc;
		Text info;
	};

	constexpr std::array<CategoryText, 3> CATEGORIES = {{
		{CATEGORY_SYSTEM, {"System", "系统"},
			{"BIOS image, compatibility patches and emulation threads.", "BIOS 镜像、兼容性补丁与模拟线程。"}},
		{CATEGORY_VIDEO, {"Video", "视频"}, {"Renderer and internal resolution.", "渲染器与内部分辨率。"}},
		{CATEGORY_AUDIO, {"Audio", "音频"}, {"Output volume.", "输出音量。"}},
	}};

	constexpr Text BIOS_DESC = {"BIOS Image", "BIOS 镜像"};
	constexpr Text BIOS_INFO = {
		"Automatic selects the first valid COH-H arcade BIOS in filename order from system/pcsx2x6/bios. Select a specific image to override it. Close and reload content after changing this option.",
		"“自动”按文件名顺序选用 system/pcsx2x6/bios 中第一个有效的 COH-H 街机 BIOS。选择具体镜像可覆盖自动选择。修改后需关闭并重新加载内容。"};
	constexpr Text BIOS_AUTO_LABEL = {"Automatic", "自动"};

	constexpr Text REGION_DESC = {"Region Check Bypass", "区域检查绕过"};
	constexpr Text REGION_INFO = {
		"Enable the matching Region check bypass patch from system/pcsx2x6/resources/patches.zip. The archive must be supplied separately. Close and reload content after changing this option.",
		"启用 system/pcsx2x6/resources/patches.zip 中对应的区域检查绕过补丁。该压缩包需另行提供。修改后需关闭并重新加载内容。"};
	constexpr std::array<ValueText, 2> ENABLED_DISABLED = {{
		{"enabled", {"Enabled", "开启"}},
		{"disabled", {"Disabled", "关闭"}},
	}};

	constexpr Text VOLUME_DESC = {"Volume", "音量"};
	constexpr Text VOLUME_INFO = {
		"Output volume in percent. Applies immediately.",
		"输出音量（百分比）。修改后立即生效。"};

	constexpr Text MTVU_DESC = {"VU1 Thread (MTVU)", "VU1 独立线程（MTVU）"};
	constexpr Text MTVU_INFO = {
		"Runs the VU1 vector unit on its own thread. Automatic leaves the choice to the emulator, which enables it on processors with at least three cores. Close and reload content after changing this option.",
		"在单独的线程上运行 VU1 向量单元。“自动”由模拟器决定，处理器不少于三个核心时开启。修改后需关闭并重新加载内容。"};
	constexpr std::array<ValueText, 3> MTVU_VALUES = {{
		{"auto", {"Automatic", "自动"}},
		{"enabled", {"Enabled", "开启"}},
		{"disabled", {"Disabled", "关闭"}},
	}};

	constexpr Text UPSCALE_DESC = {"Internal Resolution", "内部分辨率"};
	constexpr Text UPSCALE_INFO = {
		"Renders the GS at a multiple of the native resolution. Higher multipliers need more video memory. Close and reload content after changing this option.",
		"以原生分辨率的整数倍渲染 GS。倍率越高，所需显存越多。修改后需关闭并重新加载内容。"};
	constexpr std::array<ValueText, LibretroOptions::MAX_UPSCALE_MULTIPLIER> UPSCALE_VALUES = {{
		{"1", {"Native", "原生"}},
		{"2", {"2x", "2 倍"}},
		{"3", {"3x", "3 倍"}},
		{"4", {"4x", "4 倍"}},
	}};

	constexpr Text RELOAD_NOTICE = {
		"Close and reload content to apply the changed core options.",
		"已修改的核心选项需关闭并重新加载内容后生效。"};

	constexpr Text RENDERER_DESC = {"Renderer", "渲染器"};
	constexpr Text RENDERER_INFO = {
		"Graphics API used by the GS. The frontend switches to the matching video driver. Close and reload content after changing this option.",
		"GS 使用的图形接口。前端会切换到对应的视频驱动。修改后需关闭并重新加载内容。"};
	// In the order of LibretroOptions::Renderer.
	constexpr std::array<ValueText, 2> RENDERER_VALUES = {{
		{"vulkan", {"Vulkan", "Vulkan"}},
		{"d3d11", {"Direct3D 11", "Direct3D 11"}},
	}};

	constexpr unsigned VOLUME_STEP = 10;
	constexpr unsigned VOLUME_MAX = 200;
	constexpr size_t VOLUME_VALUE_COUNT = VOLUME_MAX / VOLUME_STEP + 1;
	constexpr size_t OPTION_COUNT = 6;

	enum class Language
	{
		English,
		Chinese,
	};

	const char* Pick(const Text& text, Language language)
	{
		return language == Language::Chinese ? text.chinese : text.english;
	}

	// The frontend keeps its own copy of what is registered, but the strings must stay valid
	// for the duration of the call; these outlive it.
	std::vector<std::string> s_bios_values;
	std::array<std::string, VOLUME_VALUE_COUNT> s_volume_values;
	std::array<retro_core_option_v2_category, CATEGORIES.size() + 1> s_categories[2];
	std::array<retro_core_option_v2_definition, OPTION_COUNT + 1> s_definitions_v2[2];
	std::array<retro_core_option_definition, OPTION_COUNT + 1> s_definitions_v1[2];
	std::array<retro_variable, OPTION_COUNT + 1> s_variables;
	std::array<std::string, OPTION_COUNT> s_variable_text;

	void SetValues(retro_core_option_value* out, const ValueText* values, size_t count, Language language)
	{
		for (size_t i = 0; i < count; i++)
			out[i] = {values[i].value, Pick(values[i].label, language)};
	}

	void BuildV2(Language language)
	{
		const size_t index = static_cast<size_t>(language);
		auto& categories = s_categories[index];
		categories = {};
		for (size_t i = 0; i < CATEGORIES.size(); i++)
			categories[i] = {CATEGORIES[i].key, Pick(CATEGORIES[i].desc, language), Pick(CATEGORIES[i].info, language)};

		auto& definitions = s_definitions_v2[index];
		definitions = {};

		retro_core_option_v2_definition& bios = definitions[0];
		bios.key = LibretroOptions::BIOS_KEY;
		bios.desc = Pick(BIOS_DESC, language);
		bios.info = Pick(BIOS_INFO, language);
		bios.category_key = CATEGORY_SYSTEM;
		bios.default_value = LibretroOptions::BIOS_AUTO;
		bios.values[0] = {LibretroOptions::BIOS_AUTO, Pick(BIOS_AUTO_LABEL, language)};
		for (size_t i = 0; i < s_bios_values.size(); i++)
			bios.values[i + 1] = {s_bios_values[i].c_str(), nullptr};

		retro_core_option_v2_definition& region = definitions[1];
		region.key = LibretroOptions::REGION_BYPASS_KEY;
		region.desc = Pick(REGION_DESC, language);
		region.info = Pick(REGION_INFO, language);
		region.category_key = CATEGORY_SYSTEM;
		region.default_value = "enabled";
		SetValues(region.values, ENABLED_DISABLED.data(), ENABLED_DISABLED.size(), language);

		retro_core_option_v2_definition& mtvu = definitions[2];
		mtvu.key = LibretroOptions::MTVU_KEY;
		mtvu.desc = Pick(MTVU_DESC, language);
		mtvu.info = Pick(MTVU_INFO, language);
		mtvu.category_key = CATEGORY_SYSTEM;
		mtvu.default_value = "auto";
		SetValues(mtvu.values, MTVU_VALUES.data(), MTVU_VALUES.size(), language);

		retro_core_option_v2_definition& renderer = definitions[3];
		renderer.key = LibretroOptions::RENDERER_KEY;
		renderer.desc = Pick(RENDERER_DESC, language);
		renderer.info = Pick(RENDERER_INFO, language);
		renderer.category_key = CATEGORY_VIDEO;
		renderer.default_value = RENDERER_VALUES[0].value;
		SetValues(renderer.values, RENDERER_VALUES.data(), RENDERER_VALUES.size(), language);

		retro_core_option_v2_definition& upscale = definitions[4];
		upscale.key = LibretroOptions::UPSCALE_KEY;
		upscale.desc = Pick(UPSCALE_DESC, language);
		upscale.info = Pick(UPSCALE_INFO, language);
		upscale.category_key = CATEGORY_VIDEO;
		upscale.default_value = UPSCALE_VALUES[0].value;
		SetValues(upscale.values, UPSCALE_VALUES.data(), UPSCALE_VALUES.size(), language);

		retro_core_option_v2_definition& volume = definitions[5];
		volume.key = LibretroOptions::VOLUME_KEY;
		volume.desc = Pick(VOLUME_DESC, language);
		volume.info = Pick(VOLUME_INFO, language);
		volume.category_key = CATEGORY_AUDIO;
		volume.default_value = "100";
		for (size_t i = 0; i < VOLUME_VALUE_COUNT; i++)
			volume.values[i] = {s_volume_values[i].c_str(), nullptr};
	}

	// The v1 interface has no categories; the text is otherwise the same.
	void BuildV1(Language language)
	{
		const size_t index = static_cast<size_t>(language);
		auto& definitions = s_definitions_v1[index];
		definitions = {};
		for (size_t i = 0; i < OPTION_COUNT; i++)
		{
			const retro_core_option_v2_definition& source = s_definitions_v2[index][i];
			retro_core_option_definition& target = definitions[i];
			target.key = source.key;
			target.desc = source.desc;
			target.info = source.info;
			std::memcpy(target.values, source.values, sizeof(target.values));
			target.default_value = source.default_value;
		}
	}

	// The legacy interface: "Description; default|other|values", default first, English only.
	void BuildVariables()
	{
		s_variables = {};
		for (size_t i = 0; i < OPTION_COUNT; i++)
		{
			const retro_core_option_v2_definition& source = s_definitions_v2[0][i];
			std::string& text = s_variable_text[i];
			text = std::string(source.desc) + "; " + source.default_value;
			for (const retro_core_option_value& value : source.values)
			{
				if (!value.value)
					break;
				if (std::strcmp(value.value, source.default_value) != 0)
					text += std::string("|") + value.value;
			}
			s_variables[i] = {source.key, text.c_str()};
		}
	}

	Language GetLanguage(retro_environment_t environment)
	{
		unsigned language = RETRO_LANGUAGE_ENGLISH;
		if (environment(RETRO_ENVIRONMENT_GET_LANGUAGE, &language) && language == RETRO_LANGUAGE_CHINESE_SIMPLIFIED)
			return Language::Chinese;
		return Language::English;
	}

	const char* GetValue(retro_environment_t environment, const char* key)
	{
		retro_variable variable{key, nullptr};
		return (environment(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) && variable.value) ? variable.value : nullptr;
	}
} // namespace

void LibretroOptions::Register(retro_environment_t environment, const std::vector<std::string>& bios_images)
{
	// One value slot is Automatic and the last stays empty as the list terminator.
	const size_t bios_slots = RETRO_NUM_CORE_OPTION_VALUES_MAX - 2;
	s_bios_values.assign(bios_images.begin(), bios_images.begin() + std::min(bios_images.size(), bios_slots));
	for (size_t i = 0; i < VOLUME_VALUE_COUNT; i++)
		s_volume_values[i] = std::to_string(i * VOLUME_STEP);

	BuildV2(Language::English);
	BuildV2(Language::Chinese);

	// The interface follows the version the frontend reports, never the result of a
	// registration call: a v2 call returns false when only categories are unsupported,
	// although the options were registered.
	unsigned version = 0;
	if (!environment(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version))
		version = 0;

	const bool chinese = GetLanguage(environment) == Language::Chinese;
	if (version >= 2)
	{
		retro_core_options_v2 us{s_categories[0].data(), s_definitions_v2[0].data()};
		retro_core_options_v2 local{s_categories[1].data(), s_definitions_v2[1].data()};
		retro_core_options_v2_intl intl{&us, chinese ? &local : nullptr};
		environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL, &intl);
	}
	else if (version == 1)
	{
		BuildV1(Language::English);
		BuildV1(Language::Chinese);
		retro_core_options_intl intl{s_definitions_v1[0].data(), chinese ? s_definitions_v1[1].data() : nullptr};
		environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL, &intl);
	}
	else
	{
		BuildVariables();
		environment(RETRO_ENVIRONMENT_SET_VARIABLES, s_variables.data());
	}
}

std::string LibretroOptions::GetBios(retro_environment_t environment)
{
	const char* value = GetValue(environment, BIOS_KEY);
	return value ? value : BIOS_AUTO;
}

bool LibretroOptions::GetRegionBypass(retro_environment_t environment)
{
	const char* value = GetValue(environment, REGION_BYPASS_KEY);
	return !value || std::strcmp(value, "disabled") != 0;
}

unsigned LibretroOptions::GetVolume(retro_environment_t environment)
{
	const char* value = GetValue(environment, VOLUME_KEY);
	if (!value)
		return DEFAULT_VOLUME;

	// Only the stored values offered by the option are accepted; anything else, such as a
	// hand-edited "55" or " 50", falls back to the default.
	for (size_t i = 0; i < VOLUME_VALUE_COUNT; i++)
	{
		if (std::to_string(i * VOLUME_STEP) == value)
			return static_cast<unsigned>(i * VOLUME_STEP);
	}
	return DEFAULT_VOLUME;
}

LibretroOptions::MtvuSetting LibretroOptions::GetMtvu(retro_environment_t environment)
{
	const char* value = GetValue(environment, MTVU_KEY);
	if (value && std::strcmp(value, "enabled") == 0)
		return MtvuSetting::Enabled;
	if (value && std::strcmp(value, "disabled") == 0)
		return MtvuSetting::Disabled;
	return MtvuSetting::Automatic;
}

LibretroOptions::Renderer LibretroOptions::GetRenderer(retro_environment_t environment)
{
	// Only the offered stored values are accepted; anything else uses Vulkan.
	if (const char* value = GetValue(environment, RENDERER_KEY))
	{
		for (size_t i = 0; i < RENDERER_VALUES.size(); i++)
		{
			if (std::strcmp(RENDERER_VALUES[i].value, value) == 0)
				return static_cast<Renderer>(i);
		}
	}
	return Renderer::Vulkan;
}

unsigned LibretroOptions::GetUpscaleMultiplier(retro_environment_t environment)
{
	// Only the offered stored values are accepted; anything else renders at native resolution.
	if (const char* value = GetValue(environment, UPSCALE_KEY))
	{
		for (size_t i = 0; i < UPSCALE_VALUES.size(); i++)
		{
			if (std::strcmp(UPSCALE_VALUES[i].value, value) == 0)
				return static_cast<unsigned>(i + 1);
		}
	}
	return 1;
}

const char* LibretroOptions::GetReloadNotice(retro_environment_t environment)
{
	return Pick(RELOAD_NOTICE, GetLanguage(environment));
}
