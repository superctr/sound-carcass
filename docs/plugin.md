# The plugin

`scemu.clap` is the emulator as a CLAP instrument, one per model: SC-88, SC-88VL, SC-88Pro, SC-8850
and SC-55mkII appear in the host's instrument list as separate plugins, each running its own firmware
on its own emulated board.  What comes out is the machine's DAC words, not a rendering of them, so the
plugin sounds like the players and the command-line renderer to the bit when the host runs at the
machine's own rate.

## Installing

The build makes `scemu.clap` beside the other tools (`-DSCEMU_BUILD_PLUGIN=OFF` leaves it out).  Put it
where the host looks: `~/.clap/` or `/usr/lib/clap/` on Linux.  A host that runs in a sandbox
(Flatpak) has to be allowed to see that directory and the ROMs.

The ROM images are found the way the players find them (`roms.md`): recognised by content, in any zip
or directory beside the plugin file or in `~/.mame/roms`, or under the path in the `SCEMU_ROMS`
environment variable.  An instrument whose set is not there loads all the same and stays silent; the
host's log says which set is missing.

## What the host sees

- **Two stereo outputs**, OUTPUT 1 and OUTPUT 2, the unit's own jacks.  Models with one pair leave
  OUTPUT 2 silent.  The samples are the machine's 24-bit words on the float scale, times the volume.
- **MIDI IN A and B** as note ports taking MIDI (channel messages and sysex alike; a host that sends
  CLAP note events gets them turned into note on and off).  The SC-88Pro and later play 32 parts over
  the two.  The input is paced at the cable's speed, as the real MIDI IN is, so a dense block of
  sysex at the start of a track takes its wire time before the notes after it sound, exactly as on
  the unit.
- **MIDI OUT** as a note port out: what the machine sends, an identity reply for instance, comes back
  as events.
- **Parameters**: the volume knob; the instrument map (the song's own, or every part forced onto the
  SC-55, SC-88, SC-88Pro or SC-8850 map); the MIDI input speed (31250 baud, the 38400 of the computer
  port, or unlimited, which loses setup messages on some songs); the output headroom, the DAC rail
  in bits (24 is the unit's, which busy songs clip on; up to 29 gives 30 dB above it, the samples then
  going above 0 dBFS as float allows).
- **State**: the whole machine is the plugin's state, every part exactly as the track's sysex left
  it, so a project reopens sounding as it was saved, with the notes that were held still held.  A state
  from another model is refused.

The machine boots when the host activates the instrument: about half a second the first time, instant
afterwards from the boot cache the players keep too (`~/.cache/scemu`).  The plugin always starts from
factory settings; it does not keep the unit's settings memory across sessions.

## Not there yet

The window with the front panel; MIDI IN C and D on an SC-8850 on USB; a VST3 build.
