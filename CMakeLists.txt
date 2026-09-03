cmake_minimum_required(VERSION 3.16)
project(scemu C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(SCEMU_BUILD_TOOLS "Build the command-line tools" ON)
option(SCEMU_EXTERNAL_EXEC_ALLOCATOR
	"Do not build sljit's executable-memory allocator; the integrator supplies one through scemu_config" OFF)
option(SCEMU_H8500_JIT "Build the H8/510 dynamic translator (the interpreter stays the fallback)" ON)

# ---------------------------------------------------------------------------
# sljit, built from the submodule as a static library

add_library(sljit STATIC sljit/sljit_src/sljitLir.c)
target_include_directories(sljit PUBLIC sljit/sljit_src)
target_compile_definitions(sljit PUBLIC SLJIT_CONFIG_AUTO=1)
if(SCEMU_EXTERNAL_EXEC_ALLOCATOR)
	target_compile_definitions(sljit PUBLIC SLJIT_EXECUTABLE_ALLOCATOR=0)
endif()
if(NOT MSVC)
	target_compile_options(sljit PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# the library

add_library(scemu STATIC
	lib/scemu.c
	lib/h8500.c
	lib/xp.c
	lib/lsp.c
	lib/sc88.c
	lib/gate_array.c
	lib/sub_hle.c
	lib/lcd.c
	lib/wave_rom.c
	lib/jit.c
	lib/midi_map.c
)
if(SCEMU_H8500_JIT)
	target_sources(scemu PRIVATE lib/h8500_jit.c)
	target_compile_definitions(scemu PUBLIC SCEMU_H8500_JIT=1)
endif()
target_include_directories(scemu PUBLIC lib)
target_link_libraries(scemu PRIVATE sljit)
if(NOT MSVC)
	target_link_libraries(scemu PUBLIC m)
endif()
if(NOT MSVC)
	target_compile_options(scemu PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-unused-function)
endif()

# ---------------------------------------------------------------------------
# tools

if(SCEMU_BUILD_TOOLS)
	add_executable(scemu-cli cli/scemu_cli.c)
	target_link_libraries(scemu-cli PRIVATE scemu)
endif()

# ---------------------------------------------------------------------------
# scplay, the terminal player (needs SDL2 for audio and zlib to read ROM zips)

if(SCEMU_BUILD_TOOLS)
	find_package(SDL2 QUIET)
	find_package(ZLIB QUIET)
	if(SDL2_FOUND AND ZLIB_FOUND)
		add_executable(scplay play/scplay.c play/roms.c play/smf.c play/tui.c play/audio.c)
		target_link_libraries(scplay PRIVATE scemu ZLIB::ZLIB)
		if(TARGET SDL2::SDL2)
			target_link_libraries(scplay PRIVATE SDL2::SDL2)
		else()
			target_include_directories(scplay PRIVATE ${SDL2_INCLUDE_DIRS})
			target_link_libraries(scplay PRIVATE ${SDL2_LIBRARIES})
		endif()
		if(NOT MSVC)
			target_compile_options(scplay PRIVATE -Wall -Wextra -Wno-unused-parameter)
		endif()
	else()
		message(STATUS "scplay skipped: needs SDL2 and zlib")
	endif()
endif()
