// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#include "LibretroHost.h"
#include "LibretroOptions.h"
#include "LibretroSession.h"

#include "Achievements.h"
#include "Config.h"
#include "GS.h"
#include "GameList.h"
#include "Host.h"
#include "ImGui/FullscreenUI.h"
#include "ImGui/ImGuiFullscreen.h"
#include "ImGui/ImGuiManager.h"
#include "Input/InputManager.h"
#include "MTGS.h"
#include "Patch.h"
#include "SPU2/spu2.h"
#include "VMManager.h"
#include "ps2/BiosTools.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/SmallString.h"
#include "common/StringUtil.h"
#include "common/WindowInfo.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{
	constexpr const char* DIRECTORY_NAME = "pcsx2x6";
	constexpr const char* CONTENT_EXTENSION = "acgame";
	constexpr const char* ARCADE_SAVE_DIRECTORY_NAME = "arcade";
	constexpr const char* REGION_BYPASS_PATCH_NAME = "Region check bypass";

	constexpr size_t MAX_QUEUED_LOG_MESSAGES = 4096;
	constexpr unsigned ERROR_MESSAGE_FRAMES = 300;
	constexpr unsigned NOTICE_FRAMES = 300;
	constexpr unsigned NOTICE_DURATION_MS = 5000;

	struct QueuedLogMessage
	{
		retro_log_level level;
		std::string text;
	};

	retro_environment_t s_environment = nullptr;
	retro_log_printf_t s_frontend_log = nullptr;

	std::mutex s_log_mutex;
	std::deque<QueuedLogMessage> s_log_queue;
	u32 s_dropped_log_messages = 0;

	// Outlives every session because the emulator keeps a pointer to it.
	MemorySettingsInterface s_settings;

	// ImGui references the font data without copying it, so the mapping lasts until
	// the GS has shut down.
	std::span<const u8> s_font_data;

	// The reload-required options the current content was loaded with.
	struct LoadedOptions
	{
		std::string bios;
		bool region_bypass = true;
		LibretroOptions::MtvuSetting mtvu = LibretroOptions::MtvuSetting::Automatic;
		unsigned upscale_multiplier = 1;
		LibretroOptions::Renderer renderer = LibretroOptions::Renderer::Vulkan;
	};
	LoadedOptions s_loaded_options;
	unsigned s_applied_volume = LibretroOptions::DEFAULT_VOLUME;
	// The manifest of the content prepared last.
	std::string s_content_path;
	bool s_reload_notice_shown = false;
} // namespace

// Console.Error() and Console.Warning() log at LOGLEVEL_INFO and carry their
// severity in the colour alone.
static retro_log_level GetFrontendLogLevel(LOGLEVEL level, ConsoleColors color)
{
	if (level == LOGLEVEL_ERROR || color == Color_StrongRed)
		return RETRO_LOG_ERROR;
	if (level == LOGLEVEL_WARNING || color == Color_StrongOrange)
		return RETRO_LOG_WARN;
	return (level == LOGLEVEL_INFO) ? RETRO_LOG_INFO : RETRO_LOG_DEBUG;
}

// Runs on any emulator thread, so nothing here may call back into the frontend.
static void QueueLogMessage(LOGLEVEL level, ConsoleColors color, std::string_view message)
{
	std::unique_lock lock(s_log_mutex);
	if (s_log_queue.size() >= MAX_QUEUED_LOG_MESSAGES)
	{
		s_dropped_log_messages++;
		return;
	}

	s_log_queue.push_back({GetFrontendLogLevel(level, color), std::string(message)});
}

static void WriteFrontendLog(retro_log_level level, std::string_view message)
{
	const int length = static_cast<int>(message.size());
	if (s_frontend_log)
		s_frontend_log(level, "[PCSX2x6] %.*s\n", length, message.data());
	else
		std::fprintf(stderr, "[PCSX2x6] %.*s\n", length, message.data());
}

void LibretroHost::DrainLogMessages()
{
	std::deque<QueuedLogMessage> messages;
	u32 dropped_messages;
	{
		std::unique_lock lock(s_log_mutex);
		messages.swap(s_log_queue);
		dropped_messages = std::exchange(s_dropped_log_messages, 0);
	}

	for (const QueuedLogMessage& message : messages)
		WriteFrontendLog(message.level, message.text);

	if (dropped_messages > 0)
		WriteFrontendLog(RETRO_LOG_WARN, fmt::format("Dropped {} log messages", dropped_messages));
}

void LibretroHost::InitializeLogging()
{
	Log::SetHostOutputLevel(LOGLEVEL_INFO, &QueueLogMessage);
}

void LibretroHost::ShutdownLogging()
{
	DrainLogMessages();
	Log::SetHostOutputLevel(LOGLEVEL_NONE, nullptr);
}

void LibretroHost::ReportLoadFailure(std::string_view message)
{
	Console.ErrorFmt("Content load failed: {}", message);
	DrainLogMessages();

	const std::string text(message);
	retro_message frontend_message{text.c_str(), ERROR_MESSAGE_FRAMES};
	s_environment(RETRO_ENVIRONMENT_SET_MESSAGE, &frontend_message);
}

// Frontend thread: shows a notification. The extended interface carries a priority and a
// type; frontends without it show the plain message.
static void ShowNotice(const char* text)
{
	unsigned message_version = 0;
	if (s_environment(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &message_version) && message_version >= 1)
	{
		retro_message_ext message{};
		message.msg = text;
		message.duration = NOTICE_DURATION_MS;
		message.priority = 1;
		message.level = RETRO_LOG_INFO;
		message.target = RETRO_MESSAGE_TARGET_ALL;
		message.type = RETRO_MESSAGE_TYPE_NOTIFICATION;
		message.progress = -1;
		s_environment(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &message);
		return;
	}

	retro_message message{text, NOTICE_FRAMES};
	s_environment(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
}

// CPU thread: applies an output volume without reloading the settings, which would reset
// running state such as the Test DIP switch. Both SPU2 volumes carry the one core volume;
// SPU2 keeps a muted output muted and applies the new value when it unmutes.
static void ApplyVolume(unsigned volume)
{
	{
		const std::unique_lock lock = Host::GetSettingsLock();
		s_settings.SetUIntValue("SPU2/Output", "StandardVolume", volume);
		s_settings.SetUIntValue("SPU2/Output", "FastForwardVolume", volume);
	}

	if (!VMManager::HasValidVM())
		return;

	const Pcsx2Config old_config(EmuConfig);
	EmuConfig.SPU2.StandardVolume = volume;
	EmuConfig.SPU2.FastForwardVolume = volume;
	SPU2::CheckForConfigChanges(old_config);
	Console.WriteLnFmt("Output volume set to {}%", volume);
}

// A frontend may report no directory, or a relative one that would resolve against
// its own working directory. Neither is a safe place for BIOS data or arcade saves.
static bool GetFrontendDirectory(unsigned command, const char* description, std::string* directory, Error* error)
{
	const char* path = nullptr;
	if (!s_environment(command, &path) || !path || path[0] == '\0')
	{
		Error::SetStringFmt(error, "The frontend did not provide a {} directory.", description);
		return false;
	}

	if (!Path::IsAbsolute(path))
	{
		Error::SetStringFmt(error, "The frontend {} directory '{}' is not an absolute path.", description, path);
		return false;
	}

	const std::string native_path = Path::Canonicalize(Path::ToNativePath(path));
	// RetroArch can already include the core name, optionally followed by a content
	// directory. Never escape the directory supplied by the frontend to flatten it.
	const bool core_save_directory = command == RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY &&
		(StringUtil::compareNoCase(Path::GetFileName(native_path), DIRECTORY_NAME) ||
			StringUtil::compareNoCase(Path::GetFileName(Path::GetDirectory(native_path)), DIRECTORY_NAME));
	*directory = core_save_directory ? native_path : Path::Combine(native_path, DIRECTORY_NAME);
	return true;
}

static std::string GetSourceBiosDirectory(const std::string& system_directory)
{
	return Path::Combine(system_directory, "bios");
}

static bool IsArcadeBios(const std::string& path)
{
	u32 version, region;
	std::string description, zone;
	return IsBIOS(path.c_str(), version, description, region, zone) && zone == "COH-H";
}

static std::vector<std::string> FindArcadeBiosImages(const std::string& directory)
{
	std::vector<std::string> images;
	FileSystem::FindResultsArray files;
	FileSystem::FindFiles(directory.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_SORT_BY_NAME, &files);
	for (const FILESYSTEM_FIND_DATA& file : files)
	{
		if (IsArcadeBios(file.FileName))
			images.emplace_back(Path::GetFileName(file.FileName));
	}
	return images;
}

// Frontends also call this while probing core information, with an environment that
// rejects everything. BIOS discovery is repeated when loading content.
static void RegisterCoreOptions()
{
	std::vector<std::string> bios_images;
	std::string system_directory;
	if (GetFrontendDirectory(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, "system", &system_directory, nullptr))
		bios_images = FindArcadeBiosImages(GetSourceBiosDirectory(system_directory));

	LibretroOptions::Register(s_environment, bios_images);
}

void LibretroHost::SetEnvironment(retro_environment_t environment)
{
	s_environment = environment;

	retro_log_callback log_callback{};
	s_frontend_log = environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log_callback) ? log_callback.log : nullptr;

	RegisterCoreOptions();
}

static bool ResolveBios(const std::string& source_directory, std::string* bios_name, Error* error)
{
	*bios_name = s_loaded_options.bios;
	// Treat the previous default as Automatic when upgrading an existing configuration.
	if (bios_name->empty() || *bios_name == LibretroOptions::BIOS_AUTO || *bios_name == "none")
	{
		const std::vector<std::string> images = FindArcadeBiosImages(source_directory);
		if (images.empty())
		{
			Error::SetStringFmt(error, "No valid COH-H arcade BIOS was found in '{}'. Add your arcade BIOS dump and its companion files to this folder.", source_directory);
			return false;
		}
		*bios_name = images.front();
		Console.WriteLnFmt("Automatically selected arcade BIOS '{}'", *bios_name);
		return true;
	}

	if (bios_name->find_first_of("/\\:") != std::string::npos || *bios_name == "." || *bios_name == "..")
	{
		Error::SetString(error, "The BIOS option must be a filename within system/pcsx2x6/bios.");
		return false;
	}
	const std::string path = Path::Combine(source_directory, *bios_name);
	if (!IsArcadeBios(path))
	{
		Error::SetStringFmt(error, "The selected BIOS '{}' is missing or is not a valid COH-H arcade BIOS. Select Automatic or another arcade BIOS.", path);
		return false;
	}
	return true;
}

static bool CheckRuntimeResources(Error* error)
{
	static constexpr const char* required_resources[] = {
		"GameIndex.yaml",
		"fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf",
	};

	for (const char* resource : required_resources)
	{
		const std::string path = Path::Combine(EmuFolders::Resources, resource);
		if (!FileSystem::FileExists(path.c_str()))
		{
			Error::SetStringFmt(error, "Required resource '{}' is missing.", path);
			return false;
		}
	}

	const std::string shaders = Path::Combine(EmuFolders::Resources, "shaders");
	if (!FileSystem::DirectoryExists(shaders.c_str()))
	{
		Error::SetStringFmt(error, "Required resource directory '{}' is missing.", shaders);
		return false;
	}

	return true;
}

// Only called without an active session: the GS, and with it ImGui, is closed.
static bool LoadFonts(Error* error)
{
	if (!s_font_data.empty())
		return true;

	const std::string path = Path::Combine(EmuFolders::Resources, "fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
	s_font_data = FileSystem::MapBinaryFileForRead(path.c_str());
	if (s_font_data.empty())
	{
		Error::SetStringFmt(error, "Failed to load font '{}'.", path);
		return false;
	}

	ImGuiManager::FontInfo font{};
	font.data = s_font_data;
	ImGuiManager::SetFonts({font});
	return true;
}

void LibretroHost::ReleaseFonts()
{
	if (s_font_data.empty())
		return;

	ImGuiManager::SetFonts({});
	FileSystem::UnmapFile(s_font_data);
	s_font_data = {};
}

static void ApplyCoreSettings(const std::string& bios_name)
{
	s_settings.SetStringValue("Filenames", "BIOS", bios_name.c_str());
	// Only files explicitly requested by the manifest are inserted.
	s_settings.SetStringValue("MemoryCards", "Slot1_Filename", "");
	s_settings.SetStringValue("MemoryCards", "Slot2_Filename", "");
	// x6 enables named arcade groups by default; use its existing enable/disable lists
	// so only this group is controlled, without disabling other compatibility patches.
	s_settings.SetStringList(Patch::PATCHES_CONFIG_SECTION,
		s_loaded_options.region_bypass ? Patch::PATCH_ENABLE_CONFIG_KEY : Patch::PATCH_DISABLE_CONFIG_KEY,
		{REGION_BYPASS_PATCH_NAME});

	// The GS renders without a window on the selected API, and the frontend paces the session.
	const GSRendererType renderer =
		s_loaded_options.renderer == LibretroOptions::Renderer::D3D11 ? GSRendererType::DX11 : GSRendererType::VK;
	s_settings.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(renderer));
	s_settings.SetBoolValue("EmuCore/GS", "VsyncEnable", false);
	// The maximum frame size reported to the frontend follows the same multiplier.
	s_settings.SetFloatValue("EmuCore/GS", "upscale_multiplier", static_cast<float>(s_loaded_options.upscale_multiplier));
	s_settings.SetStringValue("SPU2/Output", "Backend", "Null");
	// The frontend paces at an unlimited target speed, so SPU2 applies its fast-forward
	// volume whenever it is unmuted; both volumes carry the one core volume.
	s_settings.SetUIntValue("SPU2/Output", "StandardVolume", s_applied_volume);
	s_settings.SetUIntValue("SPU2/Output", "FastForwardVolume", s_applied_volume);

	// Automatic keeps the emulator's hardware-dependent default from SetDefaultSettings.
	if (s_loaded_options.mtvu != LibretroOptions::MtvuSetting::Automatic)
		s_settings.SetBoolValue("EmuCore/Speedhacks", "vuThread", s_loaded_options.mtvu == LibretroOptions::MtvuSetting::Enabled);

	// Input arrives through the libretro API, never from host devices.
	for (const char* source : {"SDL", "DInput", "XInput", "RawInput"})
		s_settings.SetBoolValue("InputSources", source, false);
	s_settings.ClearSection("Hotkeys");

	s_settings.SetBoolValue("Achievements", "Enabled", false);
	s_settings.SetBoolValue("EmuCore", "EnableDiscordPresence", false);
	s_settings.SetBoolValue("EmuCore", "EnablePINE", false);
	s_settings.SetBoolValue("EmuCore", "EnableRecordingTools", false);
	s_settings.SetBoolValue("EmuCore", "InhibitScreensaver", false);

	// Emulator logging goes through the frontend only. A system console would
	// open a window on Windows.
	s_settings.SetBoolValue("Logging", "EnableSystemConsole", false);
	s_settings.SetBoolValue("Logging", "EnableFileLogging", false);
}

static void LoadCoreSettings(const std::string& bios_name, const std::string& save_directory)
{
	{
		const std::unique_lock lock = Host::GetSettingsLock();
		if (!Host::Internal::GetBaseSettingsLayer())
			Host::Internal::SetBaseSettingsLayer(&s_settings);

		// AutoDetectSource edits this layer, so a session starts from the defaults.
		s_settings.Clear();
		VMManager::SetDefaultSettings(s_settings, true, true, true, true, true);
		ApplyCoreSettings(bios_name);
		s_settings.SetStringValue("Folders", "MemoryCards", Path::Combine(save_directory, "memcards").c_str());
	}

	// The core creates only directories used by the active features.
	VMManager::Internal::LoadStartupSettings(false);
}

static bool ValidateBios(const std::string& bios_name, Error* error)
{
	// Do not let x6 silently fall back to another image if the selected one changes
	// between discovery and startup.
	const std::string bios_path = Path::Combine(EmuFolders::Bios, bios_name);
	u32 version, region;
	std::string description, zone;
	if (!IsBIOS(bios_path.c_str(), version, description, region, zone) || zone != "COH-H")
	{
		Error::SetStringFmt(error, "'{}' is not a valid BIOS image.", bios_path);
		return false;
	}

	Console.WriteLnFmt("Using BIOS '{}' ({} {})", bios_name, description, zone);
	return true;
}

bool LibretroHost::PrepareContent(const char* content_path, VMBootParameters* boot_params, Error* error)
{
	// The Windows file functions reject absolute paths with forward slashes, which
	// frontends can legitimately pass.
	std::string native_content_path = content_path ? Path::ToNativePath(content_path) : std::string();
	if (!FileSystem::FileExists(native_content_path.c_str()))
	{
		Error::SetStringFmt(error, "Content file '{}' does not exist.", native_content_path);
		return false;
	}

	if (!StringUtil::compareNoCase(Path::GetExtension(native_content_path), CONTENT_EXTENSION))
	{
		Error::SetStringFmt(error, "'{}' is not a .{} manifest.", native_content_path, CONTENT_EXTENSION);
		return false;
	}

	const char* hardware_error = nullptr;
	if (!VMManager::PerformEarlyHardwareChecks(&hardware_error))
	{
		Error::SetString(error, hardware_error);
		return false;
	}

	std::string system_directory, save_directory;
	if (!GetFrontendDirectory(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, "system", &system_directory, error) ||
		!GetFrontendDirectory(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, "save", &save_directory, error))
	{
		return false;
	}

	s_loaded_options = {LibretroOptions::GetBios(s_environment), LibretroOptions::GetRegionBypass(s_environment),
		LibretroOptions::GetMtvu(s_environment), LibretroOptions::GetUpscaleMultiplier(s_environment),
		LibretroOptions::GetRenderer(s_environment)};
	s_applied_volume = LibretroOptions::GetVolume(s_environment);
	s_reload_notice_shown = false;

	const std::string source_bios_directory = GetSourceBiosDirectory(system_directory);
	std::string bios_name;
	if (!ResolveBios(source_bios_directory, &bios_name, error))
		return false;

	EmuFolders::AppRoot = system_directory;
	EmuFolders::DataRoot = system_directory;
	EmuFolders::Resources = Path::Combine(system_directory, "resources");
	// The in-memory settings need no INI directory. x6 keeps playtime.dat here.
	EmuFolders::Settings = system_directory;
	if (!CheckRuntimeResources(error) || !LoadFonts(error))
		return false;

	if (FileSystem::IsSamePath(system_directory.c_str(), save_directory.c_str()))
	{
		Error::SetStringFmt(error,
			"The system and save directories share '{}'. Use a separate save directory.", system_directory);
		return false;
	}

	Error directory_error;
	if (!FileSystem::EnsureDirectoryExists(save_directory.c_str(), true, &directory_error))
	{
		Error::SetStringFmt(error, "Cannot create the save directory '{}': {}", save_directory,
			directory_error.GetDescription());
		return false;
	}

	LoadCoreSettings(bios_name, save_directory);
	if (!FileSystem::EnsureDirectoryExists(EmuFolders::Cache.c_str(), true, &directory_error))
	{
		Error::SetStringFmt(error, "Cannot create the cache directory '{}': {}", EmuFolders::Cache,
			directory_error.GetDescription());
		return false;
	}

	if (!ValidateBios(bios_name, error))
		return false;

	// SRAM is a core extension of the save layout: one image per gameid, seeded from the
	// manifest's image on first use. x6 resolves the gameid and creates the game folder.
	s_content_path = native_content_path;
	boot_params->filename = std::move(native_content_path);
	// Unlimited disables the VM's own frame limiter: the frontend paces every frame.
	boot_params->start_unlimited = true;
	boot_params->arcade_save_directory = Path::Combine(save_directory, ARCADE_SAVE_DIRECTORY_NAME);
	boot_params->arcade_dongle_directory = Path::Combine(system_directory, "memcards");
	return true;
}

const std::string& LibretroHost::GetContentPath()
{
	return s_content_path;
}

u32 LibretroHost::GetUpscaleMultiplier()
{
	return s_loaded_options.upscale_multiplier;
}

LibretroOutput::Backend LibretroHost::GetBackend()
{
	return s_loaded_options.renderer == LibretroOptions::Renderer::D3D11 ? LibretroOutput::Backend::D3D11 :
																		   LibretroOutput::Backend::Vulkan;
}

void LibretroHost::CheckOptionChanges()
{
	bool updated = false;
	if (!s_environment(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) || !updated)
		return;

	const unsigned volume = LibretroOptions::GetVolume(s_environment);
	if (volume != s_applied_volume)
	{
		s_applied_volume = volume;
		LibretroSession::RunOnCPUThread([volume]() { ApplyVolume(volume); }, false);
	}

	// Options that take effect on the next content load are compared with the values this
	// content was loaded with; a session shows the notice once.
	const bool reload_changed = LibretroOptions::GetBios(s_environment) != s_loaded_options.bios ||
		LibretroOptions::GetRegionBypass(s_environment) != s_loaded_options.region_bypass ||
		LibretroOptions::GetMtvu(s_environment) != s_loaded_options.mtvu ||
		LibretroOptions::GetUpscaleMultiplier(s_environment) != s_loaded_options.upscale_multiplier ||
		LibretroOptions::GetRenderer(s_environment) != s_loaded_options.renderer;
	if (reload_changed && !std::exchange(s_reload_notice_shown, true))
	{
		Console.WriteLn("Changed core options take effect when the content is loaded again");
		ShowNotice(LibretroOptions::GetReloadNotice(s_environment));
	}
}

void Host::CommitBaseSettingChanges()
{
	// Settings live in memory for the lifetime of the core.
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	return false;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
}

bool Host::LocaleCircleConfirm()
{
	return false;
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	Console.WriteLnFmt("{}: {}", title, message);
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	Console.ErrorFmt("{}: {}", title, message);
}

void Host::OpenURL(const std::string_view url)
{
	Console.WarningFmt("Ignoring URL open request: {}", url);
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
	return false;
}

void Host::BeginTextInput()
{
}

void Host::EndTextInput()
{
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	return std::nullopt;
}

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name)
{
}

void Host::OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier)
{
}

void Host::SetMouseMode(bool relative_mode, bool hide_cursor)
{
}

void Host::SetMouseLock(bool state)
{
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	// The core owns its graphics device and renders without a window.
	WindowInfo window_info;
	window_info.type = WindowInfo::Type::Surfaceless;
	return window_info;
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

void Host::OnVMStarting()
{
}

void Host::OnVMStarted()
{
}

void Host::OnVMDestroyed()
{
}

void Host::OnVMPaused()
{
}

void Host::OnVMResumed()
{
}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
	const std::string& disc_serial, u32 disc_crc, u32 current_crc)
{
}

void Host::OnPerformanceMetricsUpdated()
{
}

void Host::OnSaveStateLoading(const std::string_view filename)
{
}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful)
{
}

void Host::OnSaveStateSaved(const std::string_view filename)
{
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	LibretroSession::RunOnCPUThread(std::move(function), block);
}

void Host::RunOnGSThread(std::function<void()> function)
{
	// The MTGS ring has a single producer, which is the CPU thread.
	RunOnCPUThread([function = std::move(function)]() { MTGS::RunOnGSThread(function); });
}

void Host::PumpMessagesOnCPUThread()
{
	LibretroSession::PumpCPUThread();
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
}

void Host::CancelGameListRefresh()
{
}

bool Host::IsFullscreen()
{
	return false;
}

void Host::SetFullscreen(bool enabled)
{
}

void Host::OnCaptureStarted(const std::string& filename)
{
}

void Host::OnCaptureStopped()
{
}

void Host::RequestExitApplication(bool allow_confirm)
{
	Console.Warning("Ignoring application exit request: the frontend owns the process");
}

void Host::RequestExitBigPicture()
{
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
	Console.Warning("The emulator requested a VM shutdown");
	LibretroSession::RequestStop();
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
}

void Host::OnAchievementsRefreshed()
{
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
}

void Host::OnCoverDownloaderOpenRequested()
{
}

void Host::OnCreateMemoryCardOpenRequested()
{
}

bool Host::InBatchMode()
{
	return true;
}

bool Host::InNoGUIMode()
{
	return true;
}

bool Host::ShouldPreferHostFileSelector()
{
	return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
	FileSelectorFilters filters, std::string_view initial_directory)
{
	// An empty path reports a cancelled selection.
	callback(std::string());
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	return lhs.compare(rhs);
}

s32 Host::Internal::GetTranslatedStringImpl(
	const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;
	else if (msg.empty())
		return 0;

	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	const TinyString count_string = TinyString::from_format("{}", count);

	std::string translated(msg);
	for (std::string::size_type position = translated.find("%n"); position != std::string::npos;
		 position = translated.find("%n", position + count_string.length()))
	{
		translated.replace(position, 2, count_string.view());
	}

	return translated;
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str)
{
	return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	return std::nullopt;
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
	return nullptr;
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()
