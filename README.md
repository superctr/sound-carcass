# scemu aka SoundCarcass

An emulator of the Roland Sound Canvas SC-55mkII, SC-88, SC-88VL, SC-88Pro and SC-8850 as a C library,
with a terminal player and a headless renderer.  It runs the machine's own firmware on an emulated
board; the H8/510 main CPU (the H8/532 on the SC-55mkII, the SH-2 on the SC-8850), the XP tone
generator's DSP program and the LSP effect processor's program are all compiled to native code with
[sljit](https://github.com/zherczeg/sljit).  An audio plugin is planned on top of
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
and render frames at the machine's own rate — 32 kHz, and the SC-55mkII's own 66206 Hz, which
`scemu_sample_rate` reports:

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

`scemu-cli <sc88|sc88vl|sc88pro|sc8850|sc55mk2> <romdir> <out.wav> [--midi file.mid] [--seconds N] [--state boot.state] [--computer midi|pc1|pc2|usb] [--rail BITS]
...` boots the machine (or loads a saved state, saving one after the boot when the file is not there) and
renders a song to a WAV file; `--rail 29` widens the DSP's output rail for the busy songs that clip on the
unit's 24 bits.

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

## Status

Playable.  The SC-88 and the SC-88Pro boot their real firmware through the display sequence, the
front panel and its LEDs work, MIDI files render and play on both, and the SC-88VL boots and plays
with the SC-88's wave ROMs.  The SC-8850 boots its own firmware on the SH-2, draws its graphic
display, answers its panel and its value dial and plays, and with its rear COMPUTER switch on USB it
takes all four MIDI port groups A-D — 64 parts — and sends its MIDI out back on the port it came from.
The SC-55mkII boots its own firmware through the 4.75 s display animation — twice from a blank settings
memory, which is what the machine needs to come up in tune — takes both MIDI INs, drives
its MIDI OUT, answers its panel and its POWER key — a position in the panel matrix, so the machine
mutes itself and goes on running — and renders at its own 66206 Hz.  What is emulated:

| part | how | checked against |
|---|---|---|
| H8/510 main CPU (H8/532 on the SC-55mkII), its timers, serial ports and A/D | interpreter, and a dynamic translator for the firmware | MAME*'s core, instruction by instruction; the translator against the interpreter in lockstep |
| XP tone generator (voices, ramps, filters, host interface) and its DSP program (reverb, chorus, delay, EQ) | voice engine in C; the DSP program compiled per frame, recompiled when the firmware patches a coefficient | MAME's device, every register every frame |
| LSP insertion-effect processor (SC-88Pro) | the 384-word program compiled per sample, coefficient patches without recompiling | MAME's device on 171 firmware programs, and an SC-8850 |
| GP tone generator (SC-55mkII): its 28 voices and the reverb and chorus in the same chip | C | MAME's device, a recorded firmware session replayed into both: every register read, every interrupt edge and every output sample |
| gate array (interrupts, LEDs, LCD interface), LCD controller | C | firmware behaviour |
| sub-CPU (MIDI in, panel matrix, MIDI out) | high-level emulation, without its ROM | firmware behaviour |
| SC-8850 USB controller (the four port groups, the boot's box) | high-level emulation of its mailbox protocol; its ROM is not dumped | firmware behaviour |
| wave ROMs | unscrambled on load | descrambled chips |

`*`: MAME refers to a branch containing a previous version of the XP/LSP emulator created by this author.

Speed: a 32 kHz frame costs about 2.3 µs when playing on this machine's x86-64 (the tone generator
now dominates), so a render runs around 12× real time and playback takes a few percent of a core.
Only 64-bit hosts compile the programs for now.

Not done: an audio plugin; a save-state format that survives versions (the current one is a snapshot
for the boot cache); 32-bit hosts; the rest of the SC-55 family; and the sound has been compared to
MAME's renders and to hardware measurements, not yet to a real SC-88Pro side by side.
