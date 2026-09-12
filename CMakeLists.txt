cmake_minimum_required(VERSION 3.16)
project(scemu C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# everything goes into the plugin, a shared object, as much as into the tools
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(SCEMU_BUILD_TOOLS "Build the command-line tools" ON)
option(SCEMU_EXTERNAL_EXEC_ALLOCATOR
	"Do not build sljit's executable-memory allocator; the integrator supplies one through scemu_config" OFF)
option(SCEMU_H8500_JIT "Build the H8/510 dynamic translator (the interpreter stays the fallback)" ON)
option(SCEMU_BUILD_PLUGIN "Build the CLAP plugin" ON)

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
	lib/sh2.c
	lib/sh2_jit.c
	lib/xp.c
	lib/gp.c
	lib/lsp.c
	lib/sc88.c
	lib/sc8850.c
	lib/sc8820.c
	lib/uipc.c
	lib/sc55.c
	lib/sc55mk2.c
	lib/i8251.c
	lib/state.c
	lib/midi_queue.c
	lib/sub_hle.c
	lib/sub55_hle.c
	lib/lcd.c
	lib/glcd.c
	lib/flash.c
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
# the players' settings file, its reader lexed by re2c

find_program(RE2C re2c)
if(RE2C)
	add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/play/config.c
		COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/play
		COMMAND ${RE2C} -W --no-generation-date
		        -o ${CMAKE_CURRENT_BINARY_DIR}/play/config.c
		        ${CMAKE_CURRENT_SOURCE_DIR}/play/config.re
		DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/play/config.re
		COMMENT "re2c play/config.re")
	add_library(scemu_config STATIC ${CMAKE_CURRENT_BINARY_DIR}/play/config.c)
	target_include_directories(scemu_config PUBLIC play)
	if(NOT MSVC)
		target_compile_options(scemu_config PRIVATE -Wall -Wextra)
	endif()
else()
	message(STATUS "the settings file skipped: needs re2c")
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
	set(PANEL_ART)
	foreach(model sc88pro sc88 sc88vl sc55mk2 sc55)
		foreach(pitch 4 8)
			list(APPEND PANEL_ART panel_${model}_base_p${pitch} panel_${model}_atlas_p${pitch})
		endforeach()
	endforeach()
	foreach(pitch 3 6)   # the SC-8850's glass is a 160 x 64 dot panel, so its own pitches
		list(APPEND PANEL_ART panel_sc8850_base_p${pitch} panel_sc8850_atlas_p${pitch})
	endforeach()
	foreach(pitch 7 14)  # the SC-8820 has no glass; its pitch is pixels per millimetre
		list(APPEND PANEL_ART panel_sc8820_base_p${pitch} panel_sc8820_atlas_p${pitch})
	endforeach()
	foreach(art ${PANEL_ART})
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
	add_library(scemu_gui STATIC gui/panel.c gui/png.c gui/lcd_font.c gui/combos.c gui/controls.c gui/text.c gui/menu.c
		${PANEL_EMBEDS})
	target_include_directories(scemu_gui PUBLIC gui)
	target_link_libraries(scemu_gui PUBLIC scemu PRIVATE ZLIB::ZLIB)
	if(NOT MSVC)
		target_compile_options(scemu_gui PRIVATE -Wall -Wextra -Wno-unused-parameter)
	endif()
else()
	message(STATUS "gui skipped: needs zlib")
endif()

# ---------------------------------------------------------------------------
# PortAudio, PortMidi and libsamplerate, built from the submodules as static
# libraries, for the players; the library itself resamples nothing

if(SCEMU_BUILD_TOOLS)
	set(BUILD_SHARED_LIBS OFF)
	set(PA_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
	set(PA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
	set(PA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
	set(BUILD_PORTMIDI_TESTS OFF CACHE BOOL "" FORCE)
	set(BUILD_JAVA_NATIVE_INTERFACE OFF CACHE BOOL "" FORCE)
	set(BUILD_TESTING OFF)              # libsamplerate's tests, through the CTest module
	set(LIBSAMPLERATE_EXAMPLES OFF)     # they want libsndfile
	set(LIBSAMPLERATE_INSTALL OFF)
	set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
	add_subdirectory(portaudio EXCLUDE_FROM_ALL)
	add_subdirectory(portmidi EXCLUDE_FROM_ALL)
	set(CMAKE_POLICY_VERSION_MINIMUM 3.5)   # libsamplerate asks for 3.1
	add_subdirectory(libsamplerate EXCLUDE_FROM_ALL)
	unset(CMAKE_POLICY_VERSION_MINIMUM)
	set_target_properties(portaudio portmidi PROPERTIES C_EXTENSIONS ON)
	if(NOT MSVC)
		target_compile_options(portaudio PRIVATE -w)
		target_compile_options(portmidi PRIVATE -w)
		target_compile_options(samplerate PRIVATE -w)
	endif()
endif()

# what the players and the plugin share around the library: the ROM sets, the
# boot cache and settings memory, a unit short of a clock, MIDI files, the
# resampler (zlib reads ROM zips)
if(ZLIB_FOUND)
	add_library(scemu_play STATIC play/roms.c play/session.c play/unit.c play/smf.c play/sjis.c play/resample.c)
	target_include_directories(scemu_play PUBLIC play)
	target_link_libraries(scemu_play PUBLIC scemu ZLIB::ZLIB samplerate)
	if(NOT MSVC)
		target_compile_options(scemu_play PRIVATE -Wall -Wextra -Wno-unused-parameter)
	endif()
endif()

# the CLAP plugin: one instrument per model, in a shared object the host loads
if(SCEMU_BUILD_PLUGIN AND TARGET scemu_play AND TARGET scemu_gui)
	# PUGL, from the submodule: the plugin's window, drawn with OpenGL
	find_package(OpenGL QUIET)
	if(WIN32)
		set(PUGL_PLATFORM pugl/src/win.c pugl/src/win_gl.c)
		set(PUGL_LIBS gdi32 user32 shlwapi dwmapi)
		set(PUGL_FOUND ${OpenGL_FOUND})
	else()
		find_package(X11 QUIET)
		set(PUGL_PLATFORM pugl/src/x11.c pugl/src/x11_gl.c)
		set(PUGL_LIBS ${X11_LIBRARIES} ${X11_Xext_LIB})
		set(PUGL_FOUND ${OpenGL_FOUND})
		if(NOT X11_FOUND OR NOT X11_Xext_FOUND)
			set(PUGL_FOUND FALSE)
		endif()
	endif()
	if(PUGL_FOUND)
		add_library(pugl STATIC pugl/src/common.c pugl/src/internal.c ${PUGL_PLATFORM})
		target_include_directories(pugl PUBLIC pugl/include)
		target_compile_definitions(pugl PUBLIC PUGL_STATIC PRIVATE PUGL_INTERNAL USE_XRANDR=0 USE_XCURSOR=0)
		if(NOT WIN32)
			target_compile_definitions(pugl PRIVATE _POSIX_C_SOURCE=200809L)
		endif()
		target_link_libraries(pugl PUBLIC ${PUGL_LIBS} OpenGL::GL)
		if(NOT MSVC)
			target_compile_options(pugl PRIVATE -w)
		endif()
		add_library(scemu_clap MODULE plugin/scemu_clap.c plugin/window.c)
		set_target_properties(scemu_clap PROPERTIES OUTPUT_NAME scemu PREFIX "" SUFFIX ".clap"
			C_VISIBILITY_PRESET hidden)
		target_include_directories(scemu_clap PRIVATE clap/include)
		target_link_libraries(scemu_clap PRIVATE scemu_play scemu_gui scemu pugl pthread)
		if(NOT MSVC)
			target_compile_options(scemu_clap PRIVATE -Wall -Wextra -Wno-unused-parameter)
		endif()
		if(CMAKE_SYSTEM_NAME STREQUAL "Linux")   # only clap_entry leaves the shared object
			target_link_options(scemu_clap PRIVATE -Wl,--exclude-libs,ALL)
		endif()
	else()
		message(STATUS "the plugin skipped: needs OpenGL and X11")
	endif()
elseif(SCEMU_BUILD_PLUGIN)
	message(STATUS "the plugin skipped: needs zlib")
endif()

# scgui, the desktop player: the panel in a GTK 4 window, PortAudio and PortMidi for the outside world
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
	pkg_check_modules(GTK4 QUIET IMPORTED_TARGET gtk4)
endif()
if(SCEMU_BUILD_TOOLS AND TARGET scemu_gui AND TARGET scemu_config AND GTK4_FOUND)
	add_executable(scgui app/scgui.c app/machine.c app/midi_io.c play/audio.c)
	add_custom_target(git_version
		COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
		        -DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/app/git_version.h
		        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GitVersion.cmake
		BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/app/git_version.h
		COMMENT "the build's git version")
	add_dependencies(scgui git_version)
	target_include_directories(scgui PRIVATE play ${CMAKE_CURRENT_BINARY_DIR}/app)
	target_link_libraries(scgui PRIVATE scemu_gui scemu_play scemu scemu_config ZLIB::ZLIB PkgConfig::GTK4 portaudio
		portmidi samplerate pthread)
	if(NOT MSVC)
		target_compile_options(scgui PRIVATE -Wall -Wextra -Wno-unused-parameter)
	endif()
elseif(SCEMU_BUILD_TOOLS)
	message(STATUS "scgui skipped: needs GTK 4, zlib and re2c")
endif()

# scplay, the terminal player (PortAudio for audio, zlib to read ROM zips)

if(SCEMU_BUILD_TOOLS)
	find_package(ZLIB QUIET)
	if(ZLIB_FOUND)
		add_executable(scplay play/scplay.c play/tui.c play/audio.c)
		target_link_libraries(scplay PRIVATE scemu_play scemu ZLIB::ZLIB portaudio samplerate)
		if(NOT MSVC)
			target_compile_options(scplay PRIVATE -Wall -Wextra -Wno-unused-parameter)
		endif()
	else()
		message(STATUS "scplay skipped: needs zlib")
	endif()
endif()
