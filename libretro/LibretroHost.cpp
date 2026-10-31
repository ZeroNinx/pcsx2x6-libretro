// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#include "LibretroHost.h"
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
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{
	constexpr const char* DIRECTORY_NAME = "pcsx2x6";
	constexpr const char* CONTENT_EXTENSION = "acgame";

	constexpr const char* BIOS_OPTION_KEY = "pcsx2x6_bios";
	constexpr const char* BIOS_OPTION_NONE = "none";

	constexpr size_t MAX_QUEUED_LOG_MESSAGES = 4096;
	constexpr unsigned ERROR_MESSAGE_FRAMES = 300;

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

	std::vector<std::string> s_bios_option_values;
	retro_core_option_v2_definition s_core_option_definitions[2];
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

	*directory = Path::Combine(Path::ToNativePath(path), DIRECTORY_NAME);
	return true;
}

static std::string GetSourceBiosDirectory(const std::string& system_directory)
{
	return Path::Combine(system_directory, "bios");
}

// Compares the directories themselves, so differences in case, separators, and links
// do not hide an overlap. A directory that does not exist yet cannot overlap.
static bool IsSameDirectory(const std::string& first, const std::string& second)
{
	const auto to_path = [](const std::string& path) {
		return std::filesystem::path(std::u8string(path.begin(), path.end()));
	};

	std::error_code error;
	return std::filesystem::equivalent(to_path(first), to_path(second), error);
}

static void RegisterCoreOptions()
{
	s_bios_option_values = {BIOS_OPTION_NONE};

	std::string system_directory;
	if (GetFrontendDirectory(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, "system", &system_directory, nullptr))
	{
		FileSystem::FindResultsArray files;
		FileSystem::FindFiles(GetSourceBiosDirectory(system_directory).c_str(), "*",
			FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_SORT_BY_NAME, &files);
		for (const FILESYSTEM_FIND_DATA& file : files)
		{
			u32 version, region;
			std::string description, zone;
			if (s_bios_option_values.size() < RETRO_NUM_CORE_OPTION_VALUES_MAX - 1 &&
				IsBIOS(file.FileName.c_str(), version, description, region, zone))
			{
				s_bios_option_values.emplace_back(Path::GetFileName(file.FileName));
			}
		}
	}

	std::memset(s_core_option_definitions, 0, sizeof(s_core_option_definitions));
	retro_core_option_v2_definition& bios_option = s_core_option_definitions[0];
	bios_option.key = BIOS_OPTION_KEY;
	bios_option.desc = "BIOS Image";
	bios_option.info = "Arcade BIOS image from the pcsx2x6/bios folder in the system directory.";
	bios_option.default_value = BIOS_OPTION_NONE;
	for (size_t i = 0; i < s_bios_option_values.size(); i++)
		bios_option.values[i].value = s_bios_option_values[i].c_str();

	// Frontends also call this while probing core information, with an environment
	// that rejects everything. Content loading reports a missing BIOS selection.
	retro_core_options_v2 options{nullptr, s_core_option_definitions};
	s_environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options);
}

void LibretroHost::SetEnvironment(retro_environment_t environment)
{
	s_environment = environment;

	retro_log_callback log_callback{};
	s_frontend_log = environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log_callback) ? log_callback.log : nullptr;

	RegisterCoreOptions();
}

static std::string GetSelectedBios()
{
	retro_variable variable{BIOS_OPTION_KEY, nullptr};
	if (!s_environment(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) || !variable.value)
		return BIOS_OPTION_NONE;

	return variable.value;
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

	// The core owns a device without a window, and the frontend paces the session.
	s_settings.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::VK));
	s_settings.SetBoolValue("EmuCore/GS", "FrameLimitEnable", false);
	s_settings.SetBoolValue("EmuCore/GS", "VsyncEnable", false);
	s_settings.SetStringValue("SPU2/Output", "Backend", "Null");

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

static void LoadCoreSettings(const std::string& bios_name)
{
	{
		const std::unique_lock lock = Host::GetSettingsLock();
		if (!Host::Internal::GetBaseSettingsLayer())
			Host::Internal::SetBaseSettingsLayer(&s_settings);

		// AutoDetectSource edits this layer, so a session starts from the defaults.
		s_settings.Clear();
		VMManager::SetDefaultSettings(s_settings, true, true, true, true, true);
		ApplyCoreSettings(bios_name);
	}

	VMManager::Internal::LoadStartupSettings();
}

static bool IsBiosCompanionFile(std::string_view file_name, std::string_view bios_name)
{
	return StringUtil::compareNoCase(Path::GetFileTitle(file_name), Path::GetFileTitle(bios_name)) ||
		   StringUtil::StartsWithNoCase(file_name, fmt::format("{}.", bios_name));
}

// x6 writes .nvm and .mec beside the BIOS it loads, so that BIOS has to live in a
// writable location. Everything sharing its filename stem comes along, because
// extra ROMs are optional and a missing one is skipped with only a log message.
// Existing files are kept: they hold the state of earlier sessions.
static bool PrepareBiosWorkingCopy(const std::string& source_directory, const std::string& bios_name, Error* error)
{
	const std::string source_path = Path::Combine(source_directory, bios_name);
	if (!FileSystem::FileExists(source_path.c_str()))
	{
		Error::SetStringFmt(error, "The selected BIOS '{}' does not exist.", source_path);
		return false;
	}

	FileSystem::FindResultsArray files;
	FileSystem::FindFiles(source_directory.c_str(), "*", FILESYSTEM_FIND_FILES, &files);
	for (const FILESYSTEM_FIND_DATA& file : files)
	{
		const std::string_view file_name = Path::GetFileName(file.FileName);
		if (!IsBiosCompanionFile(file_name, bios_name))
			continue;

		const std::string destination = Path::Combine(EmuFolders::Bios, file_name);
		if (FileSystem::FileExists(destination.c_str()))
			continue;

		if (!FileSystem::CopyFilePath(file.FileName.c_str(), destination.c_str(), false))
		{
			Error::SetStringFmt(error, "Failed to copy '{}' to '{}'.", file.FileName, destination);
			return false;
		}
		Console.WriteLnFmt("Copied BIOS file '{}' to the working directory", file_name);
	}

	// LoadBIOS() falls back to the first image it can find when the configured file
	// is unusable, so a bad working copy has to be rejected before startup.
	const std::string working_path = Path::Combine(EmuFolders::Bios, bios_name);
	u32 version, region;
	std::string description, zone;
	if (!IsBIOS(working_path.c_str(), version, description, region, zone))
	{
		Error::SetStringFmt(error, "'{}' is not a valid BIOS image.", working_path);
		return false;
	}

	Console.WriteLnFmt("Using BIOS '{}' ({} {})", bios_name, description, zone);
	return true;
}

bool LibretroHost::PrepareContent(const char* content_path, std::string* boot_filename, Error* error)
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

	const std::string bios_name = GetSelectedBios();
	if (bios_name == BIOS_OPTION_NONE)
	{
		Error::SetString(error, "No BIOS is selected. Choose one in the core options.");
		return false;
	}

	EmuFolders::AppRoot = system_directory;
	EmuFolders::DataRoot = save_directory;
	EmuFolders::Resources = Path::Combine(system_directory, "resources");
	EmuFolders::Settings = Path::Combine(save_directory, "inis");
	if (!CheckRuntimeResources(error) || !LoadFonts(error))
		return false;

	// The default settings put the BIOS working copy in <data root>/bios. Nothing is
	// written below the save directory before this check.
	const std::string source_bios_directory = GetSourceBiosDirectory(system_directory);
	if (IsSameDirectory(source_bios_directory, Path::Combine(save_directory, "bios")))
	{
		Error::SetStringFmt(error,
			"The system and save directories share the BIOS folder '{}'. BIOS working files would overwrite "
			"the source BIOS; use a separate save directory.",
			source_bios_directory);
		return false;
	}

	if (!FileSystem::EnsureDirectoryExists(save_directory.c_str(), true, error))
		return false;

	LoadCoreSettings(bios_name);
	if (!FileSystem::DirectoryExists(EmuFolders::Bios.c_str()) ||
		!FileSystem::DirectoryExists(EmuFolders::MemoryCards.c_str()))
	{
		Error::SetStringFmt(error, "Failed to create the working directories under '{}'.", save_directory);
		return false;
	}

	if (!PrepareBiosWorkingCopy(source_bios_directory, bios_name, error))
		return false;

	*boot_filename = std::move(native_content_path);
	return true;
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
