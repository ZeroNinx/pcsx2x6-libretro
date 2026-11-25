// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "libretro.h"

namespace LibretroSession
{
	struct InputSnapshot;
} // namespace LibretroSession

/// Turns frontend RetroPad input into the JVS controls of the running game. The control layout
/// comes from x6's GameID-selected layout; levels and edges are applied on the CPU thread.
namespace LibretroInput
{
	/// CPU thread: releases every mapped control at a session boundary. Coin, Test and drum
	/// hits only trigger again after their buttons have been seen released.
	void Reset();

	/// CPU thread: applies the input of the frame that is about to run. Each drum press emits
	/// one guest frame of sensor input; holding the button does not repeat the hit.
	void Apply(const LibretroSession::InputSnapshot& input);

	/// Frontend thread: describes the controls of the game that was just loaded.
	void DescribeInputs(retro_environment_t environment);

	/// Frontend thread: offers the RetroPad on each port; the frontend may also choose none.
	void DescribeControllers(retro_environment_t environment);
} // namespace LibretroInput
