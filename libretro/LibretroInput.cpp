// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#include "LibretroInput.h"
#include "LibretroSession.h"

#include "Config.h"
#include "DEV9/ACJV.h"

#include "common/Console.h"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace
{
	// A JVS player bit and the RetroPad button that drives it.
	struct ButtonMapping
	{
		u16 jvs_mask;
		u32 retropad_button;
	};

	// Cabinet controls every game has.
	constexpr std::array<ButtonMapping, 2> SYSTEM_BUTTONS = {{
		{JVS_BTN_START, RETRO_DEVICE_ID_JOYPAD_START},
		{JVS_BTN_SERVICE, RETRO_DEVICE_ID_JOYPAD_L3},
	}};

	// Joystick directions; a drum cabinet has none, and its D-pad hits the drum instead.
	constexpr std::array<ButtonMapping, 4> DIRECTION_BUTTONS = {{
		{JVS_BTN_UP, RETRO_DEVICE_ID_JOYPAD_UP},
		{JVS_BTN_DOWN, RETRO_DEVICE_ID_JOYPAD_DOWN},
		{JVS_BTN_LEFT, RETRO_DEVICE_ID_JOYPAD_LEFT},
		{JVS_BTN_RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT},
	}};

	// RetroPad button of each drum part, keyed by the part name in x6's drum bindings
	// ("P1_DonLeft"): the left hand plays the D-pad, the right hand the face buttons.
	constexpr std::array<std::pair<std::string_view, u32>, 4> DRUM_PARTS = {{
		{"KaLeft", RETRO_DEVICE_ID_JOYPAD_LEFT},
		{"DonLeft", RETRO_DEVICE_ID_JOYPAD_DOWN},
		{"DonRight", RETRO_DEVICE_ID_JOYPAD_B},
		{"KaRight", RETRO_DEVICE_ID_JOYPAD_A},
	}};

	// A drum sensor channel and the RetroPad button that hits it.
	struct DrumMapping
	{
		u32 channel;
		u32 retropad_button;
		const char* description;
	};

	// Select inserts a coin into the port's own slot; only port 1 toggles the global Test Mode.
	constexpr u32 COIN_BUTTON = RETRO_DEVICE_ID_JOYPAD_SELECT;
	constexpr u32 TEST_BUTTON = RETRO_DEVICE_ID_JOYPAD_R3;
	constexpr u32 TEST_PORT = 0;

	// An edge-triggered control. It is disarmed at session boundaries, so a button that is
	// already held then does not trigger until it has been released once.
	struct EdgeControl
	{
		bool armed = false;

		bool Pressed(bool down)
		{
			const bool triggered = armed && down;
			armed = !down;
			return triggered;
		}
	};

	std::array<EdgeControl, LibretroSession::INPUT_PORTS> s_coin;
	EdgeControl s_test;

	// RetroPad button at the position x6 binds a layout button to on a PS2 pad.
	bool GetRetroPadButton(GenericInputBinding binding, u32* button)
	{
		switch (binding)
		{
			case GenericInputBinding::Square: *button = RETRO_DEVICE_ID_JOYPAD_Y; return true;
			case GenericInputBinding::Cross: *button = RETRO_DEVICE_ID_JOYPAD_B; return true;
			case GenericInputBinding::Circle: *button = RETRO_DEVICE_ID_JOYPAD_A; return true;
			case GenericInputBinding::Triangle: *button = RETRO_DEVICE_ID_JOYPAD_X; return true;
			case GenericInputBinding::L1: *button = RETRO_DEVICE_ID_JOYPAD_L; return true;
			case GenericInputBinding::R1: *button = RETRO_DEVICE_ID_JOYPAD_R; return true;
			case GenericInputBinding::L2: *button = RETRO_DEVICE_ID_JOYPAD_L2; return true;
			case GenericInputBinding::R2: *button = RETRO_DEVICE_ID_JOYPAD_R2; return true;
			default: return false;
		}
	}

	// Action buttons of the running game, which wires both players the same way.
	std::vector<ButtonMapping> GetActionButtons()
	{
		std::vector<ButtonMapping> buttons;
		for (const std::span<const InputBindingInfo> layout : {ACJV::GetFightingButtons(), ACJV::GetStandardButtons()})
		{
			for (const InputBindingInfo& info : layout)
			{
				u32 button;
				if (GetRetroPadButton(info.generic_mapping, &button))
					buttons.push_back({static_cast<u16>(info.bind_index), button});
				else
					Console.WarningFmt("JVS button '{}' has no RetroPad position and stays unmapped", info.display_name);
			}
		}
		return buttons;
	}

	// Drum sensors of each player, from x6's drum bindings.
	std::array<std::vector<DrumMapping>, LibretroSession::INPUT_PORTS> GetDrumMappings()
	{
		std::array<std::vector<DrumMapping>, LibretroSession::INPUT_PORTS> drums;
		for (const InputBindingInfo& info : ACJV::GetDrumBindings())
		{
			const std::string_view name(info.name);
			const u32 player = name.starts_with("P1_") ? 0 : (name.starts_with("P2_") ? 1 : LibretroSession::INPUT_PORTS);
			const auto part = std::find_if(DRUM_PARTS.begin(), DRUM_PARTS.end(),
				[&name](const auto& entry) { return name.size() > 3 && name.substr(3) == entry.first; });
			if (player >= LibretroSession::INPUT_PORTS || part == DRUM_PARTS.end())
			{
				Console.WarningFmt("Drum binding '{}' has no RetroPad button and stays unmapped", info.name);
				continue;
			}
			drums[player].push_back({info.bind_index, part->second, info.display_name});
		}
		return drums;
	}

	// The game's controls are fixed for a session, so they are looked up when it starts.
	std::vector<ButtonMapping> s_action_buttons;
	bool s_drum_mode = false;
	std::array<std::vector<DrumMapping>, LibretroSession::INPUT_PORTS> s_drums;

	std::optional<u32> FindTestModeSwitch()
	{
		const std::span<const ACJV::DIPSwitchInfo> switches = ACJV::GetDIPSwitches();
		for (u32 i = 0; i < switches.size(); i++)
		{
			if (&switches[i] == &ACJV::GetTestModeDIPSwitch())
				return i;
		}
		return std::nullopt;
	}

	// Several RetroPad buttons may drive one JVS bit (a layout can wire actions onto the
	// direction bits), so every bit is pressed while any of its buttons is held. Drum
	// sensors read the level of their button.
	void ApplyLevels(u32 port, u16 buttons)
	{
		u16 mapped = 0;
		u16 pressed = 0;
		const auto collect = [buttons, &mapped, &pressed](const ButtonMapping& mapping) {
			mapped |= mapping.jvs_mask;
			if (buttons & (1u << mapping.retropad_button))
				pressed |= mapping.jvs_mask;
		};
		for (const ButtonMapping& mapping : SYSTEM_BUTTONS)
			collect(mapping);
		if (!s_drum_mode)
		{
			for (const ButtonMapping& mapping : DIRECTION_BUTTONS)
				collect(mapping);
		}
		for (const ButtonMapping& mapping : s_action_buttons)
			collect(mapping);

		ACJV::SetButtonState(port, mapped & ~pressed, false);
		ACJV::SetButtonState(port, pressed, true);

		for (const DrumMapping& drum : s_drums[port])
			ACJV::SetDrumHit(drum.channel, (buttons & (1u << drum.retropad_button)) != 0);
	}
} // namespace

void LibretroInput::Reset()
{
	s_action_buttons = GetActionButtons();
	s_drum_mode = (ACJV::GetMode() == JVS_MODE::DRUM);
	s_drums = s_drum_mode ? GetDrumMappings() : decltype(s_drums)();

	// Nothing of the previous session stays held, whichever controls this game uses.
	for (u32 port = 0; port < LibretroSession::INPUT_PORTS; port++)
	{
		ACJV::SetButtonState(port, 0xFFFF, false);
		s_coin[port] = {};
	}
	for (u32 channel = 0; channel < JVS_DRUM_CHANNEL_MAX; channel++)
		ACJV::SetDrumHit(channel, false);
	s_test = {};
}

void LibretroInput::Apply(const LibretroSession::InputSnapshot& input)
{
	for (u32 port = 0; port < LibretroSession::INPUT_PORTS; port++)
	{
		// A port that lost or changed its device waits for a release before its edges trigger
		// again, so a button held while reconnecting does not trigger, even when the frontend
		// disconnected and reconnected it between two frames.
		if (!input.connected[port] || input.device_changed[port])
		{
			s_coin[port] = {};
			if (port == TEST_PORT)
				s_test = {};
		}

		// A port without a device releases its controls.
		if (!input.connected[port])
		{
			ApplyLevels(port, 0);
			continue;
		}

		const u16 buttons = input.joypad_buttons[port];
		ApplyLevels(port, buttons);
		if (s_coin[port].Pressed((buttons & (1u << COIN_BUTTON)) != 0))
			ACJV::InsertCoin(port);
	}

	if (input.connected[TEST_PORT] && s_test.Pressed((input.joypad_buttons[TEST_PORT] & (1u << TEST_BUTTON)) != 0))
	{
		if (const std::optional<u32> test_switch = FindTestModeSwitch())
			ACJV::ToggleDIPSwitchState(*test_switch);
		else
			Console.Error("The JVS Test Mode switch is missing; Test was not toggled");
	}
}

void LibretroInput::DescribeControllers(retro_environment_t environment)
{
	static constexpr retro_controller_description retropad[] = {{"RetroPad", RETRO_DEVICE_JOYPAD}};
	static constexpr retro_controller_info ports[LibretroSession::INPUT_PORTS + 1] = {
		{retropad, 1},
		{retropad, 1},
		{nullptr, 0},
	};
	environment(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, const_cast<retro_controller_info*>(ports));
}

void LibretroInput::DescribeInputs(retro_environment_t environment)
{
	// The frontend keeps pointers to these descriptions, so they live until the next call.
	static std::vector<retro_input_descriptor> descriptors;
	descriptors.clear();

	const auto add = [](u32 port, u32 button, const char* description) {
		descriptors.push_back({port, RETRO_DEVICE_JOYPAD, 0, button, description});
	};
	const bool drum_mode = (ACJV::GetMode() == JVS_MODE::DRUM);
	const auto drums = drum_mode ? GetDrumMappings() : decltype(s_drums)();
	for (u32 port = 0; port < LibretroSession::INPUT_PORTS; port++)
	{
		if (drum_mode)
		{
			for (const DrumMapping& drum : drums[port])
				add(port, drum.retropad_button, drum.description);
		}
		else
		{
			add(port, RETRO_DEVICE_ID_JOYPAD_UP, "Up");
			add(port, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down");
			add(port, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left");
			add(port, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right");
		}
		add(port, RETRO_DEVICE_ID_JOYPAD_START, "Start");
		add(port, COIN_BUTTON, "Coin");
		add(port, RETRO_DEVICE_ID_JOYPAD_L3, "Service");
		for (const std::span<const InputBindingInfo> layout : {ACJV::GetFightingButtons(), ACJV::GetStandardButtons()})
		{
			for (const InputBindingInfo& info : layout)
			{
				u32 button;
				if (GetRetroPadButton(info.generic_mapping, &button))
					add(port, button, info.display_name);
			}
		}
	}
	add(TEST_PORT, TEST_BUTTON, "Test Mode");
	descriptors.push_back({});

	if (!environment(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, descriptors.data()))
		Console.Warning("The frontend does not take input descriptions");
}
