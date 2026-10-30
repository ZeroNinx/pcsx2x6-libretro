# Places the sources needed for shaderc's combined library under third_party/.
# Only these three repositories are required once shaderc's tests are skipped.
# Each is fetched as a single pinned commit and retried, since one dropped
# connection would otherwise surface much later as a confusing configure error.
#
# Run as: cmake -DTHIRD_PARTY_DIR=... -DGLSLANG_REVISION=... -DSPIRV_HEADERS_REVISION=...
#         -DSPIRV_TOOLS_REVISION=... -P FetchShadercSources.cmake

foreach(required THIRD_PARTY_DIR GLSLANG_REVISION SPIRV_HEADERS_REVISION SPIRV_TOOLS_REVISION)
	if(NOT ${required})
		message(FATAL_ERROR "${required} is required")
	endif()
endforeach()

find_package(Git REQUIRED)

set(FETCH_ATTEMPTS 5)

function(fetch_revision name url revision)
	set(directory "${THIRD_PARTY_DIR}/${name}")
	file(REMOVE_RECURSE "${directory}")
	file(MAKE_DIRECTORY "${directory}")
	execute_process(COMMAND ${GIT_EXECUTABLE} -C "${directory}" init --quiet COMMAND_ERROR_IS_FATAL ANY)

	foreach(attempt RANGE 1 ${FETCH_ATTEMPTS})
		message(STATUS "Fetching ${name} at ${revision} (attempt ${attempt}/${FETCH_ATTEMPTS})")
		execute_process(COMMAND ${GIT_EXECUTABLE} -C "${directory}" fetch --quiet --depth 1 "${url}" ${revision}
			RESULT_VARIABLE fetch_result)
		if(fetch_result EQUAL 0)
			execute_process(COMMAND ${GIT_EXECUTABLE} -C "${directory}" checkout --quiet --detach FETCH_HEAD
				COMMAND_ERROR_IS_FATAL ANY)
			return()
		endif()
		execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 5)
	endforeach()

	message(FATAL_ERROR "Failed to fetch ${name} from ${url}")
endfunction()

fetch_revision(glslang https://github.com/KhronosGroup/glslang.git ${GLSLANG_REVISION})
fetch_revision(spirv-headers https://github.com/KhronosGroup/SPIRV-Headers.git ${SPIRV_HEADERS_REVISION})
fetch_revision(spirv-tools https://github.com/KhronosGroup/SPIRV-Tools.git ${SPIRV_TOOLS_REVISION})
