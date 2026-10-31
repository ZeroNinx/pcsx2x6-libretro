# - Try to find SHADERC
# Once done this will define
#  SHADERC_FOUND - System has SHADERC
#  SHADERC_INCLUDE_DIR - The SHADERC include directory
#  SHADERC_LIBRARY - The static shaderc_combined library
#  Shaderc::shaderc_combined - Imported target that links it

find_path(
    SHADERC_INCLUDE_DIR shaderc/shaderc.h
    ${SHADERC_PATH_INCLUDES}
)

# The core links shaderc statically, so the combined archive that carries glslang
# and SPIRV-Tools is the only accepted form.
find_library(
    SHADERC_LIBRARY
    NAMES shaderc_combined
    PATHS ${ADDITIONAL_LIBRARY_PATHS} ${SHADERC_PATH_LIB}
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Shaderc DEFAULT_MSG
                                  SHADERC_LIBRARY SHADERC_INCLUDE_DIR)

if(SHADERC_FOUND AND NOT TARGET Shaderc::shaderc_combined)
    add_library(Shaderc::shaderc_combined STATIC IMPORTED)
    set_target_properties(Shaderc::shaderc_combined PROPERTIES
        IMPORTED_LOCATION ${SHADERC_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${SHADERC_INCLUDE_DIR}
    )
endif()

mark_as_advanced(SHADERC_INCLUDE_DIR SHADERC_LIBRARY)
