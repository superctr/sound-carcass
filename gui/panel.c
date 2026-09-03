/* scemu GUI: the front panel compositor.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "panel.h"
#include "png.h"
#include "lcd_font.h"

extern const unsigned char panel_base_p4_png[];
extern const size_t panel_base_p4_png_size;
extern const unsigned char panel_atlas_p4_png[];
extern const size_t panel_atlas_p4_png_size;
extern const unsigned char panel_base_p8_png[];
extern const size_t panel_base_p8_png_size;
extern const unsigned char panel_atlas_p8_png[];
extern const size_t panel_atlas_p8_png_size;

static const struct
{
	int pitch;
	const unsigned char *base, *atlas;
	const size_t *base_size, *atlas_size;
} artwork[] = {
	{ 4, panel_base_p4_png, panel_atlas_p4_png, &panel_base_p4_png_size, &panel_atlas_p4_png_size },
	{ 8, panel_base_p8_png, panel_atlas_p8_png, &panel_base_p8_png_size, &panel_atlas_p8_png_size },
};

#define SEG_COLOR 0xff201000u

struct panel
{
	const panel_size_t *size;
	png_image_t base;
	png_image_t atlas;      /* premultiplied */
	scemu_lcd_t lcd;
	uint32_t leds;
	float knob;
	uint64_t pressed;
	bool dirty;
};

static uint32_t premultiply(uint32_t c)
{
	uint32_t a = c >> 24;
	if (a == 0xff)
		return c;
	uint32_t r = ((c >> 16) & 0xff) * a / 255, g = ((c >> 8) & 0xff) * a / 255, b = (c & 0xff) * a / 255;
	return (a << 24) | (r << 16) | (g << 8) | b;
}

panel_t *panel_create(int pitch)
{
	const panel_size_t *size = NULL;
	for (int n = 0; n < PANEL_SIZE_COUNT; n++)
		if (panel_sizes[n].pitch == pitch)
			size = &panel_sizes[n];
	size_t art = 0;
	while (art < sizeof(artwork) / sizeof(artwork[0]) && artwork[art].pitch != pitch)
		art++;
	if (!size || art == sizeof(artwork) / sizeof(artwork[0]))
		return NULL;

	panel_t *p = calloc(1, sizeof(*p));
	if (!p)
		return NULL;
	p->size = size;
	if (!png_decode(artwork[art].base, *artwork[art].base_size, &p->base)
	    || !png_decode(artwork[art].atlas, *artwork[art].atlas_size, &p->atlas)
	    || p->base.width != size->width || p->base.height != size->height)
	{
		panel_destroy(p);
		return NULL;
	}
	for (size_t n = 0; n < (size_t)p->atlas.width * p->atlas.height; n++)
		p->atlas.pixels[n] = premultiply(p->atlas.pixels[n]);
	memset(p->lcd.ddram, ' ', sizeof(p->lcd.ddram));
	p->knob = 0.75f;
	p->dirty = true;
	return p;
}

void panel_destroy(panel_t *p)
{
	if (!p)
		return;
	free(p->base.pixels);
	free(p->atlas.pixels);
	free(p);
}

int panel_width(const panel_t *p) { return p->size->width; }
int panel_height(const panel_t *p) { return p->size->height; }
int panel_pitch(const panel_t *p) { return p->size->pitch; }
const panel_rect_t *panel_element_rect(const panel_t *p, panel_element_t e) { return &p->size->element[e]; }

void panel_set_lcd(panel_t *p, const scemu_lcd_t *lcd)
{
	if (memcmp(p->lcd.ddram, lcd->ddram, sizeof(lcd->ddram)) == 0
	    && memcmp(p->lcd.cgram, lcd->cgram, sizeof(lcd->cgram)) == 0
	    && p->lcd.display_on == lcd->display_on)
		return;
	memcpy(p->lcd.ddram, lcd->ddram, sizeof(lcd->ddram));
	memcpy(p->lcd.cgram, lcd->cgram, sizeof(lcd->cgram));
	p->lcd.display_on = lcd->display_on;
	p->dirty = true;
}

void panel_set_leds(panel_t *p, uint32_t mask)
{
	if (p->leds != mask)
	{
		p->leds = mask;
		p->dirty = true;
	}
}

void panel_set_knob(panel_t *p, float turn)
{
	turn = turn < 0 ? 0 : turn > 1 ? 1 : turn;
	if (p->knob != turn)
	{
		p->knob = turn;
		p->dirty = true;
	}
}

void panel_set_pressed(panel_t *p, panel_element_t e, bool down)
{
	uint64_t bit = (uint64_t)1 << e;
	uint64_t was = p->pressed;
	p->pressed = down ? (was | bit) : (was & ~bit);
	if (p->pressed != was)
		p->dirty = true;
}

bool panel_dirty(const panel_t *p) { return p->dirty; }

/* ---------------------------------------------------------------- drawing */

static void fill(uint32_t *pixels, size_t stride, int x, int y, int w, int h, uint32_t color)
{
	for (int j = 0; j < h; j++)
	{
		uint32_t *row = pixels + (size_t)(y + j) * stride + x;
		for (int i = 0; i < w; i++)
			row[i] = color;
	}
}

static uint32_t over(uint32_t src, uint32_t dst)
{
	uint32_t a = src >> 24;
	if (a == 0xff)
		return src;
	if (a == 0)
		return dst;
	uint32_t ia = 255 - a;
	uint32_t r = ((src >> 16) & 0xff) + ((dst >> 16) & 0xff) * ia / 255;
	uint32_t g = ((src >> 8) & 0xff) + ((dst >> 8) & 0xff) * ia / 255;
	uint32_t b = (src & 0xff) + (dst & 0xff) * ia / 255;
	return 0xff000000u | (r << 16) | (g << 8) | b;
}

static void blit_sprite(panel_t *p, uint32_t *pixels, size_t stride, panel_sprite_id_t id)
{
	const panel_sprite_t *s = &p->size->sprite[id];
	for (int j = 0; j < s->atlas.h; j++)
	{
		const uint32_t *src = p->atlas.pixels + (size_t)(s->atlas.y + j) * p->atlas.width + s->atlas.x;
		uint32_t *dst = pixels + (size_t)(s->y + j) * stride + s->x;
		for (int i = 0; i < s->atlas.w; i++)
			dst[i] = over(src[i], dst[i]);
	}
}

static uint32_t sample(const png_image_t *img, const panel_rect_t *r, float x, float y)
{
	int x0 = (int)floorf(x), y0 = (int)floorf(y);
	float fx = x - (float)x0, fy = y - (float)y0;
	uint32_t acc[4] = { 0, 0, 0, 0 };
	for (int j = 0; j < 2; j++)
		for (int i = 0; i < 2; i++)
		{
			int sx = x0 + i, sy = y0 + j;
			float wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
			if (sx < 0 || sy < 0 || sx >= r->w || sy >= r->h || wgt <= 0)
				continue;
			uint32_t c = img->pixels[(size_t)(r->y + sy) * img->width + r->x + sx];
			uint32_t w256 = (uint32_t)(wgt * 256 + 0.5f);
			acc[0] += (c >> 24) * w256;
			acc[1] += ((c >> 16) & 0xff) * w256;
			acc[2] += ((c >> 8) & 0xff) * w256;
			acc[3] += (c & 0xff) * w256;
		}
	uint32_t a = acc[0] >> 8, rr = acc[1] >> 8, g = acc[2] >> 8, b = acc[3] >> 8;
	if (a > 255) a = 255;
	if (rr > a) rr = a;
	if (g > a) g = a;
	if (b > a) b = a;
	return (a << 24) | (rr << 16) | (g << 8) | b;
}

static void draw_knob(panel_t *p, uint32_t *pixels, size_t stride)
{
	const panel_sprite_t *s = &p->size->sprite[PANEL_SPRITE_KNOB_VOLUME];
	const panel_rect_t *k = &p->size->element[PANEL_KNOB_VOLUME];
	float cx = k->x + k->w / 2.0f - s->x, cy = k->y + k->h / 2.0f - s->y;
	float angle = p->knob * 270.0f * 3.14159265f / 180.0f;
	float c = cosf(angle), sn = sinf(angle);
	for (int j = -1; j <= s->atlas.h; j++)
		for (int i = -1; i <= s->atlas.w; i++)
		{
			float dx = i + 0.5f - cx, dy = j + 0.5f - cy;
			float sx = cx + dx * c + dy * sn - 0.5f, sy = cy - dx * sn + dy * c - 0.5f;
			uint32_t src = sample(&p->atlas, &s->atlas, sx, sy);
			uint32_t *dst = pixels + (size_t)(s->y + j) * stride + s->x + i;
			*dst = over(src, *dst);
		}
}

static const uint8_t *cell_pattern(const panel_t *p, uint8_t code, uint8_t *rows)
{
	if (code < 8)
		return p->lcd.cgram + code * 8;
	memcpy(rows, lcd_font_glyph(code), 7);
	rows[7] = 0;
	return rows;
}

static void cell_origin(const panel_glass_t *g, int line, int cell, int *x, int *y)
{
	static const uint8_t field_col[6] = { 0, 1, 1, 0, 0, 1 };
	static const uint8_t field_row[6] = { 1, 1, 2, 2, 3, 3 };
	if (line == 0)
	{
		*x = cell < 3 ? g->col[0] + cell * g->cell_pitch : g->col[1] + (cell - 3) * g->cell_pitch;
		*y = g->row[0];
	}
	else
	{
		*x = g->col[field_col[cell / 3]] + (cell % 3) * g->cell_pitch;
		*y = g->row[field_row[cell / 3]];
	}
}

static void draw_glass(panel_t *p, uint32_t *pixels, size_t stride)
{
	const panel_glass_t *g = &p->size->glass;
	uint8_t rows[8];
	if (!p->lcd.display_on)
		return;
	for (int line = 0; line < 2; line++)
		for (int cell = 0; cell < (line ? 18 : 19); cell++)
		{
			const uint8_t *pat = cell_pattern(p, p->lcd.ddram[line * 40 + cell], rows);
			int x, y;
			cell_origin(g, line, cell, &x, &y);
			for (int r = 0; r < 7; r++)
				for (int c = 0; c < 5; c++)
					if (pat[r] & (0x10 >> c))
						fill(pixels, stride, x + c * g->dot_pitch, y + r * g->dot_pitch, g->dot, g->dot, SEG_COLOR);
		}
	for (int line = 0; line < 2; line++)
		for (int cell = 20; cell < 24; cell++)
		{
			const uint8_t *pat = cell_pattern(p, p->lcd.ddram[line * 40 + cell], rows);
			for (int r = 0; r < 8; r++)
				for (int c = 0; c < 5; c++)
				{
					int bar = (cell - 20) * 5 + c;
					if (bar < 16 && (pat[r] & (0x10 >> c)))
						fill(pixels, stride, g->bar_x + bar * g->bar_pitch_x, g->bar_y + (line * 8 + r) * g->bar_pitch_y,
						     g->bar_w, g->bar_h, SEG_COLOR);
				}
		}
	const uint8_t *marks = cell_pattern(p, p->lcd.ddram[40 + 18], rows);
	if (marks[0] & 0x01)
	{
		blit_sprite(p, pixels, stride, PANEL_SPRITE_LCD_MARK_L);
		blit_sprite(p, pixels, stride, PANEL_SPRITE_LCD_MARK_R);
	}
}

static const panel_sprite_id_t led_sprite[SCEMU_LED_COUNT] = {
	PANEL_SPRITE_LED_ALL, PANEL_SPRITE_LED_MUTE, PANEL_SPRITE_LED_SC55_MAP, PANEL_SPRITE_LED_SC88_MAP,
	PANEL_SPRITE_LED_EDIT1, PANEL_SPRITE_LED_EDIT2, PANEL_SPRITE_LED_EDIT3,
	PANEL_SPRITE_LED_USER_INST, PANEL_SPRITE_LED_USER_INST_RED,
};

static void darken(uint32_t *pixels, size_t stride, const panel_rect_t *r)
{
	for (int j = 0; j < r->h; j++)
	{
		uint32_t *row = pixels + (size_t)(r->y + j) * stride + r->x;
		for (int i = 0; i < r->w; i++)
		{
			uint32_t c = row[i];
			row[i] = 0xff000000u | (((c >> 16) & 0xff) * 5 / 8) << 16 | (((c >> 8) & 0xff) * 5 / 8) << 8 | ((c & 0xff) * 5 / 8);
		}
	}
}

void panel_render(panel_t *p, uint32_t *pixels, size_t stride)
{
	for (int y = 0; y < p->base.height; y++)
		memcpy(pixels + (size_t)y * stride, p->base.pixels + (size_t)y * p->base.width, (size_t)p->base.width * sizeof(uint32_t));
	draw_glass(p, pixels, stride);
	for (int n = 0; n < SCEMU_LED_COUNT; n++)
		if (p->leds & (1u << n))
			blit_sprite(p, pixels, stride, led_sprite[n]);
	draw_knob(p, pixels, stride);
	for (int e = 0; e < PANEL_ELEMENT_COUNT; e++)
		if (p->pressed & ((uint64_t)1 << e))
			darken(pixels, stride, &p->size->element[e]);
	p->dirty = false;
}

/* ---------------------------------------------------------------- input */

int panel_hit(const panel_t *p, int x, int y)
{
	for (int e = PANEL_ELEMENT_COUNT - 1; e >= 0; e--)
	{
		const panel_rect_t *r = &p->size->element[e];
		if (x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h)
			return e;
	}
	return -1;
}

int panel_element_button(panel_element_t e)
{
	static const int8_t button[PANEL_ELEMENT_COUNT] = {
		[PANEL_SWITCH_POWER] = -1, [PANEL_KNOB_VOLUME] = -1, [PANEL_JACK_MIDI_IN_B] = -1, [PANEL_JACK_PHONES] = -1,
		[PANEL_LCD_GLASS] = -1, [PANEL_LENS_USER_INST] = -1,
		[PANEL_BUTTON_ALL] = SCEMU_BUTTON_ALL, [PANEL_BUTTON_MUTE] = SCEMU_BUTTON_MUTE,
		[PANEL_BUTTON_SC55_MAP] = SCEMU_BUTTON_SC55_MAP, [PANEL_BUTTON_SC88_MAP] = SCEMU_BUTTON_SC88_MAP,
		[PANEL_BUTTON_PART_LEFT] = SCEMU_BUTTON_PART_LEFT, [PANEL_BUTTON_PART_RIGHT] = SCEMU_BUTTON_PART_RIGHT,
		[PANEL_BUTTON_INSTRUMENT_LEFT] = SCEMU_BUTTON_INSTRUMENT_LEFT, [PANEL_BUTTON_INSTRUMENT_RIGHT] = SCEMU_BUTTON_INSTRUMENT_RIGHT,
		[PANEL_BUTTON_LEVEL_LEFT] = SCEMU_BUTTON_LEVEL_LEFT, [PANEL_BUTTON_LEVEL_RIGHT] = SCEMU_BUTTON_LEVEL_RIGHT,
		[PANEL_BUTTON_PAN_LEFT] = SCEMU_BUTTON_PAN_LEFT, [PANEL_BUTTON_PAN_RIGHT] = SCEMU_BUTTON_PAN_RIGHT,
		[PANEL_BUTTON_REVERB_LEFT] = SCEMU_BUTTON_REVERB_LEFT, [PANEL_BUTTON_REVERB_RIGHT] = SCEMU_BUTTON_REVERB_RIGHT,
		[PANEL_BUTTON_CHORUS_LEFT] = SCEMU_BUTTON_CHORUS_LEFT, [PANEL_BUTTON_CHORUS_RIGHT] = SCEMU_BUTTON_CHORUS_RIGHT,
		[PANEL_BUTTON_KEY_SHIFT_LEFT] = SCEMU_BUTTON_KEY_SHIFT_LEFT, [PANEL_BUTTON_KEY_SHIFT_RIGHT] = SCEMU_BUTTON_KEY_SHIFT_RIGHT,
		[PANEL_BUTTON_MIDI_CH_LEFT] = SCEMU_BUTTON_MIDI_CH_LEFT, [PANEL_BUTTON_MIDI_CH_RIGHT] = SCEMU_BUTTON_MIDI_CH_RIGHT,
		[PANEL_BUTTON_USER_INST] = SCEMU_BUTTON_USER_INST, [PANEL_BUTTON_SELECT] = SCEMU_BUTTON_SELECT,
		[PANEL_BUTTON_EDIT1_LEFT] = SCEMU_BUTTON_EDIT1_LEFT, [PANEL_BUTTON_EDIT1_RIGHT] = SCEMU_BUTTON_EDIT1_RIGHT,
		[PANEL_BUTTON_EDIT2_LEFT] = SCEMU_BUTTON_EDIT2_LEFT, [PANEL_BUTTON_EDIT2_RIGHT] = SCEMU_BUTTON_EDIT2_RIGHT,
		[PANEL_BUTTON_EDIT3_LEFT] = SCEMU_BUTTON_EDIT3_LEFT, [PANEL_BUTTON_EDIT3_RIGHT] = SCEMU_BUTTON_EDIT3_RIGHT,
		[PANEL_BUTTON_PREVIEW] = SCEMU_BUTTON_PREVIEW,
	};
	return e >= 0 && e < PANEL_ELEMENT_COUNT ? button[e] : -1;
}
