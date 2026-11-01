# pcsx2x6-libretro

A libretro core project for PCSX2x6's NAMCO System 246 and System 256 arcade emulation, designed to be loaded by frontends such as RetroArch.

## Status

The core can initialize, run, and shut down an arcade VM using Vulkan and presents video at the GS output dimensions and display aspect ratio. Stereo audio output and two-player RetroPad input for JVS fighting layouts are connected.

See the [development roadmap](docs/roadmap.md), [M1 integration plan](docs/M1.md), and [upstream sources](docs/upstream.md).

## Building

Windows x86-64 with Visual Studio 2022, CMake, Ninja, Git, and Python 3 is currently supported. From a Visual Studio x64 developer prompt:

```text
cmake -S libretro/deps -B build/deps -G Ninja -DCMAKE_INSTALL_PREFIX=<dependency-prefix>
cmake --build build/deps
cmake -S . -B build/core -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<dependency-prefix>
cmake --build build/core --target pcsx2x6_libretro
```

The core is written to `build/core/bin/pcsx2x6_libretro.dll`.

## Upstream projects

- [PCSX2](https://github.com/PCSX2/pcsx2): the upstream PlayStation 2 emulator.
- [PCSX2x6](https://github.com/PS2Homebrew-arcade/pcsx2x6): the source of System 246 and System 256 arcade support.
- [PCEE2](https://github.com/WizzardSK/pcee2-libretro) and [paraLLEl-PS2](https://github.com/libretro/parallel-ps2): references for libretro integration.

This project is independent and is not affiliated with or endorsed by these projects.

## License

GNU General Public License, version 3. See [COPYING.GPLv3](COPYING.GPLv3).

Imported upstream source keeps its copyright and license notices, including the notices of third-party components under `3rdparty/`.
