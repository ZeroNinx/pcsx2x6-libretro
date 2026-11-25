# Upstream Sources

## Emulation base

| Project | Revision | Integration |
| --- | --- | --- |
| [PCSX2x6](https://github.com/PS2Homebrew-arcade/pcsx2x6) | `583f0e90907d69bb2db670f570d0865260fcc71b` | Selected source snapshot; upstream Git history is not merged |

Retained resource and third-party source files match this revision. Emulation source matches it except for the project changes listed below. Project build configuration is adapted for the libretro integration. Source paths are preserved for upstream comparisons.

## Build differences

The root and emulator CMake files define the emulation libraries, the `pcsx2x6_libretro` shared library, and optional unit tests. Multi-ISA compilation is the default so that one core runs across supported CPUs, and MSVC builds use the static CRT.

`libretro/deps/` builds and pins the static libraries the core links. Its revisions match the PCSX2x6 Windows dependency set, except for shaderc, which is pinned to a newer tag with a newer glslang revision as explained in that file. The recipe is adapted from the [PCEE2](https://github.com/WizzardSK/pcee2-libretro) dependency build at `5719627731beb90e379ff1be1223391b7f563b3b`. `libretro/libretro.h` and `libretro/libretro_vulkan.h` are the libretro API headers from the same PCEE2 revision, with their original notices preserved. `libretro/libretro_d3d11.h` is the [RetroArch](https://github.com/libretro/RetroArch) `v1.22.2` copy of `libretro-common/include/libretro_d3d11.h` (render interface version 1), unmodified with its original notice.

## Resources and notices

Runtime resources under `bin/resources/` include `GameIndex.yaml`, `fonts/`, and `shaders/`. Retained third-party components keep their license and copyright notices. The upstream distribution notice is `bin/docs/ThirdPartyLicenses.html`.

The Windows source dependency `3rdparty/winpixeventruntime/` includes upstream-vendored import libraries and DLLs.

## Emulation changes

The first nine changes make the emulator safe to load, restart, and unload inside a frontend process, protect saved data, or make startup failures actionable, and are candidates for upstream contribution. The others serve this project: audio/video output hooks, frontend-selected storage locations, selective directory creation, the single-library build, in-memory save states, process-termination handling and the D3D11 adapter selection.

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
| The Vulkan GS can use a host-owned instance and negotiated device, load functions through the host resolver, serialize graphics-queue access and disable the spin queue; failed negotiation releases objects not handed to the host | `pcsx2/GS/Renderers/Vulkan/GSDeviceVK.h`, `pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp`, `pcsx2/GS/Renderers/Vulkan/VKLoader.h`, `pcsx2/GS/Renderers/Vulkan/VKLoader.cpp` |
| GS-thread queries report the current output dimensions and display aspect ratio; a draw operation renders the composite into a host target using the snapshot source region, sampling and opaque-alpha conversion | `pcsx2/GS/GS.h`, `pcsx2/GS/GS.cpp`, `pcsx2/GS/Renderers/Common/GSRenderer.h`, `pcsx2/GS/Renderers/Common/GSRenderer.cpp` |
| Vulkan texture creation accepts optional image creation flags for frontend-shared output targets | `pcsx2/GS/Renderers/Vulkan/GSTextureVK.h`, `pcsx2/GS/Renderers/Vulkan/GSTextureVK.cpp` |
| The D3D11 GS can be created on an adapter the host selects by LUID, failing when that adapter is absent, so its shared output textures can be opened by the frontend's device; adapter enumeration errors are reported and terminate enumeration | `pcsx2/GS/Renderers/DX11/GSDevice11.h`, `pcsx2/GS/Renderers/DX11/GSDevice11.cpp` |
| An optional boot parameter keeps arcade SRAM in `<directory>/<gameid>/sram.bin`, seeded from the manifest's image when no save exists; startup rejects a save location that is the source location. File identity comparison is shared through `FileSystem::IsSamePath` | `pcsx2/VMManager.h`, `pcsx2/VMManager.cpp`, `pcsx2/DEV9/ACSRAM.h`, `pcsx2/DEV9/ACSRAM.cpp`, `common/FileSystem.h`, `common/FileSystem.cpp` |
| An optional dongle directory separates slot-1 firmware from save cards; absolute memory-card filenames are respected. Startup can load folder settings without creating unused directories | `pcsx2/VMManager.h`, `pcsx2/VMManager.cpp`, `pcsx2/Pcsx2Config.cpp` |
| The Windows-only `Optimus.cpp` export is removed because it only takes effect in an executable | `pcsx2/windows/Optimus.cpp`, `pcsx2/CMakeLists.txt` |
| shaderc is linked statically and its compiler is owned by the Vulkan shader cache instead of a runtime-loaded library released at process exit | `pcsx2/GS/Renderers/Vulkan/VKShaderCache.cpp`, `cmake/FindShaderc.cmake`, `pcsx2/CMakeLists.txt` |
| In-memory save states: the shared state buffer accepts a fixed, caller-owned span as well as growable storage, and every read, write, block and nested stream is bounds-checked. Freeze functions record the fields used as lengths, indices or offsets with their limits, so a complete state is validated before anything is restored. A segmented memory interface measures, saves, validates and loads states; EE, IOP and arcade RAM are stored as page bitmaps with their non-zero pages. The desktop ZIP format and its loader are unchanged | `pcsx2/SaveState.h`, `pcsx2/SaveState.cpp`, `pcsx2/StateWrapper.h`, `pcsx2/StateWrapper.cpp`, and the freeze functions in `pcsx2/Counters.cpp`, `pcsx2/IopCounters.cpp`, `pcsx2/Memory.cpp`, `pcsx2/VUmicroMem.cpp`, `pcsx2/Vif.cpp`, `pcsx2/Sif.cpp`, `pcsx2/IPU/IPU.cpp`, `pcsx2/Gif.cpp`, `pcsx2/Gif_Unit.cpp`, `pcsx2/CDVD/CDVD.cpp`, `pcsx2/CDVD/Ps1CD.cpp`, `pcsx2/IopBios.cpp` |
| Arcade device state previously outside save states (ATA registers with PIO and pending DMA transfers, ATAPI packet transfers, arcade RAM banking, the JVS bus and UART registers) is saved in its own segment with a validator; PAD, SIO0/SIO2 and multitap state is checked before loading, and memory-card and dongle content fingerprints let a state be refused when those files changed | `pcsx2/DEV9/ACSTATE.h`, `pcsx2/DEV9/ACSTATE.cpp`, `pcsx2/DEV9/ACATA.*`, `pcsx2/DEV9/ACATAPI.*`, `pcsx2/DEV9/ACATA_IO_CHD.*`, `pcsx2/DEV9/ACJV.*`, `pcsx2/SIO/Pad/Pad.*`, `pcsx2/SIO/Sio0.*`, `pcsx2/SIO/Sio2.*`, `pcsx2/SIO/Multitap/MultitapProtocol.*`, `pcsx2/SIO/Memcard/MemoryCardFile.*`, `pcsx2/CMakeLists.txt` |
| SPU2 saves its partly filled output block, DC filter and decoded voice blocks with its state, and checks DMA ranges, voice positions and reverb addresses before loading. The GS reads its texture cache back for a state and validates a saved GS block before loading it. The active patch set has a fingerprint for state identity | `pcsx2/SPU2/spu2.h`, `pcsx2/SPU2/spu2.cpp`, `pcsx2/SPU2/spu2freeze.cpp`, `pcsx2/GS/GS.h`, `pcsx2/GS/GS.cpp`, `pcsx2/GS/GSState.h`, `pcsx2/GS/GSState.cpp`, `pcsx2/Patch.h`, `pcsx2/Patch.cpp` |
| A state loaded with MTVU enabled restores VU1 memory and registers first and rebuilds the VU1 thread afterwards, applying the saved thread counters | `pcsx2/MTVU.h`, `pcsx2/MTVU.cpp` |
| A restore that fails after it started changing the VM marks persistent data untrusted, so SRAM and BIOS NVRAM are not written back for that session; a complete restore clears the mark | `pcsx2/VMManager.h`, `pcsx2/VMManager.cpp` |
| The GS thread handle can be released while the process terminates, after the system ended the thread, so the static thread object is destroyed without reporting an unjoined thread; normal shutdown still joins it | `pcsx2/MTGS.h`, `pcsx2/MTGS.cpp` |

## Version identity

The core must report both this project's build revision and the PCSX2x6 base revision above. Upstream CMake version detection alone reports the project checkout's revision.

## Additional upstream changes

| Source | Change | Purpose |
| --- | --- | --- |
| [PCSX2 `24eea4392`](https://github.com/PCSX2/pcsx2/commit/24eea4392) | The D3D11 device initializes its null texture pointer and clears it after deletion | Early device-creation failure does not delete an uninitialized pointer during cleanup |
