/* scemu GUI: text in the glass's own 5x7 font.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "text.h"
#include "lcd_font.h"

/* one code point of UTF-8, as a font code */
static uint8_t next_code(const char **p)
{
	const unsigned char *s = (const unsigned char *)*p;
	uint32_t cp = s[0];
	int extra = 0;
	if (cp >= 0xf0) { cp &= 0x07; extra = 3; }
	else if (cp >= 0xe0) { cp &= 0x0f; extra = 2; }
	else if (cp >= 0xc0) { cp &= 0x1f; extra = 1; }
	s++;
	for (int n = 0; n < extra && (*s & 0xc0) == 0x80; n++, s++)
		cp = (cp << 6) | (*s & 0x3f);
	*p = (const char *)s;
	if (cp < 0x80)
		return (uint8_t)cp;
	switch (cp)
	{
	case 0x25c0: return 0x7f;   /* ◀ */
	case 0x25b6: return 0x7e;   /* ▶ */
	case 0x00b7: return '.';    /* · */
	default: return '?';
	}
}

int text_width(int scale, const char *utf8)
{
	int n = 0;
	while (*utf8)
	{
		next_code(&utf8);
		n++;
	}
	return n * TEXT_CELL_W * scale;
}

static void put_glyph(const text_frame_t *f, int x0, int y0, int s, uint32_t color, uint8_t code)
{
	const uint8_t *rows = lcd_font_glyph(code);
	for (int r = 0; r < 7; r++)
		for (int c = 0; c < 5; c++)
		{
			if (!(rows[r] & (0x10 >> c)))
				continue;
			text_fill(f, x0 + c * s, y0 + r * s, s, s, color);
		}
}

void text_draw(const text_frame_t *f, int x, int y, int scale, uint32_t color, const char *utf8)
{
	while (*utf8)
	{
		put_glyph(f, x, y, scale, color, next_code(&utf8));
		x += TEXT_CELL_W * scale;
	}
}

void text_fill(const text_frame_t *f, int x, int y, int w, int h, uint32_t color)
{
	int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
	int x1 = x + w > f->width ? f->width : x + w, y1 = y + h > f->height ? f->height : y + h;
	for (int yy = y0; yy < y1; yy++)
		for (int xx = x0; xx < x1; xx++)
			f->pixels[(size_t)yy * f->stride + xx] = color;
}
