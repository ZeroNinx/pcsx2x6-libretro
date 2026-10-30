# cmake -DSRC=<file> -DDST=<file> -P CopyIfExists.cmake
#
# MSVC builds of zlib, libpng and libjpeg-turbo name their static libraries
# differently depending on the version and enabled options, while find_package()
# looks for the plain name. `cmake -E copy` would fail on the variant that was
# not produced.

if(EXISTS "${SRC}")
	configure_file("${SRC}" "${DST}" COPYONLY)
endif()
