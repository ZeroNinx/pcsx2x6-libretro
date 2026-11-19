// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "libretro.h"

#include <cstddef>
#include <string>

/// In-memory save states for the libretro serialization interface: a core-owned, uncompressed
/// container of the emulator's state segments with build and content identity, a segment table
/// and per-segment CRCs. States load only into the same build, content and key configuration.
namespace LibretroState
{
	/// Frontend thread: forgets the measurement of the previous content.
	void ResetSession();

	/// Frontend thread: the length of a state of the running VM as it stands; 0 while no VM runs
	/// or when its state cannot be saved. The length follows the VM's memory contents, so it
	/// changes as the VM runs; the measurement is reused until the VM changes.
	size_t GetSize();

	/// Frontend thread: writes a state of the VM as it stands into `data`. False when no VM runs,
	/// the frontend asks for a context other than a normal save, `size` is below the state's
	/// length, or a memory card command is in progress; the VM is unchanged in every case.
	bool Save(retro_environment_t environment, void* data, size_t size);

	enum class LoadResult
	{
		Loaded,
		/// Rejected before anything changed; the session continues.
		Rejected,
		/// Failed after the VM had started changing; the session has to be ended.
		Failed,
	};

	/// Frontend thread: restores a state. The container, identity and every segment are checked
	/// before the VM is changed. A Failed load describes its cause in `failure`.
	LoadResult Load(retro_environment_t environment, const void* data, size_t size, std::string* failure);
} // namespace LibretroState
