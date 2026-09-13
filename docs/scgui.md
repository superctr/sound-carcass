# scgui

The Sound Canvas on the desktop: the machine's front panel in a window (the SC-88's, the SC-88VL's, the
SC-88Pro's, the SC-8850's, the SC-55mkII's or the SC-55's, following the System choice; the VE-GS Pro, which has no
panel, wears the Pro's), playing Standard MIDI Files from a playlist, with the panel's buttons under
the mouse -- and, docked above it, the Sound Brush, Roland's floppy player of the SC-55's day, whose
keys and display drive that playlist ("The Sound Brush" below).

    scgui [options] [file.mid ...]

It finds its ROM images the way `scplay` does (recognised by their contents in `--rom`, beside the
program or in `~/.mame/roms`, whatever they are named), and boots the firmware once, caching the booted
machine.  Files on the command line become the playlist and the first one plays.

| option | |
|---|---|
| `--model sc88pro \| sc88 \| sc88vl \| sc8850 \| sc8820 \| sc55mk2 \| sc55` | which machine; the default is the SC-88Pro when its ROMs are found |
| `--rom PATH` | a zip or a directory holding the ROM images, whatever they are named |
| `--size 4 \| 8` | the window size: 4 is the small panel (1399 × 440 for the SC-88 and the SC-88Pro, 1399 × 282 for the shallower SC-88VL, SC-55mkII and SC-55, 1696 × 692 for the SC-8850, 1421 × 242 for the SC-8820), 8 twice that.  The bake is named by the glass's dot pitch, which is 4 and 8 on the 88 family and the two SC-55s and 3 and 6 on the SC-8850, whose display has more and smaller dots; the SC-8820 has no glass and its bake is 7 and 14 pixels a millimetre.  On a HiDPI screen the large bake is used for the small size automatically |
| `--map sc55 \| sc88 \| sc88pro \| sc8850` | play every part from that instrument map, as in scplay |
| `--midi-rate BAUD` | the speed of the MIDI input, as in scplay |
| `--rate native \| 32000 \| 44100 \| 48000` | the rate to ask the output device for, as in scplay; `native`, the default, is the machine's own |
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
  is how the unit steps a value quickly -- that one is the right button whether or not the Interface
  tab has swapped the two.
- **The volume knob** turns with the scroll wheel over it, twenty notches from silent to full; it is
  the program's output gain, the machine itself has no volume control in software.  The Audio tab has
  the same gain as a slider, for a pointer with no wheel.  The knob's push switch (PREVIEW) is a button.
- **The SC-8850's value dial** turns two ways: drag its outer ring with the left button and it follows
  the pointer round, a thirty-sixth of a turn to a detent, so the mark on the panel stays under the
  hand; or roll the scroll wheel over it, one detent to a notch.  Its centre is the push switch
  (VALUE), a button like any other, and it takes the wheel as well.
- **The power switch** switches the emulated unit off and on; on the SC-88VL the STANDBY lamp beside
  it is lit while the unit is off.  Switching on boots the firmware for
  real (not from the cache), with any queued keys held, so the power-on combinations work.  The
  SC-55mkII's and the SC-55's POWER is not a mains switch but a position in the panel's own switch
  matrix, so clicking it sends that key: the firmware mutes the audio and switches the display's power
  off while the machine keeps running, and the STANDBY lamp is the machine's own.  A note sounding when it is
  pressed keeps sounding into a dead amplifier, and MIDI arriving in standby goes nowhere.
- **The MIDI IN B jack** opens the playlist (on the SC-8850 and the SC-8820, whose MIDI comes in over
  USB, it is **the USB mark**), **the PHONES jack** the Settings window.
- **The model name** beside the logotype opens a menu of the systems whose ROMs are there, to switch
  between them, and a Reset that power-cycles the machine.  The SC-55 carries no name there, only the
  logotype, and the bare panel where the SC-55mkII has its name opens the same menu.
- **The logotype** at the bottom left opens a menu where the pointer is: the playlist, the Sound Brush,
  the Settings window on its Audio or its System tab, a **Send** submenu, and About.  Send puts a message on both of
  the machine's inputs at once (and on the song outputs, for a unit playing along): GM System On, GS
  Reset, GM2 System On, the SC-88 mode sets, or a bank select and program change that puts every part
  on the SC-55, SC-88, SC-88Pro or SC-8850 map -- the one-off form of the map override, and the way to reach
  those on a machine with no panel of its own.

- **Combinations from the manual**: a middle-click (or Ctrl and the right button, or a plain
  right-click when the buttons are swapped on the Interface tab) on a key opens a
  menu of every multi-key operation the owner's manual and the service notes list for that key, each
  with its keys spelled out and the mode it applies in; the keys light up on the panel while the
  pointer is over an entry, and choosing one plays it for you on the machine's own clock: keys the
  manual says to press together go down in one scan, a held key is held for a moment before the next
  goes down, and a power-on combination goes through a power cycle with its keys held.  Each panel has
  its own list -- the SC-88, SC-88VL, SC-55mkII and SC-8850 keep to what their own manual and service
  notes describe, and the SC-55 takes the SC-55mkII's manual's list, marked as such, until its own
  manual is in -- and the whole of it is `docs/panel/combinations.md` in the project.  The factory Test
  Mode is left out of the menus, since nothing here answers it yet; the power-on combinations that are
  offered (the version display, the MIDI THRU check) do not take effect either, and that file says so.

- **The display** is drawn from the controller's memory as it stands, sharper than the real glass: the
  SC-55mkII's firmware flashes the instrument name over a scrolling display message, which the hardware's
  response time would swallow (`docs/lcd.md`).

Keys, in every window: `space` pause, `s` stop, `n` / `p` next and previous song, `l` the playlist, `b`
the Sound Brush, `q` quit.  While the Brush's panel is shown they are its keys (PAUSE, or PLAY when
nothing plays; STOP; SONG ▶ and ◀), so they follow its modes.

## The playlist

A separate window: add files (the file chooser takes several at once), previous / pause / stop /
next, clear.  Double-click a row to play it; when a song ends the next one starts, and a list that was
empty starts playing when its first file arrives.  While the Sound Brush's panel is shown (below) the
list is the Brush's disk instead: what plays after a song ends, and after how long, is the Brush's
business -- the next song after its interval, four seconds by default, and a stop at the end of the list
unless REPT is on -- and the toolbar's buttons are its keys, previous and next SONG ◀ ▶ (which switch at
once, playing or not), pause PAUSE (or PLAY when nothing plays), stop STOP, clear the disk out.  Every song
is preceded by the
message chosen under "Before each song" (a GS Reset by default; GM or GM2 System On, an SC-88 mode
set, or nothing), a quarter second before its first event, so a song without a reset of its own does
not inherit the last one's settings; pausing or stopping sends all notes off and all sound off on
every part first, so nothing hangs.  The title bar of the main window shows the song's title (UTF-8
or Shift-JIS, as in scplay).

Files dragged onto the window from a file manager are added where they are dropped: on a row they go
in above it, past the last row or anywhere else in the window they go on the end, and if the list was
empty the first one starts playing.  A row dragged within the list moves to where it is dropped, so
the order is the order of the rows; the song that is playing keeps playing, and its highlight follows
it.  A row's tooltip is the file's whole path, and a right-click on it opens a menu with **Go to
source directory**, which opens the host's file manager on the folder the file is in, and **Remove
file from playlist**; removing the song that is playing stops it, the way the stop button does.

## The Sound Brush

The SB-55 Sound Brush was Roland's floppy-disk MIDI file player, the SC-55's case with a drive in it,
made to sit under a Sound Canvas and drive it: a three-digit display, a song number, PLAY, STOP, PAUSE,
REW and FF, and modes for the disk.  Here it is the front of the playlist: **Sound Brush** on the logo
menu, or `b`, shows its panel above the module's in the same window, as wide as the module's panel
whatever the model, so the two stack as the units did and move together (a window cannot be placed
beside another or made to follow it on Wayland, so they share one), and while that panel is shown the
playlist is the disk in its slot -- a disk is in while the list holds a song, the song number is the
row, and the Brush's keys work the list and the player.  It behaves as the real unit's firmware does,
measured on the MAME driver of the machine (`docs/panel/README.md`, "The SB-55's behaviour, measured",
in the project), without the recorder.  **Sound Brush** or `b` again hides the panel, and the list runs
as it always did from there, from the song and the pause the Brush left it at; show it again and the
Brush takes the list as it stands, its modes and settings as they were.
While it is shown each song is also preceded by its title on the module's display, as the SB-55
sends it (the sequence name at the head of the file's first track, in the Sound Canvas display
message).

- **The slot** takes files: drop Standard MIDI Files on the window, or click the slot for the file
  chooser.  **EJECT** takes the disk out, which clears the list.  **MIDI IN 2** opens the playlist
  window, as MIDI IN B does on the module.  The **DISK** lamp flashes as the drive's would: for a song
  read in, for each SONG key, and as the song plays for every read the player makes, which keeps a
  buffer per track as the SB-55 does, so a song of many tracks flashes it more.
- **The display** shows the song number, `.01`, with `---` for no disk; the tempo (`.120`, with the
  point at the left) for a second after a TEMPO key; the bar while REW or FF is held; and the seconds
  left before the next song, `- 4`, `- 3`..., while it waits between songs.  SONG ◀ and ▶ pressed
  together, or TEMPO ◀ and ▶, or REW and FF, make the number, the tempo or the bar what the display shows
  when nothing else is going on -- so REW + FF gives a bar counter while the song plays.
- **SONG ◀ ▶** step the song, without wrapping; while a song plays the new one starts at once.
  **TEMPO ◀ ▶** change the tempo a beat a minute at a time, 5 to 260, scaling the song's own tempo
  map; CLEAR + TEMPO puts it back, and every song starts at its own.
- **PLAY**, **STOP**, **PAUSE**: PLAY lights while a song plays, PAUSE beside it while it is paused
  and PLAY or PAUSE goes on -- and a pause here holds the song alone, the module running on (its
  reverb rings out, the display keeps moving, MIDI IN still plays), where the playlist's pause with the
  Brush hidden stands the whole machine still; STOP silences and, with auto rewind on, goes back to the start, otherwise
  PLAY goes on from where it stopped.  **REW** and **FF** held step through the bars, one at once, the
  next after half a second and then eight a second, silent meanwhile, and the song goes on from the
  new bar when the key comes up, with the controllers, programs and system exclusives it had set before
  that point sent first, so the parts stand as they should (the Brush's MIDI Update).  STOP + REW and
  STOP + FF jump to the start and the end.
- **The modes**: after the last song the Brush stops with the first selected, and does not start
  again on its own; **SINGLE** stops after each song with the next selected; **REPT** goes round the
  list, and with SINGLE repeats the song; **RND** plays each song once in a random order and stops,
  or with REPT starts another round.  Between songs the Brush waits the interval -- four seconds by
  default -- counting it down on the display.
- **A program**: hold SET and press PROG, and PROG blinks over `--`; SONG ◀ ▶ then show and step a
  song, SET stores it, up to ninety-nine, and STOP or PLAY ends the entry with PROG lit and the
  program's first song selected.  PLAY plays the program in order and stops at its end; with REPT it
  goes round, with RND in a random order; PROG alone leaves the program mode and takes it up again, and
  CLEAR + PROG throws the program away.  It goes with the disk.
- **The system functions**: hold SET and press PAUSE, PLAY or STOP for the interval between songs (0
  to 99 seconds), auto play (whether a disk going in plays at once) and auto rewind; REW and FF change
  the value shown, SET stores it.  The three are kept in the settings file.
- **POWER** puts the Brush into standby, its display dark and STANDBY lit, playing stopped and its
  keys dead until POWER again; it is the Brush's own, not the module's.  **REC** does nothing yet.

The keys work under the mouse as the module's do: the left button presses, a plain right-click
queues a key to go down with the next left-click (the manual's "press both"), Shift and the right
button hold a key down from now ("while holding"), and everything comes up with the left button.  The
Brush's window remembers whether it was open.

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

The PHONES jack opens the Settings window, which has three tabs.

**Audio**: the output device (every device PortAudio finds, on every host API it was built with -- on
Linux ALSA, JACK and PulseAudio -- or the host's default), the rate to ask it for, the device's buffer
(64 to 1024 frames, 2 to 32 ms at 32 kHz; 256 by default), the volume as a slider -- the knob on the
panel, which follows it and is followed by it -- and a readout of the device in use, the
latency from the machine to the jack and the underruns so far.  Changing the device or the rate
reopens the stream where it stands, as changing the buffer does.  The machine keeps two buffers ahead
of the device -- two of the buffer chosen, or of the period the host actually takes when that is
larger, as under JACK, where the server sets it; a smaller buffer means less delay from a key to the
sound and more chance of a dropout on a busy host.

The rate is the machine's own, 32 kHz, the SC-55mkII's 66206 Hz and the SC-55's 64000 Hz, or 32000, 44100 or 48000; a
device that will not open at the one asked for runs at one of its own.  The machine always renders at
its own rate, and whatever the device opens at, the output is converted to it with libsamplerate's
medium sinc.

**Interface**: how the panel is drawn and what the pointer's buttons do.  The **panel size** is the
artwork's two bakes -- the small panel, or twice as large -- and changing it redraws the window at
once, keys held and all; it is `--size 4|8` on the command line.  **Swapping the right and middle
mouse buttons** puts the combination menu on a plain right-click and moves the queue-and-hold gesture
to the middle button, for a mouse whose middle button is a wheel to press or none at all.  Pressing the
other half of a ◀ ▶ pair stays on the right button, since it happens while the left button holds the
first half and no menu is wanted there, and Ctrl with the right button opens the menu either way round.

**System**: which machine this is -- SC-55, SC-55mkII, SC-88, SC-88VL, SC-88Pro, SC-8820 or SC-8850.  A model whose
ROM images were not found is greyed out.  Choosing another one switches at once: the song is
unloaded, the machine is replaced and the new one comes up from its own boot cache, so it is instant
from the second time on; the sound card and the MIDI ties stay as they are.  Below the choice is what
is running: the model, its control ROM version, and the file its images came from.

The **computer switch** is the row under the systems: the switch on the back of the unit, in the
positions that system has -- MIDI, PC-1, PC-2 and Mac on the 88 family and the SC-55mkII, MIDI, PC-1,
PC-2 and USB on the SC-8850; the SC-55 has none, and the row is greyed out.  It belongs to the system,
not to the program, so each one keeps its own;
the row always shows the running one.  The firmware reads the ladder once when it comes up, so
changing it replaces the machine and boots it again, the way changing the model does (from that
position's own boot cache).
MIDI is the default on the 88 family, where the sub-CPU is emulated at a high level and feeds the
firmware from the jacks whatever the switch says, so only the boot differs.  It is the default on the
SC-55mkII as well, whose sub-CPU is emulated the same way but does follow the switch: on the three
positions that are not MIDI the machine listens on the computer port instead of MIDI IN 1 and answers
there, and it plays on any of them.  USB is the default on the SC-8850: it is the position that
carries all four port groups A-D, 64 parts, so a four-port song
plays whole and the playlist window gets the rows for C and D; its boot puts up the "USB On Line" box.
MIDI there is the two jacks, groups A and B, and PC-1 and PC-2 take the parts off the jacks and wait
for a serial host, which is not answered here, so the machine plays nothing.  This is not the MIDI
speed: a computer port also runs at 38400 baud, which is `--midi-rate`.

**Clear cache** is a button on the same tab.  The players keep a boot snapshot per machine, ROM set and
COMPUTER position in `~/.cache/scemu`, with the factory settings image the firmware wrote on its first
run, so a machine comes up instantly instead of running its boot again.  The button throws those away
and says how many files went; the next start of a machine boots the firmware for real and writes a fresh
snapshot.  What a machine remembers -- its own settings memory, kept when `keep_settings` is on -- is not
a cache and is left alone.  Use it after a new build of scemu, whose machine a snapshot taken by the old
one no longer matches.

## The settings file

`$XDG_CONFIG_HOME/scemu/scgui.conf`, or `~/.config/scemu/scgui.conf`, keeps what the windows set: the
machine and where its ROMs are, the window size (`size`) and whether the pointer's buttons are swapped
(`swap_buttons`), the output device, the rate asked of it (`audio_rate`,
0 for the machine's own) and its buffer, the volume knob,
the nine MIDI ties by the device's name, the message before each song, the instrument
map, the MIDI speed, the computer switch of each system (`computer_sc88`, `computer_sc88vl`,
`computer_sc88pro`, `computer_sc8820`, `computer_sc8850` and `computer_sc55mk2`, each `midi`, `pc1`,
`pc2` or `mac`, with `usb` taken as the SC-8850's and the SC-8820's word for the last), whether the
settings memory is kept, the tail, and the Sound Brush's: whether its panel is shown (`sb55_window`)
and its three system functions (`sb55_interval`, `sb55_auto_play`, `sb55_auto_rewind`).
It is read at start, written a moment after a setting changes, and written again on exit.  An option
on the command line overrides the file for that run and does not change it.  The file is `key = value` text with `#` comments; a
line it cannot make sense of is complained about on stderr and every other line is still taken.

## Building

Needs GTK 4 and zlib, besides the library; PortAudio, PortMidi and libsamplerate are built from the submodules, so
clone with them (`git submodule update --init`).  Without GTK 4 the build skips it and
still makes `scplay`.
