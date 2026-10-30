add_library(pcsx2x6_libretro SHARED)

set_target_properties(pcsx2x6_libretro PROPERTIES
	PREFIX ""
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

target_sources(pcsx2x6_libretro PRIVATE
	Libretro.cpp
	LibretroHost.cpp
	LibretroHost.h
	libretro.h
)

target_include_directories(pcsx2x6_libretro PRIVATE
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
)

# The emulation base recorded in docs/upstream.md. GIT_REV only identifies this
# repository's own revision.
target_compile_definitions(pcsx2x6_libretro PRIVATE PCSX2X6_BASE_REVISION="583f0e909")

target_link_libraries(pcsx2x6_libretro PRIVATE
	PCSX2_FLAGS
	PCSX2
)
