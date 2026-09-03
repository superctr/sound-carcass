# scgui

The Sound Canvas on the desktop: the SC-88Pro's front panel in a window, playing Standard MIDI Files
from a playlist, with the panel's buttons under the mouse.

    scgui [options] [file.mid ...]

It finds its ROM images the way `scplay` does (a zip or directory named after the model beside the
program or in `~/.mame/roms`, or `--rom`), boots the firmware once and caches the booted machine, and
starts every song from that fresh machine unless `--keep-settings` is given.  Files on the command
line become the playlist and the first one plays.

| option | |
|---|---|
| `--model sc88pro \| sc88 \| sc88vl` | which machine; the default is the SC-88Pro when its ROMs are found |
| `--rom PATH` | a zip or a directory holding the ROM images |
| `--size 4 \| 8` | the window size, named by the display's dot pitch in pixels: 4 is 1399 × 440, 8 twice that.  On a HiDPI screen the 8 is used for a 4 automatically |
| `--map sc55 \| sc88 \| sc88pro` | play every part from that instrument map, as in scplay |
| `--midi-rate BAUD` | the speed of the MIDI input, as in scplay |
| `--keep-settings` | keep the machine's settings memory across sessions, and across songs |
| `--no-cache` | boot the firmware every time |
| `--no-audio` | no sound card; the machine runs at real time anyway |
| `--tail N` | seconds to keep running after a song's last event (default 4) |

## The panel

- **Buttons** work with the mouse: press and hold with the left button.  The right button queues a
  key (it shows pressed but is not sent yet); the queued keys go down together with the next key you
  left-click, before it, and come up with it.  That is how the combinations the firmware reads while
  another key is held are entered with one mouse, and a queued key is also held through a power-on.
- **The volume knob** turns with the scroll wheel over it; it is the program's output gain, the
  machine itself has no volume control in software.  The knob's push switch (PREVIEW) is a button.
- **The power switch** switches the emulated unit off and on.  Switching on boots the firmware for
  real (not from the cache), with any queued keys held, so the power-on combinations work.
- **The MIDI IN B jack** opens the playlist, **the PHONES jack** the audio window.

- **Combinations from the manual**: a middle-click (or Ctrl and the right button) on a key opens a
  menu of every multi-key operation the owner's manual lists for that key; choosing one holds and
  presses the keys for you.  The list is `docs/panel/combinations.md` in the project.

Keys: `space` pause, `n` / `p` next and previous song, `l` the playlist, `q` quit.

## The playlist

A separate window: add files (the file chooser takes several at once), previous / pause / stop /
next, clear.  Double-click a row to play it; when a song ends the next one starts.  Every song is preceded by a GS
reset, a quarter second before its first event; pausing or stopping sends all notes off and all sound
off on every part first, so nothing hangs.  The title bar of
the main window shows the song's title (UTF-8 or Shift-JIS, as in scplay).

## MIDI ports

The bottom of the playlist window connects the machine to the host's MIDI ports (the ALSA
sequencer; `scgui` is a client with ports of its own, so other programs can connect to it too):

| | |
|---|---|
| MIDI IN A, MIDI IN B | a source feeding the machine's two inputs: a keyboard, a sequencer, another program |
| MIDI OUT | where the machine's own MIDI OUT goes |
| Song to A, Song to B | the song being played is also sent here, port A's and port B's tracks separately, with the same GS reset and the same notes-off on pause and stop: for a real unit playing along, e.g. an SC-8850's Part A and Part B |

The refresh button looks for ports again after plugging something in.

## Building

Needs GTK 4, ALSA, SDL2 (for audio) and zlib, besides the library.  Without GTK 4 the build skips it and
still makes `scplay`.
