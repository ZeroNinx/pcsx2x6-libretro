#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0+
"""Package a Windows core and its source snapshot for a RetroArch installation."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile
import zipfile


DEPENDENCIES = (
    "zlib", "libpng", "zstd", "libjpeg-turbo", "libwebp", "lz4", "sdl3",
    "freetype", "plutovg", "plutosvg", "rapidyaml", "shaderc",
)


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args])


def dependency_files(root):
    for directory, dirs, files in os.walk(root):
        dirs[:] = sorted(d for d in dirs if d not in (".git", "__pycache__", "CMakeFiles"))
        for name in sorted(files):
            if name == ".git" or name.endswith((".pyc", ".obj", ".pdb")):
                continue
            yield Path(directory) / name


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--dependency-build-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    version = re.search(r'set\(PCSX2X6_LIBRETRO_VERSION "([^"]+)"\)',
                        (root / "libretro/CMakeLists.txt").read_text()).group(1)
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    archive = output / f"pcsx2x6-libretro-v{version}-windows-x64.zip"
    if archive.exists():
        raise SystemExit(f"Refusing to replace existing package: {archive}")

    sources = {}
    for name in DEPENDENCIES:
        path = args.dependency_build_dir.resolve() / f"{name}-prefix" / "src" / name
        if not path.is_dir():
            raise SystemExit(f"Missing dependency source: {path}")
        sources[name] = path
    for path in (sources["rapidyaml"] / "ext/c4core/CMakeLists.txt",
                 sources["shaderc"] / "third_party/glslang/CMakeLists.txt",
                 sources["shaderc"] / "third_party/spirv-headers/CMakeLists.txt",
                 sources["shaderc"] / "third_party/spirv-tools/CMakeLists.txt"):
        if not path.is_file():
            raise SystemExit(f"Missing nested dependency source: {path}")

    with tempfile.TemporaryDirectory(prefix="pcsx2x6-package-", dir=output) as temporary:
        stage = Path(temporary)
        subprocess.run(["cmake", "--install", str(args.build_dir.resolve()),
                        "--component", "Libretro", "--prefix", str(stage)], check=True)
        core = stage / "cores/pcsx2x6_libretro.dll"
        if not core.is_file():
            raise SystemExit("The install did not produce a Windows core DLL.")

        source_dir = stage / "docs/pcsx2x6/sources"
        source_dir.mkdir()
        paths = sorted(set(git(root, "ls-files", "--cached", "--others",
                               "--exclude-standard", "-z").decode().split("\0")) - {""})
        manifest = {"version": version, "revision": git(root, "rev-parse", "HEAD").decode().strip(),
                    "core_sha256": hashlib.sha256(core.read_bytes()).hexdigest(), "source_sha256": {}}
        # Include the exact working files, not a git archive that would omit local changes.
        with tarfile.open(source_dir / f"pcsx2x6-libretro-v{version}-source.tar.gz", "w:gz") as tar:
            for name in paths:
                path = root / name
                if path.is_file():
                    tar.add(path, arcname="pcsx2x6-libretro/" + name, recursive=False)
                    manifest["source_sha256"][name] = hashlib.sha256(path.read_bytes()).hexdigest()
            for name, path in sources.items():
                for file in dependency_files(path):
                    tar.add(file, arcname=f"dependencies/{name}/" + file.relative_to(path).as_posix(), recursive=False)
        (source_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        (source_dir / "BUILDING.md").write_text(
            "# Package sources\n\n"
            "The source archive contains the core's working-tree snapshot and the dependency "
            "source trees used by the build, including nested dependencies and their licenses. "
            "The manifest identifies the binary and hashes the project source files. "
            "Extract it separately from the RetroArch installation.\n\n"
            "Build instructions and dependency build recipes are in the source tree's root "
            "README and libretro/deps/. Standard dependency configuration fetches the pinned "
            "revisions; the included dependency sources are also available for rebuilding. "
            "This source archive must accompany redistribution of this test binary.\n",
            encoding="utf-8")
        # Sources are already compressed; store the inner archive without recompressing it.
        with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as package:
            for path in sorted(stage.rglob("*")):
                if path.is_file():
                    method = zipfile.ZIP_STORED if path.name.endswith(".tar.gz") else zipfile.ZIP_DEFLATED
                    package.write(path, path.relative_to(stage).as_posix(), compress_type=method)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix(".zip.sha256").write_text(f"{digest}  {archive.name}\n", encoding="ascii")
    print(f"Package: {archive}\nSHA-256: {digest}")


if __name__ == "__main__":
    main()
