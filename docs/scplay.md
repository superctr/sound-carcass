# scplay

`scplay` plays a Standard MIDI File through an emulated SC-88, SC-88VL or SC-88Pro and draws the
machine's front panel in the terminal: the LCD's text fields, the sixteen level bars as they animate,
the panel LEDs and a clock.  The panel buttons are on the keyboard, so the display can be walked
through the parts while the music plays.

```
scplay song.mid
```

That is the whole normal command line.  The player boots the firmware once, caches the booted machine
and starts instantly on later runs.

## Options

| option | effect |
|---|---|
| `--model sc88 \| sc88vl \| sc88pro` | which machine (default `sc88pro`) |
| `--rom PATH` | a zip or a directory holding the ROM images, searched before the usual places |
| `--wav FILE` | also write what is played, 16-bit stereo at the machine's own 32 kHz |
| `--no-audio` | render as fast as the host allows and open no sound card; for `--wav` |
| `--no-cache` | boot the firmware instead of loading the cached boot state, and use no cached settings memory |
| `--keep-settings` | start from the settings memory the last `--keep-settings` run left, and save it again on exit |
| `--midi-rate BAUD` | the speed of the MIDI input: 31250 is the cable and the default, 38400 the SC-88Pro's computer port, 0 removes the limit.  Faster than the firmware can take loses messages inside it: a song with a large setup block right after its GS reset plays with wrong sounds and levels at 0 |
| `--tail N` | seconds to keep running after the last MIDI event (default 4) |
| `--port 0 \| 1` | the MIDI IN, A or B, for tracks that name no port (default A).  A track with a port event (Cakewalk's and Roland's dual-port files) goes to A for port 0 and B for port 1, so a 32-part song plays on both blocks.  Tracks on ports 2 and 3, an SC-8850's C and D, are not played |
| `--map sc55 \| sc88 \| sc88pro` | play every part from that instrument map whatever the song selects: the file's bank select LSBs are rewritten, a part gets the selection before its first program change, and every reset (GS, GM, XG, SC-88 mode set) is followed by the selection on all sixteen parts.  The SC-88 has no SC-88Pro map and plays its own for it |
| `--hold` | wait for a key when the song ends instead of exiting |
| `--help` | usage |

## ROM images

You need your own dumps; the file names are the ones in [roms.md](roms.md).  Without `--rom`, scplay
looks for each model's set in, in order:

1. `<model>.zip` beside the executable, then a directory `<model>/` beside it;
2. `~/.mame/roms/<model>.zip`, then `~/.mame/roms/<model>/`;
3. the executable's own directory and `~/.mame/roms` as loose files.

`<model>` is `sc88`, `sc88vl` or `sc88pro`, so a MAME ROM collection is found as it stands.  Zips are
read directly (stored and deflated entries).  Where a zip holds several control ROM versions, the 1.04
image named in `roms.md` is the one taken.

`--rom` adds its zip or directory at the front of that list; the usual places still fill in anything it
does not hold.

**The SC-88VL** shares the SC-88's wave ROMs — its own mask ROMs have never been dumped — so its four
wave images are looked for in the `sc88` set as well as in `sc88vl`.  In practice `sc88vl.zip` holds
only the control ROM and `sc88.zip` supplies the rest, and both must be present.

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
| `boot-<model>-<hash>.state` | the machine a moment after it came up, before any MIDI; the hash covers every ROM image, so changing a ROM boots afresh by itself |
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

A button is held down for about fifty milliseconds of machine time, long enough for the sub-CPU's
matrix scan to see it.

## The display

The panel is drawn from the LCD controller's memory.  The text fields are drawn as text; the sixteen
level bars are drawn from the CGRAM patterns the firmware fills, two of a bar's sixteen segments to a
character cell, so they move exactly as the glass does.  The L and R marks beside the bars follow the
same pixel the hardware wires them to.

The LED row shows the panel's nine lamps; the SC-88's third button is labelled EQ, the Pro's second
lens die appears as EFX.

Above the panel stand the model, the file name and the song's own title: its first track name, or a
text event on the first track for the many files that put the title there instead.  A title is kept as
it stands when it is well-formed UTF-8 and decoded as Shift-JIS when it is not, so the Japanese titles
most of the older files carry read as they were written; the line is cut by display columns, counting a
full-width character as two.

The terminal wants to be at least 70 columns wide and 22 rows tall (30 with the key list showing).  The
display is redrawn at most thirty times a second and only when something changed.  Colour is 256-colour
ANSI; the terminal is left as it was found, including after Ctrl-C.

## Audio

Output goes through SDL2 at the machine's own 32 kHz, stereo, 16-bit — SDL resamples for a device that
insists on 44.1 or 48 kHz.  The emulation fills a ring buffer about 200 ms deep that SDL's callback
drains; if the host cannot keep up, scplay counts the dropouts and shows them.  The Pro's OUTPUT 2 is
not played; only OUTPUT 1 is.

Without a sound card, `--no-audio --wav out.wav` renders the file as fast as the machine allows — about
nine to twelve times real time on a current desktop.
