# Upstream Sources

## Emulation base

| Project | Revision | Integration |
| --- | --- | --- |
| [PCSX2x6](https://github.com/PS2Homebrew-arcade/pcsx2x6) | `583f0e90907d69bb2db670f570d0865260fcc71b` | Selected source snapshot; upstream Git history is not merged |

Retained emulation, resource, and third-party source files match this revision. Project build configuration is adapted for the libretro integration. Source paths are preserved for upstream comparisons.

## Build differences

The root and emulator CMake files define the emulation libraries and optional unit tests. Dependency discovery and build helpers support these targets. Core dependency acquisition and version records will be defined with the libretro build target.

## Resources and notices

Runtime resources under `bin/resources/` include `GameIndex.yaml`, `fonts/`, and `shaders/`. Retained third-party components keep their license and copyright notices. The upstream distribution notice is `bin/docs/ThirdPartyLicenses.html`.

The Windows source dependency `3rdparty/winpixeventruntime/` includes upstream-vendored import libraries and DLLs.

## Version identity

The core must report both this project's build revision and the PCSX2x6 base revision above. Upstream CMake version detection alone reports the project checkout's revision.

## Additional upstream changes

None. Future adopted changes will identify the source project, revision, and integration purpose.
