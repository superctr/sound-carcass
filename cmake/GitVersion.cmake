# In script mode (-P) CMake overrides CMAKE_SOURCE_DIR with the
# current working directory, so the caller passes -DSOURCE_DIR=
# instead.
# Source tarballs carry no .git; the packaging script records the
# version in .tarball-version instead.
if (EXISTS ${SOURCE_DIR}/.tarball-version)
	file(READ ${SOURCE_DIR}/.tarball-version GIT_VERSION)
	string(STRIP "${GIT_VERSION}" GIT_VERSION)
else()
	find_package(Git QUIET)

	if (GIT_FOUND)
		execute_process(
			COMMAND ${GIT_EXECUTABLE} describe --always --dirty
			WORKING_DIRECTORY ${SOURCE_DIR}
			OUTPUT_VARIABLE GIT_VERSION
			OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET
			RESULT_VARIABLE GIT_RESULT)
	endif()

	if (NOT GIT_FOUND OR NOT GIT_RESULT EQUAL 0)
		set(GIT_VERSION "unknown")
	endif()
endif()

set(NEW_CONTENT "#pragma once
static const char* GIT_VERSION = \"${GIT_VERSION}\";
")

if (EXISTS "${OUTPUT_FILE}")
	file(READ "${OUTPUT_FILE}" OLD_CONTENT)
	if ("${NEW_CONTENT}" STREQUAL "${OLD_CONTENT}")
		return()
	endif()
endif()

file(WRITE "${OUTPUT_FILE}" "${NEW_CONTENT}")
