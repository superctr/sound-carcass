# ROM images

scemu needs the control ROM and the wave ROMs of the machine it emulates, as plain dumps of the chips.
You need your own; none are shipped.

## How they are found

Images are recognised by their contents — the CRC32 and the size of the dump — so **names do not
matter**, neither the file's nor a zip entry's.  A MAME ROM collection is found as it stands, and so is
a folder of files called anything at all.  The places looked at, in order:

1. the path given to `--rom`, a zip or a directory;
2. the directory the program is in;
3. `~/.mame/roms`.

In each directory every `*.zip` and every loose file is examined, and so is one level of
subdirectories, so an unpacked `sc88pro/` next to the zips is read too.  Zips are matched from their
central directory alone; only the images actually used are unpacked.  A loose file is read only when
its size is one of the sizes below.  The first place holding an image wins, so `--rom` overrides the
rest and can supply an image the others lack.

A set may be spread over several places: the SC-88VL's own wave ROMs have never been dumped and it
plays the SC-88's, so `sc88vl.zip` (the control ROM alone) and `sc88.zip` (the waves) together make a
complete SC-88VL.

Where several versions of a control ROM are there, the newest is taken.

## SC-88

| image | size | CRC32 |
|---|---|---|
| control ROM 1.02, IC17 | 512 KiB | `9e9c56f9` |
| control ROM 1.03, IC17 | 512 KiB | `cf953f71` |
| control ROM 1.04, IC17 | 512 KiB | `979b6c09` |
| wave ROM IC14 | 2 MiB | `f9dd9e49` |
| wave ROM IC8 | 2 MiB | `05f939f2` |
| wave ROM IC7 | 2 MiB | `a6fc7393` |
| wave ROM IC6 | 2 MiB | `7bc514aa` |

The 1.02 and 1.03 dumps in circulation have the bytes of every 16-bit word the other way round from the
1.04 dump; that is a property of the dump, not of the chip, and the loader puts it right.  (The 1.04
image is also circulated labelled "Version 1.01"; it is 1.04.)

## SC-88VL

| image | size | CRC32 |
|---|---|---|
| control ROM 1.04, IC29 | 512 KiB | `66aa5762` |
| the four SC-88 wave ROMs above | | |

## SC-88Pro

| image | size | CRC32 |
|---|---|---|
| control ROM 1.02, IC26 | 1 MiB | `7b0d392d` |
| control ROM 1.04, IC26 | 1 MiB | `820824d2` |
| wave ROM IC20 | 4 MiB | `84dfea65` |
| wave ROM IC21 | 4 MiB | `60210227` |
| wave ROM IC22 | 4 MiB | `5f883ddd` |
| wave ROM IC23 | 4 MiB | `ecb4dd39` |
| wave ROM IC24 | 4 MiB | `93541e95` |

The SC-88Pro's wave images are reconstructed from the vegspro dumps — the same sample data, on wider
chips there.

## SC-8850

Five images: the CPU's internal boot ROM, the program flash the firmware itself lives in, the tone
flash (its parameter and drum tables) and two 16 MB wave ROMs.

| image | size | CRC32 |
|---|---|---|
| CPU ROM, IC1 | 64 KiB | `4b2f36e3` |
| program flash 1.00, IC9 | 1 MiB | `3ef69f93` |
| tone flash, IC10 | 2 MiB | `390faa62` |
| wave ROM IC53 | 16 MiB | `2cfe5aa2` |
| wave ROM IC54 | 16 MiB | `623015b6` |

The USB controller's mask ROM has never been dumped and is not needed: what the controller says on
its two mailboxes is emulated at a high level, so the machine comes up as one with the controller
fitted — the boot's box reads "USB On Line" when the rear COMPUTER switch is on USB, and the four
MIDI port groups A-D, 64 parts, go through it.

## The sub-CPU

The panel sub-CPU's internal 8 KiB ROM is not dumped on any of the 88-family machines and is not
needed: its behaviour is emulated at a high level.  The SC-8850 has no sub-CPU — its panel and
display hang on the main CPU's own bus.
