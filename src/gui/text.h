/* scemu GUI: text in the glass's own 5x7 font, for a window with no toolkit.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCEMU_TEXT_H
#define SCEMU_TEXT_H

#include <stddef.h>
#include <stdint.h>

/* A frame of 0xAARRGGBB pixels, `stride` per row, `width` x `height` drawn on. */
typedef struct text_frame
{
	uint32_t *pixels;
	size_t stride;
	int width, height;
} text_frame_t;

/* a character cell is 6 x 8 dots times `scale` */
#define TEXT_CELL_W 6
#define TEXT_CELL_H 8

/* UTF-8 in; the panel's arrows and the middle dot have their glyphs, anything
 * else outside ASCII draws as a question mark */
int text_width(int scale, const char *utf8);
void text_draw(const text_frame_t *f, int x, int y, int scale, uint32_t color, const char *utf8);
void text_fill(const text_frame_t *f, int x, int y, int w, int h, uint32_t color);

#endif
