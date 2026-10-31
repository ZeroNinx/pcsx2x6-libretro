// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "libretro.h"

#include <string>
#include <string_view>

class Error;

namespace LibretroHost
{
	/// Stores the frontend environment, its log interface, and the core options.
	void SetEnvironment(retro_environment_t environment);

	/// Routes emulator logging into the queue drained on the frontend thread.
	void InitializeLogging();

	/// Forwards what is still queued, then disconnects emulator logging.
	void ShutdownLogging();

	/// Forwards queued log messages to the frontend. Must run on the frontend thread.
	void DrainLogMessages();

	/// Logs a failed content load and shows it through the frontend.
	void ReportLoadFailure(std::string_view message);

	/// Validates a content request and prepares the directories, settings, and BIOS it
	/// needs. On success, returns the manifest path to boot in native form.
	bool PrepareContent(const char* content_path, std::string* boot_filename, Error* error);

	/// Releases the font data loaded for the GS. Only valid once the CPU thread has ended.
	void ReleaseFonts();
} // namespace LibretroHost
