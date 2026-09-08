# Drawing the display

The SC-88 family's and the SC-55mkII's display is a custom glass (Roland's RCM2024T) driven by an
HD44780-compatible controller in 2-line × 40-character mode.  `scemu_lcd()` hands the host the
controller's memory: `ddram[0..39]` is line 0, `ddram[40..79]` is line 1, `cgram` holds the eight
user-defined 5×8 characters (8 bytes each, one row per byte, bits 4..0 left to right), `display_on` is
the controller's display flag and `changed` is set whenever any of it was written (clear it with
`scemu_lcd_ack()`).

Only some cells are wired to segments on the glass.  A cell is a 5×7 character (row 7 of a pattern is
never visible) unless noted.

## Line 0

| cells | segments |
|---|---|
| 0-2 | the PART field ("A01"), left column, top row |
| 3-18 | the INSTRUMENT field: number and name, 16 characters, right column, top row |
| 19 | not wired |
| 20-23 | the upper halves of the 16 level bars (see below) |
| 24-39 | not wired |

## Line 1

| cells | segments |
|---|---|
| 0-2 | LEVEL (left column, row 1) |
| 3-5 | PAN (right column, row 1) |
| 6-8 | CHORUS (right column, row 2) |
| 9-11 | REVERB (left column, row 2) |
| 12-14 | K SHIFT (left column, row 3) |
| 15-17 | MIDI CH (right column, row 3) |
| 18 | the L and R marks beside the bars: both follow pixel (x = 4, y = 0) of this cell's pattern |
| 19 | not wired |
| 20-23 | the lower halves of the 16 level bars |
| 24-39 | not wired |

## The level bars

Sixteen bars of sixteen segments each, one per part.  Cells 20-23 of each line are drawn with their
CGRAM patterns; column x of cell 20 + k is bar 5k + x (bars 0-15, so cell 23 uses columns 0 only),
and pattern row y is segment y of the upper half (line 0) or 8 + y of the lower half (line 1), counted
from the top.  The firmware fills the patterns so a bar's lit segments run from the bottom.

## Characters

Codes 0-7 are the CGRAM patterns; everything else is a character of the module's own generator, which
is not a Hitachi one.  The firmwares write ASCII, and two codes beside it: `0x11` is `±`, the first
character of the SC-88 family's KEY SHIFT field while the shift is zero, and `0x1a` is the `Ⅱ` the
SC-55mkII writes in `SC-55 mkⅡ` all through its boot.

## Geometry

Positions in the driver's 720 × 272 rendering of the glass, for a host that wants the same look:

| item | position |
|---|---|
| left column x | 24 |
| right column x | 143 |
| character pitch | 35 |
| rows y | 14, 78, 142, 206 |
| dot pitch / size | 6 / 5 |
| bars: x, y, pitch x, pitch y, size | 283, 74, 26, 11, 24 × 9 |
| L mark / R mark | (254, 74) / (254, 234), 11 × 12 |

Colours the SC-88's backlight gives: background `#f8c840`, segment on `#201000`.
