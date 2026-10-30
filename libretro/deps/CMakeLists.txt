# Builds the third-party libraries the core links into an install prefix. Run it
# before configuring the core and pass the prefix through CMAKE_PREFIX_PATH:
#
#   cmake -S libretro/deps -B <deps-build> -G Ninja -DCMAKE_INSTALL_PREFIX=<prefix>
#   cmake --build <deps-build>
#
# Everything is static and uses the static CRT, so the core loads as a single
# file without the Visual C++ redistributable. Run from a Visual Studio x64
# developer prompt; Git and Python 3 are also required.
#
# Adapted from the PCEE2 dependency build. The revisions match the PCSX2x6
# Windows dependency set, except for shaderc as noted below.

cmake_minimum_required(VERSION 3.21)
project(pcsx2x6-libretro-deps C)

if(NOT MSVC)
	message(FATAL_ERROR "The dependency build currently supports MSVC only.")
endif()

include(ExternalProject)
include(ProcessorCount)

ProcessorCount(HOST_CORES)
if(HOST_CORES EQUAL 0)
	set(HOST_CORES 1)
endif()
set(NPROCS ${HOST_CORES} CACHE STRING "Parallel jobs to build each dependency with")

set(ZLIB_TAG v1.3.2)
set(LIBPNG_TAG v1.6.58)
set(ZSTD_TAG v1.5.7)
set(JPEGTURBO_TAG 3.1.4.1)
set(WEBP_TAG v1.6.0)
set(LZ4_TAG v1.10.0)
set(SDL_TAG release-3.4.10)
set(FREETYPE_TAG VER-2-14-3)
set(PLUTOVG_TAG v1.3.2)
set(PLUTOSVG_TAG v0.0.7)
set(RAPIDYAML_TAG v0.12.1)
# Newer than the shaderc pin of the PCSX2x6 dependency set. The SPIRV revisions are
# the ones shaderc's DEPS selects for this tag; glslang is ahead of DEPS and carries
# preprocessor memory-safety fixes that matter when shaders compile inside a
# frontend process.
set(SHADERC_TAG v2026.3)
set(GLSLANG_REVISION 90afccfbd49dff0349d86a41762e9de24e1df811)
set(SPIRV_HEADERS_REVISION 29981f65241605e08b0ede4cfeb999fe3b723c6a)
set(SPIRV_TOOLS_REVISION b707790a898e44038547df54580022fc1cf89c3d)

# CMAKE_POLICY_VERSION_MINIMUM can put CMP0091 back to OLD for projects asking
# for an old CMake, which would silently leave them on /MD. Set the policy itself.
set(COMMON_ARGS
	-DCMAKE_BUILD_TYPE=Release
	-DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
	-DCMAKE_PREFIX_PATH=${CMAKE_INSTALL_PREFIX}
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON
	-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
	-DCMAKE_POLICY_DEFAULT_CMP0091=NEW
)

set(SHALLOW_CLONE GIT_SHALLOW ON GIT_PROGRESS OFF UPDATE_COMMAND "")
set(BUILD_PARALLEL BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${NPROCS})
set(COPY_IF_EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/CopyIfExists.cmake")

ExternalProject_Add(zlib
	GIT_REPOSITORY https://github.com/madler/zlib
	GIT_TAG ${ZLIB_TAG}
	${SHALLOW_CLONE}
	CMAKE_ARGS ${COMMON_ARGS} -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_STATIC=ON
		-DZLIB_BUILD_TESTING=OFF -DZLIB_BUILD_MINIZIP=OFF
	${BUILD_PARALLEL}
	INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
	COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_INSTALL_PREFIX}/lib/zlibstatic.lib
		-DDST=${CMAKE_INSTALL_PREFIX}/lib/zlib.lib -P ${COPY_IF_EXISTS}
	COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_INSTALL_PREFIX}/lib/zs.lib
		-DDST=${CMAKE_INSTALL_PREFIX}/lib/zlib.lib -P ${COPY_IF_EXISTS}
)

ExternalProject_Add(libpng
	DEPENDS zlib
	GIT_REPOSITORY https://github.com/pnggroup/libpng
	GIT_TAG ${LIBPNG_TAG}
	${SHALLOW_CLONE}
	CMAKE_ARGS ${COMMON_ARGS} -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF
		-DPNG_TOOLS=OFF -DPNG_FRAMEWORK=OFF
	${BUILD_PARALLEL}
	INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
	COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_INSTALL_PREFIX}/lib/libpng16_static.lib
		-DDST=${CMAKE_INSTALL_PREFIX}/lib/libpng16.lib -P ${COPY_IF_EXISTS}
)

ExternalProject_Add(zstd
	GIT_REPOSITORY https://github.com/facebook/zstd
	GIT_TAG ${ZSTD_TAG}
	${SHALLOW_CLONE}
	SOURCE_SUBDIR build/cmake
	CMAKE_ARGS ${COMMON_ARGS} -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON
		-DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF
	${BUILD_PARALLEL}
)

ExternalProject_Add(libjpeg-turbo
	GIT_REPOSITORY https://github.com/libjpeg-turbo/libjpeg-turbo
	GIT_TAG ${JPEGTURBO_TAG}
	${SHALLOW_CLONE}
	CMAKE_ARGS ${COMMON_ARGS} -DENABLE_SHARED=OFF -DENABLE_STATIC=ON
		-DWITH_SIMD=OFF -DWITH_TURBOJPEG=OFF
	${BUILD_PARALLEL}
	INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
	COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_INSTALL_PREFIX}/lib/jpeg-static.lib
		-DDST=${CMAKE_INSTALL_PREFIX}/lib/jpeg.lib -P ${COPY_IF_EXISTS}
)

# libwebp installs SharpYUV as a second archive, while find_package(WebP) only
# names libwebp, so the two are merged into one.
find_program(LIB_EXE NAMES lib REQUIRED)
ExternalProject_Add(libwebp
	GIT_REPOSITORY https://github.com/webmproject/libwebp
	GIT_TAG ${WEBP_TAG}
	${SHALLOW_CLONE}
	CMAKE_ARGS ${COMMON_ARGS} -DBUILD_SHARED_LIBS=OFF
		-DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF
		-DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF
		-DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF
	${BUILD_PARALLEL}
	INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
	COMMAND ${LIB_EXE} /OUT:${CMAKE_INSTALL_PREFIX}/lib/webp_merged.lib
		${CMAKE_INSTALL_PREFIX}/lib/libwebp.lib ${CMAKE_INSTALL_PREFIX}/lib/libsharpyuv.lib
	COMMAND ${CMAKE_COMMAND} -E rename ${CMAKE_INSTALL_PREFIX}/lib/webp_merged.lib
		${CMAKE_INSTALL_PREFIX}/lib/libwebp.lib
)

ExternalProject_Add(lz4
	GIT_REPOSITORY https://github.com/lz4/lz4
	GIT_TAG ${LZ4_TAG}
	${SHALLOW_CLONE}
	SOURCE_SUBDIR build/cmake
	CMAKE_ARGS ${COMMON_ARGS} -DBUILD_SHARED_LIBS=OFF
		-DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF
	${BUILD_PARALLEL}
)

# A static-only install makes SDL3::SDL3 resolve to the archive.
ExternalProject_Add(sdl3
	GIT_REPOSITORY https://github.com/libsdl-org/SDL
	GIT_TAG ${SDL_TAG}
	${SHALLOW_CLONE}
	CMAKE_ARGS ${COMMON_ARGS} -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
	${BUILD_PARALLEL}
)

ExternalProject_Add(freetype
	DEPENDS libpng
	GIT_REPOSITORY https://github.com/freetype/freetype
	GIT_TAG ${FREETYPE_TAG}
	${SHALLOW_CLONE}
	CMAKE_ARGS ${COMMON_ARGS} -DBUILD_SHARED_LIBS=OFF -DFT_REQUIRE_ZLIB=TRUE -DFT_REQUIRE_PNG=TRUE
		-DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_BROTLI=TRUE -DFT_DISABLE_HARFBUZZ=TRUE
	${BUILD_PARALLEL}
)

ExternalProject_Add(plutovg
	GIT_REPOSITORY https://github.com/sammycage/plutovg
	GIT_TAG ${PLUTOVG_TAG}
	${SHALLOW_CLONE}
	CMAKE_ARGS ${COMMON_ARGS} -DBUILD_SHARED_LIBS=OFF -DPLUTOVG_BUILD_EXAMPLES=OFF
	${BUILD_PARALLEL}
)

ExternalProject_Add(plutosvg
	DEPENDS freetype plutovg
	GIT_REPOSITORY https://github.com/sammycage/plutosvg
	GIT_TAG ${PLUTOSVG_TAG}
	${SHALLOW_CLONE}
	CMAKE_ARGS ${COMMON_ARGS} -DBUILD_SHARED_LIBS=OFF
		-DPLUTOSVG_ENABLE_FREETYPE=ON -DPLUTOSVG_BUILD_EXAMPLES=OFF
	${BUILD_PARALLEL}
)

ExternalProject_Add(rapidyaml
	GIT_REPOSITORY https://github.com/biojppm/rapidyaml
	GIT_TAG ${RAPIDYAML_TAG}
	GIT_SUBMODULES_RECURSE ON
	${SHALLOW_CLONE}
	CMAKE_ARGS ${COMMON_ARGS} -DBUILD_SHARED_LIBS=OFF
	${BUILD_PARALLEL}
)

# shaderc has no install rule for the combined archive, so it is placed by hand.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
ExternalProject_Add(shaderc
	GIT_REPOSITORY https://github.com/google/shaderc
	GIT_TAG ${SHADERC_TAG}
	${SHALLOW_CLONE}
	PATCH_COMMAND ${CMAKE_COMMAND} -DTHIRD_PARTY_DIR=<SOURCE_DIR>/third_party
		-DGLSLANG_REVISION=${GLSLANG_REVISION} -DSPIRV_HEADERS_REVISION=${SPIRV_HEADERS_REVISION}
		-DSPIRV_TOOLS_REVISION=${SPIRV_TOOLS_REVISION}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/FetchShadercSources.cmake
	CMAKE_ARGS ${COMMON_ARGS} -DCMAKE_POLICY_VERSION_MINIMUM=3.5
		-DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON
		-DSHADERC_ENABLE_SHARED_CRT=OFF
		-DPython3_EXECUTABLE=${Python3_EXECUTABLE} -DPYTHON_EXECUTABLE=${Python3_EXECUTABLE}
	BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel ${NPROCS} --target shaderc_combined
	INSTALL_COMMAND ${CMAKE_COMMAND} -E copy <BINARY_DIR>/libshaderc/shaderc_combined.lib
		${CMAKE_INSTALL_PREFIX}/lib/shaderc_combined.lib
	COMMAND ${CMAKE_COMMAND} -E copy_directory <SOURCE_DIR>/libshaderc/include/shaderc
		${CMAKE_INSTALL_PREFIX}/include/shaderc
)
