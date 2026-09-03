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
# the front panel compositor (needs zlib for the baked artwork's PNGs); no window of its own
find_package(ZLIB QUIET)
if(ZLIB_FOUND)
	set(PANEL_EMBEDS)
	foreach(art panel_base_p4 panel_atlas_p4 panel_base_p8 panel_atlas_p8)
		set(src ${CMAKE_CURRENT_SOURCE_DIR}/gui/${art}.png)
		set(dst ${CMAKE_CURRENT_BINARY_DIR}/gui/${art}_png.c)
		add_custom_command(OUTPUT ${dst}
			COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gui
			COMMAND ${CMAKE_COMMAND} -DINPUT=${src} -DOUTPUT=${dst} -DSYMBOL=${art}_png
			        -P ${CMAKE_CURRENT_SOURCE_DIR}/gui/embed.cmake
			DEPENDS ${src} ${CMAKE_CURRENT_SOURCE_DIR}/gui/embed.cmake
			COMMENT "embedding ${art}.png")
		list(APPEND PANEL_EMBEDS ${dst})
	endforeach()
	add_library(scemu_gui STATIC gui/panel.c gui/png.c gui/lcd_font.c ${PANEL_EMBEDS})
	target_include_directories(scemu_gui PUBLIC gui)
	target_link_libraries(scemu_gui PUBLIC scemu PRIVATE ZLIB::ZLIB)
	if(NOT MSVC)
		target_compile_options(scemu_gui PRIVATE -Wall -Wextra -Wno-unused-parameter)
	endif()
else()
	message(STATUS "gui skipped: needs zlib")
endif()

# ---------------------------------------------------------------------------
# PortAudio and PortMidi, built from the submodules as static libraries, for the players

if(SCEMU_BUILD_TOOLS)
	set(BUILD_SHARED_LIBS OFF)
	set(PA_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
	set(PA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
	set(PA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
	set(BUILD_PORTMIDI_TESTS OFF CACHE BOOL "" FORCE)
	set(BUILD_JAVA_NATIVE_INTERFACE OFF CACHE BOOL "" FORCE)
	set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
	add_subdirectory(portaudio EXCLUDE_FROM_ALL)
	add_subdirectory(portmidi EXCLUDE_FROM_ALL)
	set_target_properties(portaudio portmidi PROPERTIES C_EXTENSIONS ON)
	if(NOT MSVC)
		target_compile_options(portaudio PRIVATE -w)
		target_compile_options(portmidi PRIVATE -w)
	endif()
endif()

# scgui, the desktop player: the panel in a GTK 4 window, PortAudio and PortMidi for the outside world
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
	pkg_check_modules(GTK4 QUIET IMPORTED_TARGET gtk4)
endif()
if(SCEMU_BUILD_TOOLS AND TARGET scemu_gui AND GTK4_FOUND)
	add_executable(scgui app/scgui.c app/machine.c app/midi_io.c app/combos.c
		play/roms.c play/smf.c play/sjis.c play/audio.c play/session.c)
	target_include_directories(scgui PRIVATE play)
	target_link_libraries(scgui PRIVATE scemu_gui scemu ZLIB::ZLIB PkgConfig::GTK4 portaudio portmidi pthread)
	if(NOT MSVC)
		target_compile_options(scgui PRIVATE -Wall -Wextra -Wno-unused-parameter)
	endif()
elseif(SCEMU_BUILD_TOOLS)
	message(STATUS "scgui skipped: needs GTK 4 and zlib")
endif()

# scplay, the terminal player (PortAudio for audio, zlib to read ROM zips)

if(SCEMU_BUILD_TOOLS)
	find_package(ZLIB QUIET)
	if(ZLIB_FOUND)
		add_executable(scplay play/scplay.c play/roms.c play/smf.c play/sjis.c play/tui.c play/audio.c play/session.c)
		target_link_libraries(scplay PRIVATE scemu ZLIB::ZLIB portaudio)
		if(NOT MSVC)
			target_compile_options(scplay PRIVATE -Wall -Wextra -Wno-unused-parameter)
		endif()
	else()
		message(STATUS "scplay skipped: needs zlib")
	endif()
endif()
