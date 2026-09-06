# scplay

`scplay` plays a Standard MIDI File through an emulated SC-88, SC-88VL, SC-88Pro or SC-8850 and draws
the machine's front panel in the terminal: on the 88 family the LCD's text fields, the sixteen level
bars as they animate, the panel LEDs and a clock; on the SC-8850 the whole 160 × 64 graphic display,
drawn dot for dot in half-block characters.  The panel buttons are on the keyboard, so the display can
be walked through the parts while the music plays.

```
scplay song.mid
```

That is the whole normal command line.  The player boots the firmware once, caches the booted machine
and starts instantly on later runs.

## Options

| option | effect |
|---|---|
| `--model sc88 \| sc88vl \| sc88pro \| sc8850` | which machine (default `sc88pro`) |
| `--rom PATH` | a zip or a directory holding the ROM images, whatever they are named, searched before the usual places |
| `--wav FILE` | also write what is played, 16-bit stereo at the machine's own 32 kHz |
| `--no-audio` | render as fast as the host allows and open no sound card; for `--wav` |
| `--audio-device NAME` | play on the output device whose name holds `NAME` (any case) instead of the host's default; `--audio-device list` prints them and exits |
| `--audio-block N` | the device's buffer in frames (default 256, 8 ms); the player keeps two of them ahead, or two of the host's own period when that is larger |
| `--no-cache` | boot the firmware instead of loading the cached boot state, and use no cached settings memory |
| `--keep-settings` | start from the settings memory the last `--keep-settings` run left, and save it again on exit |
| `--midi-rate BAUD` | the speed of the MIDI input: 31250 is the cable and the default, 38400 the SC-88Pro's computer port, 0 removes the limit.  Faster than the firmware can take loses messages inside it: a song with a large setup block right after its GS reset plays with wrong sounds and levels at 0 |
| `--computer midi \| pc1 \| pc2 \| mac` | the switch on the back (default `midi`).  The firmware reads the ladder once when it comes up, so the position belongs to the boot and each one has a boot cache of its own; `usb` is the SC-8850's word for the fourth position and `mac` is everyone else's, and either word is taken.  On the SC-8850 the three other positions take the parts off the MIDI IN jacks and wait for a serial or a USB host, neither of which is answered here yet, so the machine plays nothing; on the 88 family the sub-CPU is emulated at a high level and feeds the firmware from the jacks whatever the switch says, so the song still plays, only the boot differs |
| `--tail N` | seconds to keep running after the last MIDI event (default 4) |
| `--port 0 \| 1` | the MIDI IN, A or B, for tracks that name no port (default A).  A track with a port event (Cakewalk's and Roland's dual-port files) goes to A for port 0 and B for port 1, so a 32-part song plays on both blocks.  Tracks on ports 2 and 3, an SC-8850's C and D, are not played |
| `--map sc55 \| sc88 \| sc88pro` | play every part from that instrument map whatever the song selects: the file's bank select LSBs are rewritten, a part gets the selection before its first program change, and every reset (GS, GM, XG, SC-88 mode set) is followed by the selection on all sixteen parts.  The SC-88 has no SC-88Pro map and plays its own for it |
| `--hold` | wait for a key when the song ends instead of exiting |
| `--help` | usage |

## ROM images

You need your own dumps.  Which images each machine takes is [roms.md](roms.md); they are recognised by
their contents, so their names, and the names of the zips holding them, do not matter.  scplay looks in
the path given to `--rom`, then in its own directory, then in `~/.mame/roms`, reading every zip and
every loose file there and one level of subdirectories.  A MAME ROM collection is found as it stands.

The first place holding an image wins, so `--rom` overrides the others and can also supply just the one
image they lack.  Where several versions of a control ROM are found, the newest is taken.

**The SC-88VL** shares the SC-88's wave ROMs — its own mask ROMs have never been dumped.  In practice
`sc88vl.zip` holds only the control ROM and the SC-88's images supply the rest, so both must be there.

If no set is found for the default model but another model's set is, scplay uses that one and says so.
Naming a model with `--model` turns that off: the named model's set must be there.

## Booting, the boot cache and the settings memory

The firmware takes about six seconds of machine time to boot.  scplay runs that as fast as the host
allows, showing the LCD as the machine walks through its startup, and holds the MIDI file until the H8
releases the analog mute — the same point `scemu_boot` waits for.

The booted machine is then saved, so later runs of the same model on the same ROM images start with no
wait at all.  The cache lives in `$XDG_CACHE_HOME/scemu/`, or `~/.cache/scemu/` when that is unset:

| file | what it is |
|---|---|
| `boot-<model>-<hash>.state` | the machine a moment after it came up, before any MIDI; the hash covers every ROM image, so changing a ROM boots afresh by itself.  A `--computer` position other than MIDI writes its own file (`boot-<model>-pc1-<hash>.state` and so on), since the firmware's boot path depends on the switch |
| `<model>-factory.nvram` | the settings memory as the firmware's own first power-on initialisation left it |
| `<model>.nvram` | the settings memory a `--keep-settings` run left; only ever written and read with that switch |

A machine whose settings memory is blank runs a longer first-power-on path than one that has been
switched on before — on the SC-88Pro that is an extra effect-processor upload and about 0.2 s of machine
time.  So the very first run boots from blank, keeps what the firmware wrote as `<model>-factory.nvram`
and caches nothing; the second run boots from that image, the way a machine that has been used before
comes up, and *that* boot is the one cached.  From the third run on there is no boot at all.  A cached
start renders exactly the same samples as the boot it was taken from.

**Every run starts from factory settings by default.**  Many Standard MIDI Files send no GM or GS reset
of their own, so a unit that remembered the last session's settings would play them differently.  The
cached state is taken before any MIDI reaches the machine and the factory image is the firmware's own,
so neither carries anything from an earlier song.

`--keep-settings` turns that off: the machine starts from `<model>.nvram` if that file exists, and the
settings memory is written back to it when the player exits.  A run that starts from a saved settings
memory boots the firmware rather than loading the cached state, so it costs about a second.

`--no-cache` bypasses the whole directory: no cached state, no saved settings memory, and the firmware
boots from a blank settings memory every time.

## Keys

The 88 family's panel:

| key | button or action |
|---|---|
| `space` | pause / resume |
| `q`, `Esc` | quit |
| `?` | show or hide the full key list |
| `←` `→` | PART |
| `↑` `↓` | INSTRUMENT |
| `-` `=` | LEVEL |
| `[` `]` | PAN |
| `;` `'` | REVERB |
| `,` `.` | CHORUS |
| `k` `l` | KEY SHIFT |
| `n` `m` | MIDI CH |
| `a` | ALL |
| `x` | MUTE |
| `5` | SC-55 map |
| `8` | SC-88 map (EQ on the SC-88) |
| `u` | USER INST / EFX |
| `s` | SELECT |
| `v` | PREVIEW — the volume knob's push switch |

The SC-8850's, which has other keys and a value dial:

| key | button or action |
|---|---|
| `space` | pause / resume |
| `q`, `Esc` | quit |
| `?` | show or hide the full key list |
| `←` `→` | PART |
| `↑` `↓` | the up and down keys |
| `[` `]` | the VALUE dial, one detent counter-clockwise or clockwise |
| `-` `=` | DEC / INC |
| `1` `2` `3` `4` | F1 to F4, the keys under the display |
| `m` | INST MAP |
| `e` | EDIT / UTIL |
| `d` | DRUM |
| `f` | EFFECTS |
| `s` | SHIFT |
| `o` | SOLO |
| `x` | MUTE |
| `Return` | ENTER |
| `Backspace` | EXIT |
| `v` | PREVIEW — the volume knob's push switch |

A button is held down for about fifty milliseconds of machine time, long enough for the panel scan to
see it; two keys typed together in one burst are therefore seen held together, which is how the
SHIFT combinations are reached.  The dial is not a button: each `[` or `]` is one detent of the
encoder, and holding the key down turns it as fast as the terminal repeats.

## The display

On the 88 family the panel is drawn from the LCD controller's memory.  The text fields are drawn as
text; the sixteen level bars are drawn from the CGRAM patterns the firmware fills, two of a bar's
sixteen segments to a character cell, so they move exactly as the glass does.  The L and R marks
beside the bars follow the same pixel the hardware wires them to.

The LED row shows the panel's nine lamps; the SC-88's third button is labelled EQ, the Pro's second
lens die appears as EFX.

**The SC-8850** draws everything — its letters as much as its instrument pictures and part meters —
into one 160 × 64 bitmap, and the terminal shows that bitmap as it stands: one character cell to a
column and two dot rows, in `▀`, `▄`, `█` and blanks, so the whole display is 160 columns and 32 rows.
Its LED row is MUTE, SOLO, EDIT, DRUM and EFFECTS.  A terminal narrower than the display shows as much
as fits and cuts the rest off on the right; nothing is scaled.

Above the panel stand the model, the file name and the song's own title: its first track name, or a
text event on the first track for the many files that put the title there instead.  A title is kept as
it stands when it is well-formed UTF-8 and decoded as Shift-JIS when it is not, so the Japanese titles
most of the older files carry read as they were written; the line is cut by display columns, counting a
full-width character as two.

The terminal wants to be at least 70 columns wide and 22 rows tall (30 with the key list showing); the
SC-8850 wants 163 columns and 46 rows (54 with the key list), and anything narrower loses the right
edge of the display.  The display is redrawn at most thirty times a second and only when something
changed.  Colour is 256-colour
ANSI; the terminal is left as it was found, including after Ctrl-C.

## Audio

Output goes through PortAudio at the machine's own 32 kHz, stereo, 16-bit, on the host's default
device or the one `--audio-device` names (`--audio-device list` shows what there is, with its host
API: on Linux ALSA, JACK and PulseAudio).  A device that will not open at 32 kHz runs at its own rate
and the output is resampled on the way, through a 32-tap windowed sinc.  The emulation keeps two
device buffers (`--audio-block`, 256 frames or 8 ms by default; the host's own period when that is
larger, as under JACK) ahead of PortAudio's callback; if the host cannot keep up, scplay counts the
dropouts and shows them.  The Pro's OUTPUT 2 is not played;
only OUTPUT 1 is.

Without a sound card, `--no-audio --wav out.wav` renders the file as fast as the machine allows — about
nine to twelve times real time on a current desktop.
