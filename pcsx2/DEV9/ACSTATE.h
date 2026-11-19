// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <span>

class Error;

/// Guest state of the arcade board devices that the PS2 save state entries do not cover: ACRAM
/// DMA pointers, the JVS bus, ACCORE interrupts and pending DMA, the ATA and ATAPI devices, the
/// UART registers, the DEV9 interrupt registers and the memory card protocol between commands.
/// One fixed-size segment, written and read field by field; nothing in it is a host address.
namespace ACSTATE
{
	/// Fails for boards whose state is not covered, such as a game with a serial drive-board device.
	bool CheckSupported(Error* error);

	u32 GetSize();
	void Save(std::span<u8> out);

	/// Checks a saved segment without changing anything: format, and every length or position
	/// that indexes a device buffer.
	bool Validate(std::span<const u8> data, Error* error);

	/// Restores a validated segment.
	void Load(std::span<const u8> data);
} // namespace ACSTATE
