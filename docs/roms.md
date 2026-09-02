# ROM images

scemu needs the control ROM and the wave ROMs of the machine it emulates, as plain dumps of the chips.
File names below are the ones `scemu-cli` looks for in the ROM directory; the library itself takes the
images as memory and does not care about names.

## SC-88

| file | size | chip |
|---|---|---|
| `roland_sc88-control-1.04.ic17` | 512 KiB | control ROM (HN27C4096), big-endian words |
| `sc88-pcm-ic-325.ic14` | 2 MiB | wave ROM A |
| `sc88-pcm-ic-326.ic8` | 2 MiB | wave ROM B |
| `sc88-pcm-ic-327.ic7` | 2 MiB | wave ROM C |
| `sc88-pcm-ic-328.ic6` | 2 MiB | wave ROM D |

## SC-88Pro

| file | size | chip |
|---|---|---|
| `roland_sc88pro-1.04.ic26` | 1 MiB | control ROM |
| `roland-r01017834-378.ic20` | 4 MiB | wave ROM |
| `roland-r01017845-379.ic21` | 4 MiB | wave ROM |
| `roland-r01124778-519.ic22` | 4 MiB | wave ROM |
| `roland-r01124789-520.ic23` | 4 MiB | wave ROM |
| `roland-r01124790-521.ic24` | 4 MiB | wave ROM |

The sub-CPU's internal 8 KiB ROM is not dumped on either machine and is not needed: its behaviour is
emulated at a high level.
