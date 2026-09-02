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
	lib/xp_dsp.c
	lib/lsp.c
	lib/sc88.c
	lib/gate_array.c
	lib/sub_hle.c
	lib/lcd.c
	lib/wave_rom.c
	lib/jit.c
)
target_include_directories(scemu PUBLIC lib)
target_link_libraries(scemu PRIVATE sljit)
if(NOT MSVC)
	target_compile_options(scemu PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-unused-function)
endif()

# ---------------------------------------------------------------------------
# tools

if(SCEMU_BUILD_TOOLS)
	add_executable(scemu-cli cli/scemu_cli.c)
	target_link_libraries(scemu-cli PRIVATE scemu)
endif()
