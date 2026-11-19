// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "svnrev.h"

/// The core's version as reported to the frontend: release, repository revision and the
/// PCSX2x6 base it is built on. Save states record it as their build identity.
inline constexpr const char* LIBRETRO_CORE_VERSION =
	PCSX2X6_LIBRETRO_VERSION " (" GIT_REV "; PCSX2x6 " PCSX2X6_BASE_REVISION ")";
