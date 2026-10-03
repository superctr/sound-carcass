/* scemu GUI: the display controller's character generator, 5x7 patterns.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCEMU_LCD_FONT_H
#define SCEMU_LCD_FONT_H

#include <stdint.h>

/* Seven rows top to bottom, bit 4 the leftmost column.  Codes without a
 * pattern are blank; codes 0-7 are the user-defined characters and are
 * the caller's to look up. */
const uint8_t *lcd_font_glyph(uint8_t code);

#endif
