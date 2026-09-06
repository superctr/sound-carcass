/* scemu GUI: the front panel compositor.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdlib.h>
#include <string.h>
#include "panel.h"
#include "png.h"
#include "lcd_font.h"

/* the baked artwork, embedded by the build: per model, a base image and a sprite atlas per size */
#define ART_DECLARE(m, p) \
	extern const unsigned char panel_##m##_base_p##p##_png[]; \
	extern const size_t panel_##m##_base_p##p##_png_size; \
	extern const unsigned char panel_##m##_atlas_p##p##_png[]; \
	extern const size_t panel_##m##_atlas_p##p##_png_size;
#define ART_MODEL(m) ART_DECLARE(m, 4) ART_DECLARE(m, 8)
ART_MODEL(sc88pro) ART_MODEL(sc88) ART_MODEL(sc88vl) ART_MODEL(sc55mk2)
ART_DECLARE(sc8850, 3) ART_DECLARE(sc8850, 6)

#define ART_ROW(m, p) { p, panel_##m##_base_p##p##_png, panel_##m##_atlas_p##p##_png, \
                        &panel_##m##_base_p##p##_png_size, &panel_##m##_atlas_p##p##_png_size }
static const struct
{
	int pitch;
	const unsigned char *base, *atlas;
	const size_t *base_size, *atlas_size;
} artwork[PANEL_MODEL_COUNT][2] = {
	[PANEL_MODEL_SC88PRO] = { ART_ROW(sc88pro, 4), ART_ROW(sc88pro, 8) },
	[PANEL_MODEL_SC88] = { ART_ROW(sc88, 4), ART_ROW(sc88, 8) },
	[PANEL_MODEL_SC88VL] = { ART_ROW(sc88vl, 4), ART_ROW(sc88vl, 8) },
	[PANEL_MODEL_SC55MK2] = { ART_ROW(sc55mk2, 4), ART_ROW(sc55mk2, 8) },
	[PANEL_MODEL_SC8850] = { ART_ROW(sc8850, 3), ART_ROW(sc8850, 6) },
};

#define SEG_COLOR 0xff201000u
#define GLCD_STRIDE 27
#define GLCD_DOTS_PER_BYTE 6

struct panel
{
	panel_model_t model;
	const panel_size_t *size;
	png_image_t base;
	png_image_t atlas;      /* premultiplied */
	scemu_lcd_t lcd;
	scemu_glcd_t glcd;
	uint32_t leds;
	float knob;
	int dial;
	uint64_t pressed;
	bool standby;
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

panel_model_t panel_model_for(scemu_model_t model)
{
	switch (model)
	{
	case SCEMU_MODEL_SC88: return PANEL_MODEL_SC88;
	case SCEMU_MODEL_SC88VL: return PANEL_MODEL_SC88VL;
	case SCEMU_MODEL_SC8850: return PANEL_MODEL_SC8850;
	default: return PANEL_MODEL_SC88PRO;
	}
}

panel_t *panel_create(panel_model_t model, int pitch)
{
	if (model < 0 || model >= PANEL_MODEL_COUNT)
		return NULL;
	const panel_size_t *size = NULL;
	for (int n = 0; n < PANEL_SIZE_COUNT; n++)
		if (panel_sizes[model][n].pitch == pitch)
			size = &panel_sizes[model][n];
	size_t art = 0;
	while (art < 2 && artwork[model][art].pitch != pitch)
		art++;
	if (!size || art == 2)
		return NULL;

	panel_t *p = calloc(1, sizeof(*p));
	if (!p)
		return NULL;
	p->model = model;
	p->size = size;
	if (!png_decode(artwork[model][art].base, *artwork[model][art].base_size, &p->base)
	    || !png_decode(artwork[model][art].atlas, *artwork[model][art].atlas_size, &p->atlas)
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

panel_model_t panel_model(const panel_t *p) { return p->model; }
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

void panel_set_glcd(panel_t *p, const scemu_glcd_t *glcd)
{
	if (memcmp(p->glcd.bitmap, glcd->bitmap, sizeof(glcd->bitmap)) == 0 && p->glcd.display_on == glcd->display_on)
		return;
	memcpy(p->glcd.bitmap, glcd->bitmap, sizeof(glcd->bitmap));
	p->glcd.display_on = glcd->display_on;
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

void panel_set_dial(panel_t *p, int steps)
{
	int at = (p->dial + steps) % PANEL_DIAL_FRAMES;
	if (at < 0)
		at += PANEL_DIAL_FRAMES;
	if (p->dial != at)
	{
		p->dial = at;
		p->dirty = true;
	}
}

void panel_set_standby(panel_t *p, bool standby)
{
	if (p->standby != standby)
	{
		p->standby = standby;
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

static uint32_t dim(uint32_t c)
{
	return (c & 0xff000000u) | (((c >> 16) & 0xff) * 5 / 8) << 16 | (((c >> 8) & 0xff) * 5 / 8) << 8 | ((c & 0xff) * 5 / 8);
}

static void blit(panel_t *p, uint32_t *pixels, size_t stride, const panel_sprite_t *s, bool dimmed)
{
	/* a sprite the model lacks is empty */
	for (int j = 0; j < s->atlas.h; j++)
	{
		const uint32_t *src = p->atlas.pixels + (size_t)(s->atlas.y + j) * p->atlas.width + s->atlas.x;
		uint32_t *dst = pixels + (size_t)(s->y + j) * stride + s->x;
		for (int i = 0; i < s->atlas.w; i++)
			dst[i] = over(dimmed ? dim(src[i]) : src[i], dst[i]);
	}
}

static void blit_sprite(panel_t *p, uint32_t *pixels, size_t stride, panel_sprite_id_t id)
{
	blit(p, pixels, stride, &p->size->sprite[id], false);
}

static void draw_knob(panel_t *p, uint32_t *pixels, size_t stride)
{
	int frame = (int)(p->knob * (PANEL_KNOB_FRAMES - 1) + 0.5f);
	blit(p, pixels, stride, &p->size->knob[frame], (p->pressed >> PANEL_BUTTON_PREVIEW) & 1);
	blit(p, pixels, stride, &p->size->dial[p->dial], (p->pressed >> PANEL_BUTTON_VALUE) & 1);
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

static void draw_glcd(panel_t *p, uint32_t *pixels, size_t stride)
{
	const panel_glass_t *g = &p->size->glass;
	if (!p->glcd.display_on)
		return;
	for (int row = 0; row < g->dot_rows; row++)
	{
		const uint8_t *line = p->glcd.bitmap + (size_t)row * GLCD_STRIDE;
		for (int col = 0; col < g->dot_cols; col++)
			if (line[col / GLCD_DOTS_PER_BYTE] & (0x80 >> (col % GLCD_DOTS_PER_BYTE)))
				fill(pixels, stride, g->dot_x + col * g->dot_pitch, g->dot_y + row * g->dot_pitch,
				     g->dot, g->dot, SEG_COLOR);
	}
}

static const panel_sprite_id_t led_sprite[SCEMU_LED_COUNT] = {
	PANEL_SPRITE_LED_ALL, PANEL_SPRITE_LED_MUTE, PANEL_SPRITE_LED_SC55_MAP, PANEL_SPRITE_LED_SC88_MAP,
	PANEL_SPRITE_LED_EDIT1, PANEL_SPRITE_LED_EDIT2, PANEL_SPRITE_LED_EDIT3,
	PANEL_SPRITE_LED_USER_INST, PANEL_SPRITE_LED_USER_INST_RED,
	PANEL_SPRITE_LED_SOLO, PANEL_SPRITE_LED_EDIT, PANEL_SPRITE_LED_DRUM, PANEL_SPRITE_LED_EFFECTS,
};

void panel_render(panel_t *p, uint32_t *pixels, size_t stride)
{
	for (int y = 0; y < p->base.height; y++)
		memcpy(pixels + (size_t)y * stride, p->base.pixels + (size_t)y * p->base.width, (size_t)p->base.width * sizeof(uint32_t));
	if (p->size->glass.dot_cols)
		draw_glcd(p, pixels, stride);
	else
		draw_glass(p, pixels, stride);
	uint32_t both = (1u << SCEMU_LED_USER_INST) | (1u << SCEMU_LED_USER_INST_RED);
	for (int n = 0; n < SCEMU_LED_COUNT; n++)
		if ((p->leds & (1u << n)) && ((p->leds & both) != both || !((1u << n) & both)))
			blit_sprite(p, pixels, stride, led_sprite[n]);
	if ((p->leds & both) == both)
		blit_sprite(p, pixels, stride, PANEL_SPRITE_LED_USER_INST_EFX);
	if (p->standby)
		blit_sprite(p, pixels, stride, PANEL_SPRITE_LED_STANDBY);
	for (int e = 0; e < PANEL_ELEMENT_COUNT; e++)
		if ((p->pressed & ((uint64_t)1 << e)) && panel_element_sprite[e] >= 0)
			blit(p, pixels, stride, &p->size->sprite[panel_element_sprite[e]], true);
	draw_knob(p, pixels, stride);
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
		[PANEL_LCD_GLASS] = -1, [PANEL_LENS_USER_INST] = -1, [PANEL_LOGO] = -1, [PANEL_LOGO_MODEL] = -1,
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
		[PANEL_BUTTON_PREVIEW] = SCEMU_BUTTON_PREVIEW, [PANEL_DIAL_VALUE] = -1,
		[PANEL_BUTTON_F1] = SCEMU_BUTTON_F1, [PANEL_BUTTON_F2] = SCEMU_BUTTON_F2,
		[PANEL_BUTTON_F3] = SCEMU_BUTTON_F3, [PANEL_BUTTON_F4] = SCEMU_BUTTON_F4,
		[PANEL_BUTTON_MAP] = SCEMU_BUTTON_MAP, [PANEL_BUTTON_VALUE] = SCEMU_BUTTON_VALUE,
		[PANEL_BUTTON_EDIT] = SCEMU_BUTTON_EDIT, [PANEL_BUTTON_DRUM] = SCEMU_BUTTON_DRUM,
		[PANEL_BUTTON_EFFECTS] = SCEMU_BUTTON_EFFECTS, [PANEL_BUTTON_SHIFT] = SCEMU_BUTTON_SHIFT,
		[PANEL_BUTTON_DOWN] = SCEMU_BUTTON_DOWN, [PANEL_BUTTON_UP] = SCEMU_BUTTON_UP,
		[PANEL_BUTTON_EXIT] = SCEMU_BUTTON_EXIT, [PANEL_BUTTON_ENTER] = SCEMU_BUTTON_ENTER,
		[PANEL_BUTTON_SOLO] = SCEMU_BUTTON_SOLO, [PANEL_BUTTON_DEC] = SCEMU_BUTTON_DEC,
		[PANEL_BUTTON_INC] = SCEMU_BUTTON_INC,
	};
	return e >= 0 && e < PANEL_ELEMENT_COUNT ? button[e] : -1;
}
