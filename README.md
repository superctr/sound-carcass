# scemu aka SoundCarcass

An emulator of the Roland Sound Canvas SC-88, SC-88VL and SC-88Pro as a C library, with a terminal
player and a headless renderer.  It runs the machine's own firmware on an emulated board; the H8/510
main CPU, the XP tone generator's DSP program and the LSP effect processor's program are all compiled
to native code with [sljit](https://github.com/zherczeg/sljit).  An audio plugin is planned on top of
the library.

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

`scemu-cli <sc88|sc88pro> <romdir> <out.wav> [--midi file.mid] [--seconds N] [--state boot.state] [--rail BITS]
...` boots the machine (or loads a saved state, saving one after the boot when the file is not there) and
renders a song to a WAV file; `--rail 29` widens the DSP's output rail for the busy songs that clip on the
unit's 24 bits.

`scplay song.mid` plays a Standard MIDI File to the speakers with the front panel drawn in the terminal:
the LCD's text fields, the sixteen level bars animating from the CGRAM patterns the firmware writes, the
LEDs and a clock, with the panel buttons on the keyboard.  It finds its ROM images by itself — recognised by
content, in any zip or directory beside the program or in `~/.mame/roms` (`docs/roms.md`) — boots the firmware once and
caches the booted machine, so later runs start instantly from factory settings (`--keep-settings` keeps
the machine's settings memory across sessions instead).  `--no-audio --wav out.wav` renders a file
about ten times faster than real time instead, and `--map sc55` (or `sc88`, `sc88pro`) plays every part
from that instrument map whatever the song selects; `--midi-rate 38400` feeds the file at the
computer port's speed instead of the cable's.  A dual-port file (tracks with port events, the
32-part songs written for a Pro on both MIDI INs) plays on both blocks.  See [docs/scplay.md](docs/scplay.md).

`scgui` is the same player as a desktop window: the front panel drawn from `gui/`'s artwork, its
buttons under the mouse, the volume knob under the wheel, a playlist in a second window.  It needs
GTK 4 besides zlib.  See [docs/scgui.md](docs/scgui.md).  Both players play through PortAudio and
`scgui` takes MIDI through PortMidi, built from the submodules (`git submodule update --init`).
Without zlib the library and `scemu-cli` still build.

## Status

Playable.  The SC-88 and the SC-88Pro boot their real firmware through the display sequence, the
front panel and its LEDs work, MIDI files render and play on both, and the SC-88VL boots and plays
with the SC-88's wave ROMs.  What is emulated:

| part | how | checked against |
|---|---|---|
| H8/510 main CPU, its timers, serial ports and A/D | interpreter, and a dynamic translator for the firmware | MAME*'s core, instruction by instruction; the translator against the interpreter in lockstep |
| XP tone generator (voices, ramps, filters, host interface) and its DSP program (reverb, chorus, delay, EQ) | voice engine in C; the DSP program compiled per frame, recompiled when the firmware patches a coefficient | MAME's device, every register every frame |
| LSP insertion-effect processor (SC-88Pro) | the 384-word program compiled per sample, coefficient patches without recompiling | MAME's device on 171 firmware programs, and an SC-8850 |
| gate array (interrupts, LEDs, LCD interface), LCD controller | C | firmware behaviour |
| sub-CPU (MIDI in, panel matrix, MIDI out) | high-level emulation; its ROM is not dumped | firmware behaviour |
| wave ROMs | unscrambled on load | descrambled chips |

`*`: MAME refers to a branch containing a previous version of the XP/LSP emulator created by this author.

Speed: a 32 kHz frame costs about 2.3 µs when playing on this machine's x86-64 (the tone generator
now dominates), so a render runs around 12× real time and playback takes a few percent of a core.
Only 64-bit hosts compile the programs for now.

Not done: an audio plugin; a save-state format that survives versions (the current one is a snapshot
for the boot cache); 32-bit hosts; the SC-55 family, which needs the GP tone generator and a real
sub-CPU; and the sound has been compared to MAME's renders and to hardware measurements, not yet to a
real SC-88Pro side by side.
