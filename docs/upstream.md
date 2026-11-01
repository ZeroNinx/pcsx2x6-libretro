# Upstream Sources

## Emulation base

| Project | Revision | Integration |
| --- | --- | --- |
| [PCSX2x6](https://github.com/PS2Homebrew-arcade/pcsx2x6) | `583f0e90907d69bb2db670f570d0865260fcc71b` | Selected source snapshot; upstream Git history is not merged |

Retained resource and third-party source files match this revision. Emulation source matches it except for the project changes listed below. Project build configuration is adapted for the libretro integration. Source paths are preserved for upstream comparisons.

## Build differences

The root and emulator CMake files define the emulation libraries, the `pcsx2x6_libretro` shared library, and optional unit tests. Multi-ISA compilation is the default so that one core runs across supported CPUs, and MSVC builds use the static CRT.

`libretro/deps/` builds and pins the static libraries the core links. Its revisions match the PCSX2x6 Windows dependency set, except for shaderc, which is pinned to a newer tag with a newer glslang revision as explained in that file. The recipe is adapted from the [PCEE2](https://github.com/WizzardSK/pcee2-libretro) dependency build at `5719627731beb90e379ff1be1223391b7f563b3b`. `libretro/libretro.h` is the libretro API header from the same PCEE2 revision.

## Resources and notices

Runtime resources under `bin/resources/` include `GameIndex.yaml`, `fonts/`, and `shaders/`. Retained third-party components keep their license and copyright notices. The upstream distribution notice is `bin/docs/ThirdPartyLicenses.html`.

The Windows source dependency `3rdparty/winpixeventruntime/` includes upstream-vendored import libraries and DLLs.

## Emulation changes

The first nine changes make the emulator safe to load, restart, and unload inside a frontend process, protect saved data, or make startup failures actionable, and are candidates for upstream contribution. The others serve this project: an SPU2 output stream factory and a display aspect query for frontend output, a frontend-selected SRAM save location and the single-library build.

| Change | Files |
| --- | --- |
| The page-fault handler can be removed; fastmem allocation installs it and fastmem release removes it | `common/HostSys.h`, `common/Windows/WinHostSys.cpp`, `common/Linux/LnxHostSys.cpp`, `common/Darwin/DarwinMisc.cpp`, `pcsx2/vtlb.cpp` |
| The VU1 thread is closed by the code that opened it instead of by a static destructor | `pcsx2/x86/microVU.cpp`, `pcsx2/VMManager.cpp` |
| A failed CPU-thread initialization releases what it acquired | `pcsx2/VMManager.cpp`, `pcsx2/VMManager.h` |
| A failed startup clears arcade identity, clocks, and media the same way shutdown does; the arcade image handle is cleared on close and never overwritten | `pcsx2/VMManager.cpp`, `pcsx2/DEV9/ACATA_IO.cpp` |
| Arcade SRAM is read without write access, limited to 32 KiB and closed on every path; only an image reported as absent starts blank, any other open or read failure keeps write protection for the session, an existing image is never recreated on save, a new image is written to a temporary file and renamed into place, and the SRAM path is reset at the start of each session | `pcsx2/DEV9/ACSRAM.cpp`, `pcsx2/VMManager.cpp` |
| The patch archive is opened for each lookup and closed when the lookup ends instead of being cached until process exit; source creation failures are reported as open failures, and archive entries are closed after reading | `pcsx2/Patch.cpp`, `common/ZipHelpers.h` |
| Opening arcade media sets the sector size from the session's declared media instead of keeping the previous session's value; the CHD sector size check compares against that declaration and is skipped with a warning for an unknown media type | `pcsx2/DEV9/ACATA_IO.cpp` |
| Arcade startup failures name the file involved: a missing or unreadable media image, a missing dongle or save card, with the full path of the expected file | `pcsx2/DEV9/ACATA_IO.cpp`, `pcsx2/VMManager.cpp` |
| A disabled input source is created without being initialized, instead of being initialized once and shut down again | `pcsx2/Input/InputManager.cpp` |
| SPU2 can create its output stream through a factory set by the host, and `AudioStream` subclasses can read how many frames are buffered | `pcsx2/SPU2/spu2.h`, `pcsx2/SPU2/spu2.cpp`, `pcsx2/Host/AudioStream.h` |
| A GS-thread query reports the display aspect ratio of the current output, from the aspect ratio setting and video mode | `pcsx2/GS/GS.h`, `pcsx2/GS/GS.cpp`, `pcsx2/GS/Renderers/Common/GSRenderer.h`, `pcsx2/GS/Renderers/Common/GSRenderer.cpp` |
| An optional boot parameter keeps arcade SRAM in `<directory>/<gameid>/sram.bin`, seeded from the manifest's image when no save exists; startup rejects a save location that is the source location. File identity comparison is shared through `FileSystem::IsSamePath` | `pcsx2/VMManager.h`, `pcsx2/VMManager.cpp`, `pcsx2/DEV9/ACSRAM.h`, `pcsx2/DEV9/ACSRAM.cpp`, `common/FileSystem.h`, `common/FileSystem.cpp` |
| The Windows-only `Optimus.cpp` export is removed because it only takes effect in an executable | `pcsx2/windows/Optimus.cpp`, `pcsx2/CMakeLists.txt` |
| shaderc is linked statically and its compiler is owned by the Vulkan shader cache instead of a runtime-loaded library released at process exit | `pcsx2/GS/Renderers/Vulkan/VKShaderCache.cpp`, `cmake/FindShaderc.cmake`, `pcsx2/CMakeLists.txt` |

## Version identity

The core must report both this project's build revision and the PCSX2x6 base revision above. Upstream CMake version detection alone reports the project checkout's revision.

## Additional upstream changes

None. Future adopted changes will identify the source project, revision, and integration purpose.
