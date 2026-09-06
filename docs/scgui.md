# scgui

The Sound Canvas on the desktop: the machine's front panel in a window (the SC-88's, the SC-88VL's, the
SC-88Pro's or the SC-8850's, following the System choice; the VE-GS Pro, which has no panel, wears the
Pro's), playing Standard MIDI Files from a playlist, with the panel's buttons under the mouse.

    scgui [options] [file.mid ...]

It finds its ROM images the way `scplay` does (recognised by their contents in `--rom`, beside the
program or in `~/.mame/roms`, whatever they are named), and boots the firmware once, caching the booted
machine.  Files on the command line become the playlist and the first one plays.

| option | |
|---|---|
| `--model sc88pro \| sc88 \| sc88vl \| sc8850` | which machine; the default is the SC-88Pro when its ROMs are found |
| `--rom PATH` | a zip or a directory holding the ROM images, whatever they are named |
| `--size 4 \| 8` | the window size: 4 is the small panel (1399 × 440 for the 88 family, 1696 × 692 for the SC-8850), 8 twice that.  The bake is named by the glass's dot pitch, which is 4 and 8 on the 88 family and 3 and 6 on the SC-8850, whose display has more and smaller dots.  On a HiDPI screen the large bake is used for the small size automatically |
| `--map sc55 \| sc88 \| sc88pro \| sc8850` | play every part from that instrument map, as in scplay |
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
- **The SC-8850's value dial** turns two ways: drag its outer ring with the left button and it follows
  the pointer round, a thirty-sixth of a turn to a detent, so the mark on the panel stays under the
  hand; or roll the scroll wheel over it, one detent to a notch.  Its centre is the push switch
  (VALUE), a button like any other, and it takes the wheel as well.
- **The power switch** switches the emulated unit off and on; on the SC-88VL the STANDBY lamp beside
  it is lit while the unit is off.  Switching on boots the firmware for
  real (not from the cache), with any queued keys held, so the power-on combinations work.
- **The MIDI IN B jack** opens the playlist, **the PHONES jack** the Settings window.
- **The model name** beside the logotype opens a menu of the systems whose ROMs are there, to switch
  between them, and a Reset that power-cycles the machine.
- **The logotype** at the bottom left opens a menu where the pointer is: the playlist, the Settings
  window on its Audio or its System tab, a **Send** submenu, and About.  Send puts a message on both of
  the machine's inputs at once (and on the song outputs, for a unit playing along): GM System On, GS
  Reset, GM2 System On, the SC-88 mode sets, or a bank select and program change that puts every part
  on the SC-55, SC-88, SC-88Pro or SC-8850 map -- the one-off form of the map override, and the way to reach
  those on a machine with no panel of its own.

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

The bottom of the playlist window ties the machine to the host's MIDI devices, through PortMidi.  On
Linux that is the ALSA sequencer, where `scgui` is a client with ports of its own -- MIDI IN A, MIDI
IN B, MIDI OUT, Song A, Song B, and on an SC-8850 whose computer switch is on USB also MIDI IN C, MIDI
IN D, Song C and Song D -- so other programs can connect to it as well; the drop-downs tie one of
those to a device besides:

| | |
|---|---|
| MIDI IN A, MIDI IN B | a device feeding the machine's inputs: a keyboard, an interface, another program's port |
| MIDI IN C, MIDI IN D | the same for port groups C and D; the rows are there while the machine has them, an SC-8850 on USB, and the ports on the sequencer with them |
| MIDI OUT | where the machine's own MIDI OUT goes |
| Song to A, Song to B | the song being played is also sent here, port A's and port B's tracks separately, with the same GS reset and the same notes-off on pause and stop: for a real unit playing along, e.g. an SC-8850's Part A and Part B |
| Song to C, Song to D | port C's and port D's tracks, with the rows above |

The refresh button looks for devices again after plugging something in; a tie survives it when its
device is still there.  A program that lists the sequencer's ports once, when it starts -- Wine's
MIDI does -- sees `scgui`'s ports only if `scgui` was running first; the tie can be made from this
side instead, since such a program's own output port (Wine's "WINE ALSA Output #n") is in the
drop-downs too, or through "Midi Through Port-0" with both ends set to it.  The inputs and MIDI OUT
are always in view; the toolbar's gear button reveals the song outputs together with the message
sent before each song and the instrument map override ("as the song selects", or one of the four maps
forced on every part, the same as `--map`).

What arrives from the host is timed, not just taken: the machine's thread looks at the ports about
every millisecond, stamps each message as it is seen, and places it one buffer plus two milliseconds
after that on the machine's own clock, so what you play lands with the same delay every time -- the
jitter is the polling, about a millisecond, instead of a whole buffer.

## Settings

The PHONES jack opens the Settings window, which has two tabs.

**Audio**: the output device (every device PortAudio finds, on every host API it was built with -- on
Linux ALSA, JACK and PulseAudio -- or the host's default), the device's buffer (64 to 1024 frames, 2
to 32 ms at the machine's 32 kHz; 256 by default), the volume knob's travel in notches of the scroll
wheel (5 to 200 from silent to full; 20 by default), and a readout of the device in use, the latency
from the machine to the jack and the underruns so far.  The machine keeps two buffers ahead of the
device -- two of the buffer chosen, or of the period the host actually takes when that is larger, as
under JACK, where the server sets it; a smaller buffer means less delay from a key to the sound and
more chance of a dropout on a busy host.  A device that will not open at 32 kHz runs at its own rate,
and the output is resampled on the way (a 32-tap windowed sinc).

**System**: which machine this is -- SC-88, SC-88VL, SC-88Pro or SC-8850.  A model whose ROM images
were not found is greyed out.  Choosing another one switches at once: the song is unloaded, the machine is
replaced and the new one comes up from its own boot cache, so it is instant from the second time on;
the sound card and the MIDI ties stay as they are.  Below the choice is what is running: the model,
its control ROM version, and the file its images came from.

The **computer switch** is the row under the systems: the switch on the back of the unit, in the
positions that system has -- MIDI, PC-1, PC-2 and Mac on the 88 family, MIDI, PC-1, PC-2 and USB on
the SC-8850.  It belongs to the system, not to the program, so each one keeps its own; the row always
shows the running one.  The firmware reads the ladder once when it comes up, so changing it replaces
the machine and boots it again, the way changing the model does (from that position's own boot cache).
MIDI is the default on the 88 family, where the sub-CPU is emulated at a high level and feeds the
firmware from the jacks whatever the switch says, so only the boot differs.  USB is the default on
the SC-8850: it is the position that carries all four port groups A-D, 64 parts, so a four-port song
plays whole and the playlist window gets the rows for C and D; its boot puts up the "USB On Line" box.
MIDI there is the two jacks, groups A and B, and PC-1 and PC-2 take the parts off the jacks and wait
for a serial host, which is not answered here, so the machine plays nothing.  This is not the MIDI
speed: a computer port also runs at 38400 baud, which is `--midi-rate`.

The **wide output rail** is a box on the same tab.  The unit's DSP saturates the words it hands the
converters at 24 bits, and a busy song runs into that ceiling and clips there, as it does on the real
thing.  Ticking the box gives those words 29 bits -- the width of the DSP's own accumulator, 30 dB
above the ceiling -- and changes nothing below it, so the sound stays the unit's and only what would
have been chopped off is kept.  It takes effect at once, also mid-song.  The level does not move: the volume knob has the same scale on
either rail, and what the wide rail keeps is only lost again where the knob leaves it above full
scale at the sound card, so turn the knob down for a song that used to clip.

## The settings file

`$XDG_CONFIG_HOME/scemu/scgui.conf`, or `~/.config/scemu/scgui.conf`, keeps what the windows set: the
machine and where its ROMs are, the window size, the output device and its buffer, the volume knob,
its travel in notches and the rail, the nine MIDI ties by the device's name, the message before each song, the instrument
map, the MIDI speed, the computer switch of each system (`computer_sc88`, `computer_sc88vl`,
`computer_sc88pro` and `computer_sc8850`, each `midi`, `pc1`, `pc2` or `mac`, with `usb` taken as the
SC-8850's word for the last), whether the settings memory is kept, and the tail.  It is read at start, written
a moment after a setting changes, and written again on exit.  An option on the command line overrides
the file for that run and does not change it.  The file is `key = value` text with `#` comments; a
line it cannot make sense of is complained about on stderr and every other line is still taken.

## Building

Needs GTK 4 and zlib, besides the library; PortAudio and PortMidi are built from the submodules, so
clone with them (`git submodule update --init`).  Without GTK 4 the build skips it and
still makes `scplay`.
