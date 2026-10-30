# Upstream Sources

## Emulation base

| Project | Revision | Integration |
| --- | --- | --- |
| [PCSX2x6](https://github.com/PS2Homebrew-arcade/pcsx2x6) | `583f0e90907d69bb2db670f570d0865260fcc71b` | Selected source snapshot; upstream Git history is not merged |

Retained emulation, resource, and third-party source files match this revision. Project build configuration is adapted for the libretro integration. Source paths are preserved for upstream comparisons.

## Build differences

The root and emulator CMake files define the emulation libraries, the `pcsx2x6_libretro` shared library, and optional unit tests. Multi-ISA compilation is the default so that one core runs across supported CPUs, and MSVC builds use the static CRT.

`libretro/deps/` builds and pins the static libraries the core links. Its revisions match the PCSX2x6 Windows dependency set, except for shaderc, which is pinned to a newer tag with a newer glslang revision as explained in that file. The recipe is adapted from the [PCEE2](https://github.com/WizzardSK/pcee2-libretro) dependency build at `5719627731beb90e379ff1be1223391b7f563b3b`. `libretro/libretro.h` is the libretro API header from the same PCEE2 revision.

## Resources and notices

Runtime resources under `bin/resources/` include `GameIndex.yaml`, `fonts/`, and `shaders/`. Retained third-party components keep their license and copyright notices. The upstream distribution notice is `bin/docs/ThirdPartyLicenses.html`.

The Windows source dependency `3rdparty/winpixeventruntime/` includes upstream-vendored import libraries and DLLs.

## Version identity

The core must report both this project's build revision and the PCSX2x6 base revision above. Upstream CMake version detection alone reports the project checkout's revision.

## Additional upstream changes

None. Future adopted changes will identify the source project, revision, and integration purpose.
