# scemu aka SoundCarcass

An emulator of the Roland Sound Canvas SC-88 and SC-88Pro as a C library, with a headless renderer.
It runs the machine's own firmware on an emulated board; the XP and LSP effect DSPs are compiled to
native code with [sljit](https://github.com/zherczeg/sljit).  A front-panel player and an audio plugin
are planned on top of the library.

Clean-room, BSD-3.  You need your own ROM images: see `docs/roms.md`.

## Building

```
git submodule update --init
cmake -S . -B build
cmake --build build
```

Options: `-DSCEMU_BUILD_TOOLS=OFF` builds the library alone; `-DSCEMU_EXTERNAL_EXEC_ALLOCATOR=ON` leaves
executable memory to the integrator (see `scemu_config` in `lib/scemu.h`).

## Using the library

`lib/scemu.h` is the whole interface.  Create an instance from the ROM images, boot it, feed it MIDI bytes
and render frames at the machine's own 32 kHz:

```c
scemu_t *m = scemu_create(SCEMU_MODEL_SC88PRO, &roms, NULL);
scemu_boot(m);
scemu_midi_write(m, SCEMU_MIDI_IN_A, bytes, count, 0);
int32_t *const out[2] = { output1, output2 };
scemu_render(m, out, frames);
```

Panel buttons, LEDs and the LCD's memory are exposed for a front end; the machine state, including the
battery-backed settings memory, can be saved and restored.

## Tools

`scemu-cli <sc88|sc88pro> <romdir> <out.wav> [seconds]` boots the machine and renders to a WAV file.

`scplay song.mid` plays a Standard MIDI File to the speakers with the front panel drawn in the terminal:
the LCD's text fields, the sixteen level bars animating from the CGRAM patterns the firmware writes, the
LEDs and a clock, with the panel buttons on the keyboard.  It finds its ROM images by itself — a zip or a
directory named after the model, beside the program or in `~/.mame/roms` — boots the firmware once and
caches the booted machine, so later runs start instantly from factory settings (`--keep-settings` keeps
the machine's settings memory across sessions instead).  `--no-audio --wav out.wav` renders a file
about ten times faster than real time instead, and `--map sc55` (or `sc88`, `sc88pro`) plays every part
from that instrument map whatever the song selects.  See [docs/scplay.md](docs/scplay.md).  It needs SDL2 and
zlib; without them the library and `scemu-cli` still build.

## Status

Scaffold.  Nothing plays yet.
