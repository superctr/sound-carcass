/* scemu GUI: the combination menu drawn on the panel.
 *
 * What the desktop player shows as a popover when a key is clicked with
 * the middle button: the combinations from the manual the key takes part
 * in, one row each with the name, the mode and the keys.  Laid out and
 * drawn into the panel's frame, so a window with no toolkit has it; the
 * host feeds it pointer positions in panel pixels.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCEMU_MENU_H
#define SCEMU_MENU_H

#include <stdbool.h>
#include "combos.h"
#include "panel.h"
#include "text.h"

#define MENU_ROWS 64

typedef struct menu
{
	bool open;
	int ids[MENU_ROWS];       /* indexes into combos */
	int count;
	int scale;                /* the font's */
	int x, y, w, h;           /* the box, in panel pixels */
	int row_h, pad;
	int first;                /* the first row shown, when they do not all fit */
	int visible;
	int hover;                /* the row under the pointer, or -1 */
} menu_t;

/* Opens the menu for a key of a model's panel at a point on a panel of the
 * given size; false, and closed, when the key has no combinations to offer. */
bool menu_open(menu_t *m, panel_model_t panel, panel_element_t e, double x, double y,
               int panel_w, int panel_h, int scale);
void menu_close(menu_t *m);
bool menu_contains(const menu_t *m, double x, double y);
/* the row at a point, or -1 */
int menu_row_at(const menu_t *m, double x, double y);
const combo_t *menu_combo(const menu_t *m, int row);
/* the pointer moved: true when the hovered row changed */
bool menu_motion(menu_t *m, double x, double y);
/* by rows, positive down; true when the view moved */
bool menu_scroll(menu_t *m, int rows);
/* where a row is drawn, for a host that wants to point at it; false when it is scrolled out */
bool menu_row_rect(const menu_t *m, int row, panel_rect_t *out);
void menu_draw(const menu_t *m, const text_frame_t *f);

#endif
