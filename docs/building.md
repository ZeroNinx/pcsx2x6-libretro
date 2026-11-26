# Building and Packaging

The supported build target is Windows x64 with Visual Studio 2022, CMake, Ninja, Git and Python 3. Run the commands below from the repository root in a Visual Studio x64 developer prompt.

## Build

Choose an absolute path for `<dependency-prefix>` and use the same path in both configure commands.

```text
cmake -S libretro/deps -B build/deps -G Ninja -DCMAKE_INSTALL_PREFIX=<dependency-prefix>
cmake --build build/deps
cmake -S . -B build/core -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<dependency-prefix>
cmake --build build/core --target pcsx2x6_libretro
```

The dependency build fetches the pinned sources described in `libretro/deps/`. The core DLL is written to `build/core/bin/pcsx2x6_libretro.dll`.

## Install

Stage the files for a RetroArch installation:

```text
cmake --install build/core --component Libretro --prefix build/package
```

This installs the core, `.info`, runtime resources and documentation. Copy them to the corresponding RetroArch directories as described in the [usage guide](usage.md#install-or-update).

## Package

Create a ZIP with the runtime files, notices and corresponding source:

```text
python libretro/package.py --build-dir build/core --dependency-build-dir build/deps --output-dir build/dist
```

The package includes the project's source snapshot, dependency sources and a manifest. Keep these materials and license notices with binary redistributions. BIOS files, game files, dongles and the external patches archive are not included.
