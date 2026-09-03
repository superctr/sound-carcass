# scgui

The Sound Canvas on the desktop: the SC-88Pro's front panel in a window, playing Standard MIDI Files
from a playlist, with the panel's buttons under the mouse.

    scgui [options] [file.mid ...]

It finds its ROM images the way `scplay` does (a zip or directory named after the model beside the
program or in `~/.mame/roms`, or `--rom`), and boots the firmware once, caching the booted machine.
Files on the command line become the playlist and the first one plays.

| option | |
|---|---|
| `--model sc88pro \| sc88 \| sc88vl` | which machine; the default is the SC-88Pro when its ROMs are found |
| `--rom PATH` | a zip or a directory holding the ROM images |
| `--size 4 \| 8` | the window size, named by the display's dot pitch in pixels: 4 is 1399 × 440, 8 twice that.  On a HiDPI screen the 8 is used for a 4 automatically |
| `--map sc55 \| sc88 \| sc88pro` | play every part from that instrument map, as in scplay |
| `--midi-rate BAUD` | the speed of the MIDI input, as in scplay |
| `--keep-settings` | keep the machine's settings memory across sessions |
| `--no-cache` | boot the firmware every time |
| `--no-audio` | no sound card; the machine runs at real time anyway |
| `--tail N` | seconds to keep running after a song's last event (default 4) |

## The panel

- **Buttons** work with the mouse: press and hold with the left button.  The manual has two kinds of
  two-key operations, and the right button covers both.  A plain right-click *queues* a key: it shows
  pressed but is not sent until the next key you left-click, and then both go down in the same key
  scan, the way the manual's "press [A] and [B] simultaneously" wants them.  Shift and the right button
  *hold* a key: it goes down at once and stays down, for the manual's "while holding [A], press [B]",
  where the firmware must see the hold before the second key.  Either kind comes up when the
  left-clicked key does, or with another right-click on it, and both are held through a power-on.
  While the left button holds one half of a ◀ ▶ pair, the right button presses the other half, which
  is how the unit steps a value quickly.
- **The volume knob** turns with the scroll wheel over it; it is the program's output gain, the
  machine itself has no volume control in software.  The knob's push switch (PREVIEW) is a button.
- **The power switch** switches the emulated unit off and on.  Switching on boots the firmware for
  real (not from the cache), with any queued keys held, so the power-on combinations work.
- **The MIDI IN B jack** opens the playlist, **the PHONES jack** the audio window.

- **Combinations from the manual**: a middle-click (or Ctrl and the right button) on a key opens a
  menu of every multi-key operation the owner's manual and the service notes list for that key, each
  with its keys spelled out and the mode it applies in; the keys light up on the panel while the
  pointer is over an entry, and choosing one plays it for you on the machine's own clock: keys the
  manual says to press together go down in one scan, a held key is held for a moment before the next
  goes down, and a power-on combination goes through a power cycle with its keys held.  The list is
  `docs/panel/combinations.md` in the project.  The SC-88Pro's power-on combinations (test mode, the
  version display) do not take effect in the emulator yet; see that file.

Keys: `space` pause, `n` / `p` next and previous song, `l` the playlist, `q` quit.

## The playlist

A separate window: add files (the file chooser takes several at once), previous / pause / stop /
next, clear.  Double-click a row to play it; when a song ends the next one starts.  Every song is preceded by the
message chosen under "Before each song" (a GS Reset by default; GM or GM2 System On, an SC-88 mode
set, or nothing), a quarter second before its first event, so a song without a reset of its own does
not inherit the last one's settings; pausing or stopping sends all notes off and all sound off on
every part first, so nothing hangs.  The title bar of the main window shows the song's title (UTF-8
or Shift-JIS, as in scplay).

## MIDI ports

The bottom of the playlist window connects the machine to the host's MIDI ports (the ALSA
sequencer; `scgui` is a client with ports of its own, so other programs can connect to it too):

| | |
|---|---|
| MIDI IN A, MIDI IN B | a source feeding the machine's two inputs: a keyboard, a sequencer, another program |
| MIDI OUT | where the machine's own MIDI OUT goes |
| Song to A, Song to B | the song being played is also sent here, port A's and port B's tracks separately, with the same GS reset and the same notes-off on pause and stop: for a real unit playing along, e.g. an SC-8850's Part A and Part B |

The refresh button looks for ports again after plugging something in.  The first three are always in
view; the toolbar's gear button reveals the song outputs together with the message sent before each
song and the instrument map override ("as the song selects", or one of the three maps forced on every
part, the same as `--map`).

## Building

Needs GTK 4, ALSA, SDL2 (for audio) and zlib, besides the library.  Without GTK 4 the build skips it and
still makes `scplay`.
