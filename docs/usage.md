# Setup and Usage

## Requirements

- Windows x64 and a compatible GPU.
- RetroArch or another libretro frontend with a matching hardware video driver. Vulkan requires context negotiation v2; Direct3D 11 uses the standard libretro D3D11 interface.

RetroArch 1.22.2 is the reference frontend. It switches to the video driver selected by the core's **Renderer** option when content loads. There is no software-video fallback.

## Install or update

Extract the core package into your RetroArch directory. The runtime files use this layout:

```text
cores/pcsx2x6_libretro.dll
info/pcsx2x6_libretro.info
system/pcsx2x6/resources/
docs/pcsx2x6/
```

If you use custom directories, copy each item to the location configured in RetroArch. When updating, replace both the DLL and its `.info` file, then restart RetroArch. If the core information is still outdated, close RetroArch and rename `core_info.cache` in the core-info directory so it can be rebuilt.

## Required files

`<system>` and `<save>` below refer to the directories configured in RetroArch.

| File | Location |
| --- | --- |
| COH-H arcade BIOS and its same-stem companion files | `<system>/pcsx2x6/bios/` |
| Working dongle named by `dongle=` in the manifest | `<system>/pcsx2x6/memcards/` |
| Optional save card named by `card=` | `<save-root>/memcards/` |
| External compatibility patches | `<system>/pcsx2x6/resources/patches.zip` |

**BIOS Image: Automatic** selects the first valid arcade BIOS in filename order. Select an image explicitly if a game needs a particular BIOS revision.

Dongles contain boot data and are separate from save cards. Use working copies: the emulator can write to dongles, save cards and BIOS companion files.

Some games need the [PCSX2x6 patches archive](https://github.com/PS2Homebrew-arcade/pcsx2x6_patches/releases). Download it separately and place it at the path above. NM00054 requires its region-check bypass patch; NM00002 does not require patches. **Region Check Bypass** enables the matching patch in this archive. See [resource compatibility](M2.md#resources-and-patch-ownership) for the archive revision used by this emulation base.

Keep the runtime resources supplied with the core, including its game database, shaders and fonts.

## Load a game

Open an existing PCSX2x6 `.acgame` manifest and select this core. The ELF and media paths in the manifest must point to your game files.

For playlists, use RetroArch's Manual Scan with the `acgame` extension. Manifests are not matched by a ROM database scan.

## Core options

Options are available in English and Simplified Chinese. Close and reload content after changing an option, except **Volume**, which applies immediately.

| Option | Default | Purpose |
| --- | --- | --- |
| BIOS Image | Automatic | Select an arcade BIOS. |
| Region Check Bypass | Enabled | Enable the matching patch from `patches.zip`. |
| VU1 Thread (MTVU) | Automatic | Run VU1 on a separate CPU thread. |
| Renderer | Vulkan | Select Vulkan or Direct3D 11. |
| Internal Resolution | Native | Render at 1x to 4x native resolution. Higher values use more video memory. |
| Volume | 100 | Set output volume from 0 to 200 percent. |

## Controls

Both players use RetroPad. Use RetroArch's Controls menu to remap buttons.

| RetroPad control | Action |
| --- | --- |
| D-pad | Direction in fighting games |
| Y / B / A / X | PS2 Square / Cross / Circle / Triangle positions |
| Start | Start |
| Select | Insert a coin for that player |
| L3 | Service |
| Port 1 R3 | Toggle Test Mode |

Fighting-game actions depend on the game's layout. In Test Mode, use the game's menu to exit and save.

### Digital drums

| RetroPad control | Drum action |
| --- | --- |
| Left | Left Ka (rim) |
| Down | Left Don (center) |
| B | Right Don (center) |
| A | Right Ka (rim) |

Each press produces one hit; holding the button does not repeat it. Release and press again for another strike. After starting content, loading a state, resetting or reconnecting, release any held drum buttons first. Up and Right have no drum action. Other arcade peripherals have not been integrated.

## Save data and save states

`<save-root>` is normally `<save>/pcsx2x6`. If RetroArch already supplies a core-specific save directory, the core uses it directly. SRAM is stored at `<save-root>/arcade/<gameid>/sram.bin`; games sharing a GameID share that SRAM file. Optional save cards use `<save-root>/memcards/`.

The system directory must also be writable: BIOS companions stay beside the BIOS, shader caches use `<system>/pcsx2x6/cache/`, and play time is stored at `<system>/pcsx2x6/playtime.dat`. Keep the system and save directories separate.

Manual, automatic and undo save-state operations are supported. A state requires the same core build, content and key settings, including BIOS, MTVU, renderer, internal resolution and patches.

Save states do not roll back dongle, save-card or BIOS companion files. If those files have changed, the old state is rejected. Use ordinary save states only; rewind, run-ahead and netplay are not supported.

For current limitations and workarounds, see [known issues](issues.md).
