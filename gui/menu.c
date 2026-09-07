/* scemu GUI: the combination menu drawn on the panel.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdio.h>
#include <string.h>
#include "menu.h"

#define BACK 0xff202020
#define EDGE 0xff606060
#define HOVER 0xff3a3a3a
#define NAME 0xffe8e8e8
#define MODE 0xffb0b0b0
#define KEYS 0xff909090

static int cell_w(const menu_t *m) { return TEXT_CELL_W * m->scale; }
static int cell_h(const menu_t *m) { return TEXT_CELL_H * m->scale; }

static void row_texts(const menu_t *m, int row, char *line1, size_t size1, char *line2, size_t size2)
{
	const combo_t *c = &combos[m->ids[row]];
	snprintf(line1, size1, "%s  (%s)", c->name, c->mode ? c->mode : "any mode");
	combo_text(c, line2, size2);
}

bool menu_open(menu_t *m, panel_element_t e, double x, double y, int panel_w, int panel_h, int scale)
{
	memset(m, 0, sizeof *m);
	m->hover = -1;
	int button = panel_element_button(e);
	if (button < 0)
		return false;
	int ids[MENU_ROWS];
	int count = combos_for((scemu_button_t)button, ids, MENU_ROWS);
	/* holding one half and pressing the other is a pointer gesture here, not a menu entry */
	for (int n = 0; n < count; n++)
		if (strcmp(combos[ids[n]].name, "the value will change faster") != 0)
			m->ids[m->count++] = ids[n];
	if (!m->count)
		return false;
	m->scale = scale < 1 ? 1 : scale;
	m->pad = cell_w(m);
	m->row_h = 2 * cell_h(m) + cell_h(m) / 2;
	int width = 0;
	for (int n = 0; n < m->count; n++)
	{
		char a[256], b[256];
		row_texts(m, n, a, sizeof a, b, sizeof b);
		int wa = text_width(m->scale, a), wb = text_width(m->scale, b);
		width = wa > width ? wa : width;
		width = wb > width ? wb : width;
	}
	m->w = width + 2 * m->pad;
	if (m->w > panel_w)
		m->w = panel_w;
	m->visible = (panel_h - 2 * m->pad) / m->row_h;
	if (m->visible > m->count)
		m->visible = m->count;
	if (m->visible < 1)
		m->visible = 1;
	m->h = m->visible * m->row_h + 2 * m->pad;
	m->x = (int)x;
	m->y = (int)y;
	if (m->x + m->w > panel_w)
		m->x = panel_w - m->w;
	if (m->y + m->h > panel_h)
		m->y = panel_h - m->h;
	if (m->x < 0)
		m->x = 0;
	if (m->y < 0)
		m->y = 0;
	m->open = true;
	return true;
}

void menu_close(menu_t *m)
{
	m->open = false;
	m->hover = -1;
}

bool menu_contains(const menu_t *m, double x, double y)
{
	return m->open && x >= m->x && x < m->x + m->w && y >= m->y && y < m->y + m->h;
}

int menu_row_at(const menu_t *m, double x, double y)
{
	if (!menu_contains(m, x, y))
		return -1;
	int r = ((int)y - m->y - m->pad) / m->row_h;
	if (r < 0 || r >= m->visible || (int)y < m->y + m->pad)
		return -1;
	return m->first + r;
}

const combo_t *menu_combo(const menu_t *m, int row)
{
	return row >= 0 && row < m->count ? &combos[m->ids[row]] : NULL;
}

bool menu_motion(menu_t *m, double x, double y)
{
	int row = menu_row_at(m, x, y);
	if (row == m->hover)
		return false;
	m->hover = row;
	return true;
}

bool menu_scroll(menu_t *m, int rows)
{
	int first = m->first + rows;
	if (first > m->count - m->visible)
		first = m->count - m->visible;
	if (first < 0)
		first = 0;
	if (first == m->first)
		return false;
	m->first = first;
	return true;
}

bool menu_row_rect(const menu_t *m, int row, panel_rect_t *out)
{
	if (!m->open || row < m->first || row >= m->first + m->visible)
		return false;
	out->x = (int16_t)m->x;
	out->y = (int16_t)(m->y + m->pad + (row - m->first) * m->row_h);
	out->w = (int16_t)m->w;
	out->h = (int16_t)m->row_h;
	return true;
}

void menu_draw(const menu_t *m, const text_frame_t *f)
{
	if (!m->open)
		return;
	text_fill(f, m->x, m->y, m->w, m->h, EDGE);
	text_fill(f, m->x + 1, m->y + 1, m->w - 2, m->h - 2, BACK);
	for (int r = 0; r < m->visible; r++)
	{
		int row = m->first + r;
		int y = m->y + m->pad + r * m->row_h;
		if (row == m->hover)
			text_fill(f, m->x + 1, y, m->w - 2, m->row_h, HOVER);
		char a[256], b[256];
		row_texts(m, row, a, sizeof a, b, sizeof b);
		const combo_t *c = &combos[m->ids[row]];
		int name_w = text_width(m->scale, c->name);
		text_draw(f, m->x + m->pad, y + cell_h(m) / 4, m->scale, NAME, c->name);
		text_draw(f, m->x + m->pad + name_w, y + cell_h(m) / 4, m->scale, MODE, a + strlen(c->name));
		text_draw(f, m->x + m->pad, y + cell_h(m) / 4 + cell_h(m), m->scale, KEYS, b);
	}
	/* more rows above or below: a mark in the corner */
	if (m->first > 0)
		text_draw(f, m->x + m->w - 2 * cell_w(m), m->y + 1, m->scale, MODE, "^");
	if (m->first + m->visible < m->count)
		text_draw(f, m->x + m->w - 2 * cell_w(m), m->y + m->h - cell_h(m), m->scale, MODE, "v");
}
