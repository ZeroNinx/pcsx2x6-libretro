set(PCSX2X6_LIBRETRO_VERSION "0.1")
add_library(pcsx2x6_libretro SHARED)

set_target_properties(pcsx2x6_libretro PROPERTIES
	PREFIX ""
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

target_sources(pcsx2x6_libretro PRIVATE
	Libretro.cpp
	LibretroAudio.cpp
	LibretroAudio.h
	LibretroHost.cpp
	LibretroHost.h
	LibretroInput.cpp
	LibretroInput.h
	LibretroSession.cpp
	LibretroSession.h
	LibretroVulkan.cpp
	LibretroVulkan.h
	libretro.h
	libretro_vulkan.h
)

target_include_directories(pcsx2x6_libretro PRIVATE
	# libretro_vulkan.h includes <libretro.h>.
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
)

# The emulation base recorded in docs/upstream.md. GIT_REV only identifies this
# repository's own revision.
target_compile_definitions(pcsx2x6_libretro PRIVATE
	PCSX2X6_BASE_REVISION="583f0e909"
	PCSX2X6_LIBRETRO_VERSION="${PCSX2X6_LIBRETRO_VERSION}"
)

target_link_libraries(pcsx2x6_libretro PRIVATE
	PCSX2_FLAGS
	PCSX2
)

# A component install avoids installing build-time dependencies into RetroArch.
install(TARGETS pcsx2x6_libretro
	RUNTIME DESTINATION cores COMPONENT Libretro
	LIBRARY DESTINATION cores COMPONENT Libretro)
install(FILES pcsx2x6_libretro.info DESTINATION info COMPONENT Libretro)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/bin/resources/" DESTINATION system/pcsx2x6/resources
	COMPONENT Libretro PATTERN "patches.zip" EXCLUDE)
install(FILES "${CMAKE_SOURCE_DIR}/README.md" "${CMAKE_SOURCE_DIR}/COPYING.GPLv3"
	DESTINATION docs/pcsx2x6 COMPONENT Libretro)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/docs/" DESTINATION docs/pcsx2x6/docs
	COMPONENT Libretro FILES_MATCHING PATTERN "*.md")
install(FILES "${CMAKE_SOURCE_DIR}/bin/docs/ThirdPartyLicenses.html"
	DESTINATION docs/pcsx2x6/licenses COMPONENT Libretro)
