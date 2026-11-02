// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "libretro.h"

struct retro_hw_render_interface_vulkan;

/// The Vulkan context the frontend shares with the GS. The frontend creates the instance and
/// the device through the core's negotiation callbacks, so the GS renders on the device the
/// frontend presents with; the GS opens while the device is created, and the VM boots once the
/// frontend's context is ready.
namespace LibretroVulkan
{
	/// Frontend thread, invoked from the context callbacks.
	struct ContextCallbacks
	{
		/// Boots the VM on the ready context; returns whether it started, having reported any failure.
		bool (*context_ready)();
		/// Reports a context the VM cannot run on and ends the session.
		void (*context_failed)(const char* reason);
		/// Stops the VM and closes the GS while the device is still valid.
		void (*context_lost)();
	};

	/// Frontend thread, during retro_load_game: asks the frontend for a Vulkan context created
	/// through negotiation interface v2. Returns false when the frontend cannot provide one.
	bool Register(retro_environment_t environment, const ContextCallbacks& callbacks);

	/// Frontend thread: the frontend's Vulkan interface while its context is ready; null otherwise.
	const retro_hw_render_interface_vulkan* GetInterface();
} // namespace LibretroVulkan
