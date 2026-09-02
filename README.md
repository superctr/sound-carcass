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

## Status

Scaffold.  Nothing plays yet.
