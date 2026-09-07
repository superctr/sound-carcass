/* scemu plugin: the front panel in a host's window.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pugl/pugl.h>
#include <pugl/gl.h>
#include "window.h"
#include "menu.h"
#include "text.h"

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#define MESSAGE_MAX 512

struct window
{
	PuglWorld *world;
	PuglView *view;
	const window_actions_t *act;
	void *user;
	panel_model_t model;
	panel_t *panel;
	controls_t ctl;
	controls_actions_t inner;   /* the controls call us; we pass the machine's on and keep the menu */
	menu_t menu;
	double macro_due;           /* puglGetTime at which a played combination is over, or 0 */
	uint32_t *frame;            /* the panel's pixels, panel_width x panel_height */
	int base_w, base_h;         /* the small bake's size: the panel's shape */
	bool big;                   /* the large bake is the one drawn */
	double scale;
	uint32_t width, height;     /* the view's size */
	bool realized, visible, texture, upload;
	GLuint tex;
	char message[MESSAGE_MAX];
	bool redraw;
};

/* ---------------------------------------------------------------- the frame */

static bool use_bake(window_t *w, bool big)
{
	int pitch = panel_sizes[w->model][big ? 1 : 0].pitch;
	panel_t *p = panel_create(w->model, pitch);
	if (!p)
		return false;
	uint32_t *frame = calloc((size_t)panel_width(p) * panel_height(p), sizeof(uint32_t));
	if (!frame)
	{
		panel_destroy(p);
		return false;
	}
	if (w->panel)
	{
		/* the new glass shows what the old one did */
		controls_set_panel(&w->ctl, p);
		panel_destroy(w->panel);
	}
	free(w->frame);
	w->panel = p;
	w->frame = frame;
	w->big = big;
	w->upload = true;
	return true;
}

static text_frame_t frame_of(const window_t *w)
{
	text_frame_t f = { w->frame, (size_t)panel_width(w->panel), panel_width(w->panel), panel_height(w->panel) };
	return f;
}

static int text_scale(const window_t *w)
{
	int s = panel_pitch(w->panel) / 2;
	return s < 2 ? 2 : s;
}

/* the message in the middle of the panel, wrapped by words to its width */
static void draw_message(window_t *w)
{
	text_frame_t f = frame_of(w);
	int s = text_scale(w);
	int cw = TEXT_CELL_W * s, ch = 10 * s, margin = 4 * cw;
	int columns = (f.width - 2 * margin) / cw;
	if (columns < 8)
		return;
	char lines[16][128];
	int count = 0, width = 0;
	const char *p = w->message;
	while (*p && count < 16)
	{
		int len = 0;
		while (*p == ' ')
			p++;
		while (*p && *p != '\n')
		{
			const char *word = p;
			while (*p && *p != ' ' && *p != '\n')
				p++;
			int wl = (int)(p - word);
			if (len && len + 1 + wl > columns)
			{
				p = word;
				break;
			}
			if (wl > columns - len)
				wl = columns - len;
			if (len)
				lines[count][len++] = ' ';
			memcpy(lines[count] + len, word, (size_t)wl);
			len += wl;
			while (*p == ' ')
				p++;
		}
		if (*p == '\n')
			p++;
		lines[count][len] = 0;
		if (len > width)
			width = len;
		count++;
	}
	int box_w = width * cw + 2 * cw, box_h = count * ch + ch;
	int x0 = (f.width - box_w) / 2, y0 = (f.height - box_h) / 2;
	text_fill(&f, x0, y0, box_w, box_h, 0xff202020);
	for (int l = 0; l < count; l++)
		text_draw(&f, x0 + cw, y0 + ch / 2 + l * ch, s, 0xffe8e8e8, lines[l]);
}

static void render(window_t *w)
{
	panel_render(w->panel, w->frame, (size_t)panel_width(w->panel));
	if (w->message[0])
		draw_message(w);
	text_frame_t f = frame_of(w);
	menu_draw(&w->menu, &f);
	w->redraw = false;
}

/* ---------------------------------------------------------------- GL */

static void upload(window_t *w)
{
	if (!w->texture)
	{
		glGenTextures(1, &w->tex);
		glBindTexture(GL_TEXTURE_2D, w->tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		w->texture = true;
		w->upload = true;
	}
	glBindTexture(GL_TEXTURE_2D, w->tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	int fw = panel_width(w->panel), fh = panel_height(w->panel);
	if (w->upload)
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, w->frame);
	else
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, fh, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, w->frame);
	w->upload = false;
}

static void draw(window_t *w)
{
	glViewport(0, 0, (GLsizei)w->width, (GLsizei)w->height);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, 1, 1, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, w->tex);
	glColor4f(1, 1, 1, 1);
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0); glVertex2f(0, 0);
	glTexCoord2f(1, 0); glVertex2f(1, 0);
	glTexCoord2f(1, 1); glVertex2f(1, 1);
	glTexCoord2f(0, 1); glVertex2f(0, 1);
	glEnd();
}

/* ---------------------------------------------------------------- the menu */

static void menu_dismiss(window_t *w)
{
	if (!w->menu.open)
		return;
	menu_close(&w->menu);
	controls_highlight_clear(&w->ctl);
	w->redraw = true;
}

/* a combination is playing: the keys shown pressed come up when it is over */
static void macro_started(window_t *w, unsigned ms)
{
	if (ms)
		w->macro_due = puglGetTime(w->world) + ms / 1000.0;
}

static void menu_choose(window_t *w, int row)
{
	const combo_t *c = menu_combo(&w->menu, row);
	menu_dismiss(w);
	if (c)
		macro_started(w, controls_play_combo(&w->ctl, c));
}

static void menu_hover(window_t *w, double x, double y)
{
	int before = w->menu.hover;
	if (!menu_motion(&w->menu, x, y))
		return;
	if (before >= 0)
		controls_highlight(&w->ctl, menu_combo(&w->menu, before), false);
	if (w->menu.hover >= 0)
		controls_highlight(&w->ctl, menu_combo(&w->menu, w->menu.hover), true);
	w->redraw = true;
}

/* the controls' actions: the machine's go on to the plugin, the menu is ours */
static void inner_key(void *user, scemu_button_t b, bool down)
{
	window_t *w = user;
	w->act->controls.key(w->user, b, down);
}

static void inner_key_after(void *user, scemu_button_t b, bool down, unsigned ms)
{
	window_t *w = user;
	w->act->controls.key_after(w->user, b, down, ms);
}

static void inner_dial(void *user, int steps)
{
	window_t *w = user;
	w->act->controls.dial(w->user, steps);
}

static void inner_power(void *user, bool on)
{
	window_t *w = user;
	w->act->controls.power(w->user, on);
}

static void inner_knob(void *user, float turn)
{
	window_t *w = user;
	w->act->controls.knob(w->user, turn);
}

static void inner_element(void *user, panel_element_t e, double x, double y)
{
	window_t *w = user;
	if (w->act->controls.element)
		w->act->controls.element(w->user, e, x, y);
}

static void inner_combo_menu(void *user, panel_element_t e, double x, double y)
{
	window_t *w = user;
	menu_dismiss(w);
	if (menu_open(&w->menu, e, x, y, panel_width(w->panel), panel_height(w->panel), text_scale(w)))
		w->redraw = true;
}

/* ---------------------------------------------------------------- events */

static unsigned mods_of(PuglMods state)
{
	return ((state & PUGL_MOD_SHIFT) ? CONTROLS_SHIFT : 0) | ((state & PUGL_MOD_CTRL) ? CONTROLS_CONTROL : 0);
}

/* PUGL numbers the buttons left, right, middle from 0 */
static int button_of(uint32_t b)
{
	return b == 0 ? CONTROLS_BUTTON_LEFT : b == 1 ? CONTROLS_BUTTON_RIGHT : b == 2 ? CONTROLS_BUTTON_MIDDLE : 0;
}

static double to_panel_x(const window_t *w, double x) { return x * panel_width(w->panel) / (w->width ? w->width : 1); }
static double to_panel_y(const window_t *w, double y) { return y * panel_height(w->panel) / (w->height ? w->height : 1); }

static PuglStatus on_event(PuglView *view, const PuglEvent *e)
{
	window_t *w = puglGetHandle(view);
	switch (e->type)
	{
	case PUGL_REALIZE:
		w->realized = true;
		w->texture = false;
		break;
	case PUGL_UNREALIZE:
		if (w->texture)
			glDeleteTextures(1, &w->tex);
		w->texture = false;
		w->realized = false;
		break;
	case PUGL_CONFIGURE:
		w->width = e->configure.width;
		w->height = e->configure.height;
		{
			bool big = w->height >= (uint32_t)(w->base_h * 3 / 2);
			if (big != w->big)
				use_bake(w, big);
		}
		break;
	case PUGL_EXPOSE:
		if (panel_dirty(w->panel) || w->redraw || w->upload || !w->texture)
		{
			if (panel_dirty(w->panel) || w->redraw)
				render(w);
			upload(w);
		}
		draw(w);
		break;
	case PUGL_BUTTON_PRESS:
		if (w->menu.open)
		{
			double x = to_panel_x(w, e->button.x), y = to_panel_y(w, e->button.y);
			int row = menu_row_at(&w->menu, x, y);
			if (row >= 0 && button_of(e->button.button) == CONTROLS_BUTTON_LEFT)
				menu_choose(w, row);
			else if (!menu_contains(&w->menu, x, y) || button_of(e->button.button) != CONTROLS_BUTTON_LEFT)
				menu_dismiss(w);
		}
		else if (button_of(e->button.button))
			controls_press(&w->ctl, button_of(e->button.button), mods_of(e->button.state),
			               to_panel_x(w, e->button.x), to_panel_y(w, e->button.y));
		break;
	case PUGL_BUTTON_RELEASE:
		if (!w->menu.open && button_of(e->button.button))
			controls_release(&w->ctl, button_of(e->button.button));
		break;
	case PUGL_MOTION:
		if (w->menu.open)
			menu_hover(w, to_panel_x(w, e->motion.x), to_panel_y(w, e->motion.y));
		else
			controls_motion(&w->ctl, to_panel_x(w, e->motion.x), to_panel_y(w, e->motion.y));
		break;
	case PUGL_SCROLL:
		/* PUGL counts up as positive; the controls take the toolkits' down */
		if (w->menu.open)
		{
			if (menu_scroll(&w->menu, e->scroll.dy < 0 ? 1 : e->scroll.dy > 0 ? -1 : 0))
			{
				menu_hover(w, to_panel_x(w, e->scroll.x), to_panel_y(w, e->scroll.y));
				w->redraw = true;
			}
		}
		else
			controls_scroll(&w->ctl, to_panel_x(w, e->scroll.x), to_panel_y(w, e->scroll.y), -e->scroll.dy);
		break;
	case PUGL_KEY_PRESS:
		if (e->key.key == PUGL_KEY_ESCAPE)
			menu_dismiss(w);
		break;
	case PUGL_CLOSE:
		if (w->act->closed)
			w->act->closed(w->user);
		break;
	default:
		break;
	}
	return PUGL_SUCCESS;
}

/* ---------------------------------------------------------------- the window */

const char *window_api(void)
{
#ifdef _WIN32
	return "win32";
#else
	return "x11";
#endif
}

static void default_size(const window_t *w, uint32_t *width, uint32_t *height)
{
	*width = (uint32_t)(w->base_w * w->scale + 0.5);
	*height = (uint32_t)(w->base_h * w->scale + 0.5);
}

window_t *window_create(scemu_model_t model, double scale, const window_actions_t *act, void *user,
                        char *err, size_t err_size)
{
	window_t *w = calloc(1, sizeof *w);
	if (!w)
		return NULL;
	w->act = act;
	w->user = user;
	w->model = panel_model_for(model);
	w->scale = scale > 0 ? scale : 1;
	w->base_w = panel_sizes[w->model][0].width;
	w->base_h = panel_sizes[w->model][0].height;
	if (!use_bake(w, w->scale >= 2))
	{
		snprintf(err, err_size, "the panel artwork does not decode");
		window_destroy(w);
		return NULL;
	}
	w->inner = (controls_actions_t){ inner_key, inner_key_after, inner_dial, inner_power, inner_knob, inner_element,
	                                 inner_combo_menu };
	controls_init(&w->ctl, w->panel, &w->inner, w);
	w->menu.hover = -1;
	controls_set_soft_power(&w->ctl, model == SCEMU_MODEL_SC55MK2);
	w->ctl.power = false;
	panel_set_standby(w->panel, true);

	w->world = puglNewWorld(PUGL_MODULE, 0);
	if (!w->world)
	{
		snprintf(err, err_size, "no display");
		window_destroy(w);
		return NULL;
	}
	puglSetWorldString(w->world, PUGL_CLASS_NAME, "scemu");
	w->view = puglNewView(w->world);
	if (!w->view)
	{
		snprintf(err, err_size, "no view");
		window_destroy(w);
		return NULL;
	}
	puglSetHandle(w->view, w);
	puglSetEventFunc(w->view, on_event);
	puglSetBackend(w->view, puglGlBackend());
	puglSetViewHint(w->view, PUGL_CONTEXT_VERSION_MAJOR, 2);
	puglSetViewHint(w->view, PUGL_CONTEXT_VERSION_MINOR, 0);
	puglSetViewHint(w->view, PUGL_DOUBLE_BUFFER, PUGL_TRUE);
	puglSetViewHint(w->view, PUGL_SWAP_INTERVAL, 0);
	puglSetViewHint(w->view, PUGL_RESIZABLE, PUGL_TRUE);
	puglSetViewHint(w->view, PUGL_VIEW_TYPE, PUGL_VIEW_TYPE_NORMAL);
	puglSetViewString(w->view, PUGL_WINDOW_TITLE, "scemu");
	default_size(w, &w->width, &w->height);
	puglSetSizeHint(w->view, PUGL_DEFAULT_SIZE, (PuglSpan)w->width, (PuglSpan)w->height);
	puglSetSizeHint(w->view, PUGL_MIN_SIZE, (PuglSpan)(w->base_w / 2), (PuglSpan)(w->base_h / 2));
	puglSetSizeHint(w->view, PUGL_FIXED_ASPECT, (PuglSpan)w->base_w, (PuglSpan)w->base_h);
	w->redraw = true;
	return w;
}

void window_destroy(window_t *w)
{
	if (!w)
		return;
	if (w->view)
	{
		if (w->realized)
			puglUnrealize(w->view);
		puglFreeView(w->view);
	}
	if (w->world)
		puglFreeWorld(w->world);
	if (w->panel)
		panel_destroy(w->panel);
	free(w->frame);
	free(w);
}

bool window_set_parent(window_t *w, uintptr_t native)
{
	return puglSetParent(w->view, (PuglNativeView)native) == PUGL_SUCCESS;
}

bool window_set_transient(window_t *w, uintptr_t native)
{
	return puglSetTransientParent(w->view, (PuglNativeView)native) == PUGL_SUCCESS;
}

void window_set_title(window_t *w, const char *title)
{
	puglSetViewString(w->view, PUGL_WINDOW_TITLE, title);
}

bool window_set_scale(window_t *w, double scale)
{
	if (scale <= 0)
		return false;
	w->scale = scale;
	uint32_t width, height;
	default_size(w, &width, &height);
	return window_set_size(w, width, height);
}

void window_size(const window_t *w, uint32_t *width, uint32_t *height)
{
	*width = w->width;
	*height = w->height;
}

void window_fit(const window_t *w, uint32_t *width, uint32_t *height)
{
	double by_w = (double)*width / w->base_w, by_h = (double)*height / w->base_h;
	double s = by_w < by_h ? by_w : by_h;
	if (s < 0.5)
		s = 0.5;
	*width = (uint32_t)(w->base_w * s + 0.5);
	*height = (uint32_t)(w->base_h * s + 0.5);
}

bool window_set_size(window_t *w, uint32_t width, uint32_t height)
{
	if (!width || !height)
		return false;
	if (w->realized)
		return puglSetSizeHint(w->view, PUGL_CURRENT_SIZE, (PuglSpan)width, (PuglSpan)height) == PUGL_SUCCESS;
	w->width = width;
	w->height = height;
	puglSetSizeHint(w->view, PUGL_DEFAULT_SIZE, (PuglSpan)width, (PuglSpan)height);
	return true;
}

bool window_show(window_t *w)
{
	if (!w->realized && puglRealize(w->view) != PUGL_SUCCESS)
		return false;
	if (puglShow(w->view, PUGL_SHOW_RAISE) != PUGL_SUCCESS)
		return false;
	w->visible = true;
	return true;
}

void window_hide(window_t *w)
{
	if (w->realized)
		puglHide(w->view);
	w->visible = false;
}

void window_set_panel(window_t *w, const unit_panel_t *state)
{
	if (state->has_glcd)
		panel_set_glcd(w->panel, &state->glcd);
	else
		panel_set_lcd(w->panel, &state->lcd);
	panel_set_leds(w->panel, state->leds);
	w->ctl.power = state->power;
	panel_set_standby(w->panel, !state->power);
}

void window_set_knob(window_t *w, float turn)
{
	controls_set_knob(&w->ctl, turn);
}

void window_set_message(window_t *w, const char *text)
{
	snprintf(w->message, sizeof w->message, "%s", text ? text : "");
	w->redraw = true;
}

void window_boot_done(window_t *w)
{
	macro_started(w, controls_boot_done(&w->ctl));
}

void window_tick(window_t *w)
{
	if (w->macro_due && puglGetTime(w->world) >= w->macro_due)
	{
		w->macro_due = 0;
		controls_macro_done(&w->ctl);
	}
	if (w->realized && w->visible && (panel_dirty(w->panel) || w->redraw))
		puglObscureView(w->view);
	puglUpdate(w->world, 0);
}
