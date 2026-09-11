# scemu aka SoundCarcass

An emulator of the Roland Sound Canvas SC-55, SC-55mkII, SC-88, SC-88VL, SC-88Pro, SC-8850 and SC-8820 as a C library,
with a terminal player and a headless renderer.  It runs the machine's own firmware on an emulated
board; the H8/510 main CPU (the H8/532 on the SC-55 and the SC-55mkII, the SH-2 on the SC-8850), the XP tone
generator's DSP program and the LSP effect processor's program are all compiled to native code with
[sljit](https://github.com/zherczeg/sljit).  It comes as a library, two players and a CLAP plugin.

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
and render frames at the machine's own rate — 32 kHz, the SC-55mkII's own 66206 Hz and the SC-55's
64000 Hz, which `scemu_sample_rate` reports:

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

`scemu-cli <sc88|sc88vl|sc88pro|sc8850|sc8820|sc55mk2|sc55> <romdir> <out.wav> [--midi file.mid] [--seconds N] [--state boot.state] [--computer midi|pc1|pc2|usb]
...` boots the machine (or loads a saved state, saving one after the boot when the file is not there) and
renders a song to a WAV file.

`scplay song.mid` plays a Standard MIDI File to the speakers with the front panel drawn in the terminal:
the LCD's text fields, the sixteen level bars animating from the CGRAM patterns the firmware writes, the
LEDs and a clock, with the panel buttons on the keyboard (on the SC-8850, whose panel is one 160 × 64
bitmap, that display drawn dot for dot in half-block characters, with the value dial on two keys).  It
finds its ROM images by itself — recognised by content, in any zip or directory beside the program or
in `~/.mame/roms` (`docs/roms.md`) — boots the firmware once and
caches the booted machine, so later runs start instantly from factory settings (`--keep-settings` keeps
the machine's settings memory across sessions instead).  `--no-audio --wav out.wav` renders a file
about ten times faster than real time instead, and `--map sc55` (or `sc88`, `sc88pro`) plays every part
from that instrument map whatever the song selects; `--midi-rate 38400` feeds the file at the
computer port's speed instead of the cable's, and `--rate 44100` asks the sound card, and writes the
wav, at that rate instead of the machine's own.  A dual-port file (tracks with port events, the
32-part songs written for a Pro on both MIDI INs) plays on both blocks, and on an SC-8850, whose
switch is on USB by default, a four-port file plays on all four groups, 64 parts.  See [docs/scplay.md](docs/scplay.md).

`scgui` is the same player as a desktop window: the front panel drawn from `gui/`'s artwork, its
buttons under the mouse, the volume knob and the SC-8850's value dial under the wheel, a playlist in a
second window.  It needs GTK 4 besides zlib.  See [docs/scgui.md](docs/scgui.md).  Both players play
through PortAudio, convert to the output rate with libsamplerate and `scgui` takes MIDI through
PortMidi, all built from the submodules (`git submodule update --init`).  The library itself renders
at the machine's own rate and resamples nothing.
Without zlib the library and `scemu-cli` still build.

`scemu.clap` is the emulator as a CLAP instrument, one per model, for a DAW: the machine's outputs as
two stereo ports, its MIDI INs and OUT as note ports, the volume, the map, the MIDI speed and the
headroom as parameters, the whole machine as the state, and the front panel as its window.  It needs
OpenGL and, on Linux, X11.  See [docs/plugin.md](docs/plugin.md).

## Credits

Created by superctr 2026.

Thanks to:

- [giulioz](https://theusualsuspects.io/) for reverse engineering the XP and LSP chips.
- [nukeykt](https://github.com/nukeykt/) for reverse engineering the GP (SC-55 sound chip)
- [kode54](https://github.com/TabulaSonora) for reverse engineering Sound Canvas VA and its synth engine which was used as a reference for the initial XP implementation.

AI disclosure: Claude Opus 5, Claude Fable 5 and 5.1 were used in this project.
