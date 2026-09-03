/* scplay: the text-mode front panel.  The glass is drawn from the LCD
 * controller's memory: the text fields as text, the sixteen level bars from the
 * CGRAM patterns the firmware fills, two segments to a character cell.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "tui.h"

#define FRAME_MAX 16384
#define IN_MAX 64

#define LIT   "\033[38;5;214m"
#define DIM   "\033[38;5;94m"
#define LABEL "\033[38;5;101m"
#define PLAIN "\033[38;5;250m"
#define OFF   "\033[0m"

struct tui
{
	struct termios saved;
	bool raw;
	char frame[FRAME_MAX];
	char prev[FRAME_MAX];
	size_t len;
	char in[IN_MAX];
	size_t in_len;
};

static tui_t *g_tui;

static void tui_restore(void)
{
	tui_t *t = g_tui;
	if (!t)
		return;
	g_tui = NULL;
	if (t->raw)
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &t->saved);
	fputs("\033[0m\033[?25h\033[?1049l", stdout);
	fflush(stdout);
}

static void raw_off(tui_t *t)
{
	if (!t->raw)
		return;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &t->saved);
	t->raw = false;
}

tui_t *tui_open(void)
{
	if (!isatty(STDOUT_FILENO))
		return NULL;

	tui_t *t = calloc(1, sizeof(*t));
	if (!t)
		return NULL;

	if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &t->saved) == 0)
	{
		struct termios raw = t->saved;
		raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
		raw.c_cc[VMIN] = 0;
		raw.c_cc[VTIME] = 0;
		if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0)
			t->raw = true;
	}
	g_tui = t;
	atexit(tui_restore);
	fputs("\033[?1049h\033[?25l\033[2J", stdout);
	fflush(stdout);
	return t;
}

void tui_close(tui_t *t)
{
	if (!t)
		return;
	if (g_tui == t)
		tui_restore();
	else
		raw_off(t);
	t->raw = false;
	free(t);
}

int tui_key(tui_t *t)
{
	if (!t || !t->raw)
		return TUI_KEY_NONE;
	if (t->in_len == 0)
	{
		ssize_t got = read(STDIN_FILENO, t->in, sizeof(t->in));
		if (got <= 0)
			return TUI_KEY_NONE;
		t->in_len = (size_t)got;
	}

	int key;
	size_t used = 1;
	if (t->in[0] == '\033')
	{
		if (t->in_len >= 3 && t->in[1] == '[')
		{
			used = 3;
			switch (t->in[2])
			{
			case 'A': key = TUI_KEY_UP; break;
			case 'B': key = TUI_KEY_DOWN; break;
			case 'C': key = TUI_KEY_RIGHT; break;
			case 'D': key = TUI_KEY_LEFT; break;
			default:  key = TUI_KEY_NONE; break;
			}
		}
		else
			key = TUI_KEY_ESC;
	}
	else
		key = (unsigned char)t->in[0];

	if (used >= t->in_len)
		t->in_len = 0;
	else
	{
		memmove(t->in, t->in + used, t->in_len - used);
		t->in_len -= used;
	}
	return key;
}

/* ---------------------------------------------------------------- glass */

static const char *glyph(uint8_t c)
{
	static const char *ascii[0x60] = { 0 };
	static char cells[0x60][2];
	if (c < 0x10)
		return "▒";
	if (c < 0x20)
		return " ";
	if (c < 0x80)
	{
		int n = c - 0x20;
		if (!ascii[n])
		{
			cells[n][0] = (char)c;
			cells[n][1] = 0;
			ascii[n] = cells[n];
		}
		if (c == 0x7e)
			return "→";
		if (c == 0x7f)
			return "←";
		return ascii[n];
	}
	if (c == 0xdf)
		return "°";
	if (c == 0xe4)
		return "µ";
	if (c == 0xff)
		return "█";
	return "·";
}

/* Cells 20-23 of both lines carry the sixteen bars: column x of cell 20 + k is
 * bar 5k + x, pattern row y of line 0 is segment y and of line 1 segment 8 + y,
 * counted from the top. */
static void read_bars(const scemu_lcd_t *lcd, uint16_t bar[16])
{
	memset(bar, 0, 16 * sizeof(uint16_t));
	if (!lcd->display_on)
		return;
	for (int line = 0; line < 2; line++)
		for (int k = 0; k < 4; k++)
		{
			uint8_t code = lcd->ddram[line * 40 + 20 + k] & 7;
			const uint8_t *pattern = lcd->cgram + code * 8;
			for (int x = 0; x < 5; x++)
			{
				int b = k * 5 + x;
				if (b >= 16)
					break;
				for (int y = 0; y < 8; y++)
					if (pattern[y] & (1u << (4 - x)))
						bar[b] |= (uint16_t)(1u << (line * 8 + y));
			}
		}
}

static bool read_lr(const scemu_lcd_t *lcd)
{
	if (!lcd->display_on)
		return false;
	uint8_t code = lcd->ddram[40 + 18];
	if (code >= 0x10)
		return false;
	return (lcd->cgram[(code & 7) * 8] & 0x01) != 0;
}

static void field(char *out, size_t size, const scemu_lcd_t *lcd, int start, int count)
{
	size_t used = 0;
	out[0] = 0;
	for (int n = 0; n < count; n++)
	{
		const char *g = lcd->display_on ? glyph(lcd->ddram[start + n]) : " ";
		size_t len = strlen(g);
		if (used + len + 1 >= size)
			break;
		memcpy(out + used, g, len);
		used += len;
	}
	out[used] = 0;
}

/* ---------------------------------------------------------------- drawing */

typedef struct build
{
	char *p;
	char *end;
} build_t;

static void put(build_t *b, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b->p, (size_t)(b->end - b->p), fmt, ap);
	va_end(ap);
	if (n > 0)
	{
		ptrdiff_t room = b->end - b->p - 1;
		b->p += (ptrdiff_t)n < room ? (ptrdiff_t)n : room;
	}
}

static void pad(build_t *b, int columns)
{
	while (columns-- > 0)
		put(b, " ");
}

#define BOX_W 66

static void rule(build_t *b, const char *left, const char *right)
{
	put(b, " %s%s", DIM, left);
	for (int n = 0; n < BOX_W; n++)
		put(b, "─");
	put(b, "%s%s\n", right, OFF);
}

static void time_text(char *out, size_t size, double seconds)
{
	if (seconds < 0)
		seconds = 0;
	int s = (int)(seconds + 0.0001);
	snprintf(out, size, "%d:%02d", s / 60, s % 60);
}

static const char *LED_NAME[SCEMU_LED_COUNT] =
{
	"ALL", "MUTE", "SC-55", "SC-88", "E1", "E2", "E3", "INST", "EFX"
};

static void build_frame(tui_t *t, const tui_state_t *st)
{
	build_t b = { t->frame, t->frame + sizeof(t->frame) };
	const scemu_lcd_t *lcd = st->lcd;
	char part[32], inst[80], f[6][32];
	uint16_t bar[16];
	char elapsed[16], total[16];

	field(part, sizeof(part), lcd, 0, 3);
	field(inst, sizeof(inst), lcd, 3, 16);
	field(f[0], sizeof(f[0]), lcd, 40 + 0, 3);    /* LEVEL */
	field(f[1], sizeof(f[1]), lcd, 40 + 3, 3);    /* PAN */
	field(f[2], sizeof(f[2]), lcd, 40 + 6, 3);    /* CHORUS */
	field(f[3], sizeof(f[3]), lcd, 40 + 9, 3);    /* REVERB */
	field(f[4], sizeof(f[4]), lcd, 40 + 12, 3);   /* KEY SHIFT */
	field(f[5], sizeof(f[5]), lcd, 40 + 15, 3);   /* MIDI CH */
	read_bars(lcd, bar);
	bool lr = read_lr(lcd);

	time_text(elapsed, sizeof(elapsed), st->elapsed);
	time_text(total, sizeof(total), st->total);

	put(&b, "\n");
	put(&b, " %sscplay%s  %s%s%s  %s%.40s%s", PLAIN, OFF, LIT, st->model, OFF, PLAIN, st->song, OFF);
	if (st->title && st->title[0])
		put(&b, "  %s%.32s%s", DIM, st->title, OFF);
	put(&b, "\n");
	put(&b, "\n");

	rule(&b, "┌", "┐");

	static const char *LEFT_LABEL[4] = { "PART", "LEVEL", "REVERB", "KEY SHIFT" };
	static const char *RIGHT_LABEL[4] = { "INSTRUMENT", "PAN", "CHORUS", "MIDI CH" };
	const char *left_value[4] = { part, f[0], f[3], f[4] };
	const char *right_value[4] = { inst, f[1], f[2], f[5] };
	static const int LEFT_W = 11, RIGHT_W = 18;

	for (int row = 0; row < 8; row++)
	{
		int pair = row / 2;
		bool label = (row % 2) == 0;
		put(&b, " %s│%s", DIM, OFF);

		if (label)
		{
			put(&b, "%s %-*s%-*s%s", LABEL, LEFT_W - 1, LEFT_LABEL[pair], RIGHT_W, RIGHT_LABEL[pair], OFF);
			put(&b, "%s%s%s", LIT, row == 0 ? (lr ? " L " : "   ") : "   ", OFF);
		}
		else
		{
			int lw = LEFT_W - 1 - (int)strlen(left_value[pair]);
			int rw = RIGHT_W - (int)strlen(right_value[pair]);
			put(&b, "%s %s%s", LIT, left_value[pair], OFF);
			pad(&b, lw > 0 ? lw : 0);
			put(&b, "%s%s%s", LIT, right_value[pair], OFF);
			pad(&b, rw > 0 ? rw : 0);
			put(&b, "%s%s%s", LIT, row == 7 && lr ? " R " : "   ", OFF);
		}

		for (int n = 0; n < 16; n++)
		{
			int top = (bar[n] >> (row * 2)) & 1;
			int bottom = (bar[n] >> (row * 2 + 1)) & 1;
			const char *cell = top && bottom ? "█" : top ? "▀" : bottom ? "▄" : NULL;
			if (cell)
				put(&b, "%s%s%s", LIT, cell, OFF);
			else
				put(&b, "%s·%s", DIM, OFF);
			if (n != 15)
				put(&b, " ");
		}
		pad(&b, BOX_W - (LEFT_W + RIGHT_W + 3 + 31));
		put(&b, "%s│%s\n", DIM, OFF);
	}
	rule(&b, "└", "┘");

	put(&b, "\n");
	put(&b, " ");
	for (int n = 0; n < SCEMU_LED_COUNT; n++)
	{
		const char *name = LED_NAME[n];
		if (n == SCEMU_LED_SC88_MAP && st->eq_label)
			name = "EQ";
		if (n == SCEMU_LED_USER_INST_RED && !st->has_efx_led)
			continue;
		bool on = (st->leds >> n) & 1;
		put(&b, "%s%s %s%s ", on ? LIT : DIM, on ? "●" : "○", name, OFF);
	}
	put(&b, "\n");
	put(&b, "\n");

	int width = 44;
	double frac = st->total > 0 ? st->elapsed / st->total : 0;
	if (frac < 0) frac = 0;
	if (frac > 1) frac = 1;
	int done = (int)(frac * width + 0.5);
	put(&b, " %s%5s%s %s▏%s", PLAIN, elapsed, OFF, DIM, OFF);
	put(&b, "%s", LIT);
	for (int n = 0; n < done; n++)
		put(&b, "█");
	put(&b, "%s%s", OFF, DIM);
	for (int n = done; n < width; n++)
		put(&b, "░");
	put(&b, "▕%s %s%s%s", OFF, PLAIN, total, OFF);
	put(&b, "\n");

	put(&b, "\n");
	put(&b, " %s%s%s", PLAIN, st->paused ? "‖ paused" : (st->status ? st->status : "▶ playing"), OFF);
	if (st->underruns)
		put(&b, "   %s%u dropout%s%s", DIM, st->underruns, st->underruns == 1 ? "" : "s", OFF);
	if (st->speed > 0)
		put(&b, "   %s%.1f× real time%s", DIM, st->speed, OFF);
	put(&b, "\n");
	put(&b, "\n");

	if (st->show_keys)
	{
		static const char *KEYS[] =
		{
			"space  pause / resume        q, Esc  quit",
			"← →    PART                  ↑ ↓     INSTRUMENT",
			"- =    LEVEL                 [ ]     PAN",
			"; '    REVERB                , .     CHORUS",
			"k l    KEY SHIFT             n m     MIDI CH",
			"a      ALL                   x       MUTE",
			"5      SC-55 map             8       SC-88 map / EQ",
			"u      USER INST / EFX       s       SELECT",
			"v      PREVIEW (volume knob) ?       hide this list",
			NULL
		};
		for (int n = 0; KEYS[n]; n++)
		{
			put(&b, " %s%s%s\n", DIM, KEYS[n], OFF);
		}
	}
	else
	{
		put(&b, " %sspace pause  q quit  ←→ part  ↑↓ instrument  ? all keys%s\n", DIM, OFF);
	}

	t->len = (size_t)(b.p - t->frame);
}

void tui_draw(tui_t *t, const tui_state_t *st)
{
	if (!t)
		return;
	build_frame(t, st);
	if (t->len == strlen(t->prev) && !memcmp(t->frame, t->prev, t->len))
		return;
	memcpy(t->prev, t->frame, t->len + 1);

	fputs("\033[H", stdout);
	const char *p = t->frame;
	while (*p)
	{
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		fwrite(p, 1, len, stdout);
		fputs("\033[K", stdout);
		if (!nl)
			break;
		fputs("\n", stdout);
		p = nl + 1;
	}
	fputs("\033[J", stdout);
	fflush(stdout);
}
