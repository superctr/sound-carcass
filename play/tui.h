/* scplay: the text-mode front panel.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCPLAY_TUI_H
#define SCPLAY_TUI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "scemu.h"

enum
{
	TUI_KEY_NONE = 0,
	TUI_KEY_UP = 256,
	TUI_KEY_DOWN,
	TUI_KEY_LEFT,
	TUI_KEY_RIGHT,
	TUI_KEY_ESC
};

typedef struct tui tui_t;

typedef struct tui_state
{
	const char *model;
	const char *song;
	const char *title;   /* the SMF's own track name, if it has one */
	const scemu_lcd_t *lcd;      /* the character glass, NULL on a model without one */
	const scemu_glcd_t *glcd;    /* the bitmap glass, NULL on a model without one */
	uint32_t leds;
	bool has_efx_led;
	bool eq_label;               /* the SC-88 calls the SC-88 map button EQ */
	bool standby_lamp;           /* the SC-55mkII's three: ALL, MUTE and STANDBY */
	double elapsed, total;
	bool paused;
	bool show_keys;
	const char *status;
	uint32_t underruns;
	double speed;
} tui_state_t;

tui_t *tui_open(void);
void tui_close(tui_t *t);
int tui_key(tui_t *t);
void tui_draw(tui_t *t, const tui_state_t *st);

#endif
