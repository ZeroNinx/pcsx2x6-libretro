# pcsx2x6-libretro

A libretro core project for PCSX2x6's NAMCO System 246 and System 256 arcade emulation, designed to be loaded by frontends such as RetroArch.

## Status

Version 0.1 is an experimental Windows x86-64 core. It runs arcade VMs using Vulkan and presents video at the GS output dimensions and display aspect ratio. Stereo audio output and two-player RetroPad input for JVS fighting layouts and basic digital drum controls are connected. A Vulkan-capable graphics driver is required even when RetroArch uses another video driver.

See the [known issues](docs/issues.md), [development roadmap](docs/roadmap.md), [M1 integration plan](docs/M1.md), and [upstream sources](docs/upstream.md).

## Installing and running

Extract the test package into your RetroArch directory. It supplies these paths:

```text
cores/pcsx2x6_libretro.dll
info/pcsx2x6_libretro.info
system/pcsx2x6/resources/
docs/pcsx2x6/
```

If RetroArch uses custom core, core-info or system directories, copy each item to the corresponding configured location instead. Restart RetroArch after installing the core information file. Load Content can then offer PCSX2x6 for `.acgame` files. For playlists, use Manual Scan with the `acgame` extension; manifests are not matched by a ROM database scan.

If old core information persists, close RetroArch and remove `core_info.cache` from its configured core-info directory so it can be rebuilt.

1. Put your COH-H arcade BIOS dump and all same-stem companion files in `<system>/pcsx2x6/bios/`. BIOS Image defaults to **Automatic**, which selects the first valid arcade BIOS in filename order, regardless of extension. If several are available, choose the required image in Core Options and reopen the content. Automatic discovery identifies arcade BIOS images; it does not determine which BIOS revision is compatible with every game.
2. Put the working dongle named by `dongle=` in `<system>/pcsx2x6/memcards/`. It contains boot/security data, not game saves. x6 can write to it, so retain an original separately. If the manifest specifies `card=`, put that separate save card in `<save-root>/memcards/`.
3. Open the game's `.acgame` manifest with this core. Keep its ELF and media paths valid; existing PCSX2x6 manifests are used directly. A BIOS and game files are not included in the package.

Some games require the external [PCSX2x6 patches archive](https://github.com/PS2Homebrew-arcade/pcsx2x6_patches/releases). Place it at `<system>/pcsx2x6/resources/patches.zip`. It is not included in this package. NM00002 does not require a patch; NM00054 requires its Region check bypass. See [resource compatibility](docs/M2.md#resources-and-patch-ownership) for the archive revision supported by this emulation base.

**Region Check Bypass** defaults to **Enabled** in Core Options. It controls the matching `Region check bypass` patch group; other compatibility patches keep their existing behavior. Close and reopen content after changing it. Enabling the option still requires a matching patch in the external archive; it does not generate a patch for unsupported games.

`<save-root>` is `<save>/pcsx2x6` unless RetroArch already supplies a core-specific directory, such as `<save>/PCSX2x6` or `<save>/PCSX2x6/<content>`; that directory is used directly. No second `pcsx2x6` layer is added. The save root contains only per-game SRAM in `arcade/<gameid>/sram.bin` and optional save cards in `memcards/`. Variants sharing a GameID share their SRAM.

BIOS files are loaded directly from `<system>/pcsx2x6/bios/`; their `.nvm`/`.mec` companions retain x6's board-state behavior in that folder. Shader caches go in `<system>/pcsx2x6/cache/`, and `playtime.dat` is kept in `<system>/pcsx2x6/`. The system area must allow these runtime writes. Unused emulator directories are not created. The system and save roots must be separate.

### Controls

Both ports use RetroPad. For fighting layouts, the four face buttons follow x6's PS2 positions: Square → Y, Cross → B, Circle → A, Triangle → X. Directions and Start retain their usual meanings. Select inserts a coin, L3 is Service, and port 1 R3 toggles Test Mode. In Test Mode, use the game's menu to exit and save.

In drum mode, Left = left Ka, Down = left Don, B = right Don, A = right Ka, independently for each player. Drum mode does not drive JVS direction bits. High-speed drum input and reset after gameplay have [known issues](docs/issues.md); close and reopen content instead of using reset. Save states are not supported.

## Building

Windows x86-64 with Visual Studio 2022, CMake, Ninja, Git, and Python 3 is currently supported. From a Visual Studio x64 developer prompt:

```text
cmake -S libretro/deps -B build/deps -G Ninja -DCMAKE_INSTALL_PREFIX=<dependency-prefix>
cmake --build build/deps
cmake -S . -B build/core -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<dependency-prefix>
cmake --build build/core --target pcsx2x6_libretro
```

The core is written to `build/core/bin/pcsx2x6_libretro.dll`.

Stage the runtime layout for a RetroArch installation with:

```text
cmake --install build/core --component Libretro --prefix build/package
```

To create a ZIP with the core, metadata, runtime resources, notices and source snapshot:

```text
python libretro/package.py --build-dir build/core --dependency-build-dir build/deps --output-dir build/dist
```

Binary distributions must retain the license notices and be accompanied by the corresponding source, including the dependency sources used to build them. The dependency recipes are in `libretro/deps/`.

## Upstream projects

- [PCSX2](https://github.com/PCSX2/pcsx2): the upstream PlayStation 2 emulator.
- [PCSX2x6](https://github.com/PS2Homebrew-arcade/pcsx2x6): the source of System 246 and System 256 arcade support.
- [PCEE2](https://github.com/WizzardSK/pcee2-libretro) and [paraLLEl-PS2](https://github.com/libretro/parallel-ps2): references for libretro integration.

This project is independent and is not affiliated with or endorsed by these projects.

## License

GNU General Public License, version 3. See [COPYING.GPLv3](COPYING.GPLv3).

Imported upstream source keeps its copyright and license notices, including the notices of third-party components under `3rdparty/`.
