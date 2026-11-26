# pcsx2x6-libretro

A [libretro](https://www.libretro.com/) core for NAMCO System 246 and System 256 arcade games, based on [PCSX2x6](https://github.com/PS2Homebrew-arcade/pcsx2x6).

The core is experimental. It supports Windows x64 with Vulkan (default) or Direct3D 11, two-player fighting and digital drum controls, and save states. Core options are available in English and Simplified Chinese.

## Getting started

1. Extract the core package into your RetroArch installation. If you use custom directories, copy the core, info and system files to their configured locations.
2. Put your COH-H arcade BIOS in `system/pcsx2x6/bios/` and the game's working dongle dump in `system/pcsx2x6/memcards/`.
3. Open the game's `.acgame` manifest in RetroArch and select PCSX2x6.

Some games also need an external patches archive. See the [setup guide](docs/usage.md) for required files, core options, controls and save-state compatibility, and [known issues](docs/issues.md) for current limitations.

## Building

Build with Visual Studio 2022, CMake, Ninja, Git and Python 3. See [building and packaging](docs/building.md) for the commands.

## Documentation

- [Development roadmap](docs/roadmap.md)
- [Upstream sources and changes](docs/upstream.md)

## License

[GNU General Public License, version 3](COPYING.GPLv3). Upstream and third-party copyright and license notices are preserved in the source tree.
