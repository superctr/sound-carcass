/* scplay: the text-mode front panel.  The glass is drawn from the display
 * controller's memory: on the character models the text fields as text and the
 * sixteen level bars from the CGRAM patterns the firmware fills, two segments
 * to a character cell; on the SC-8850 the whole 160 x 64 bitmap, two dot rows
 * to a character cell.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <locale.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include "sjis.h"
#include "tui.h"

#define FRAME_MAX 65536
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

	setlocale(LC_CTYPE, "");

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

/* a cell drawn as it stands: the bitmap glass writes 5120 of them a frame */
static void put_raw(build_t *b, const char *s, size_t len)
{
	if ((size_t)(b->end - b->p) > len)
	{
		memcpy(b->p, s, len);
		b->p += len;
		*b->p = 0;
	}
}

static void pad(build_t *b, int columns)
{
	while (columns-- > 0)
		put(b, " ");
}

#define BOX_W 66

/* the bitmap glass, in half-block cells */
#define GLCD_COLS 160
#define GLCD_ROWS 64
#define GLCD_STRIDE 27
#define GLCD_DOTS_PER_BYTE 6

static void rule(build_t *b, const char *left, const char *right, int width)
{
	put(b, " %s%s", DIM, left);
	for (int n = 0; n < width; n++)
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
	"ALL", "MUTE", "SC-55", "SC-88", "E1", "E2", "E3", "INST", "EFX",
	"SOLO", "EDIT", "DRUM", "EFFECTS", "STANDBY",
	"POWER", "USB", "INST MAP", "", "", "", "PART A", "", "", "", "PART B"
};

static const uint8_t LED_ROW_LCD[] =
{
	SCEMU_LED_ALL, SCEMU_LED_MUTE, SCEMU_LED_SC55_MAP, SCEMU_LED_SC88_MAP,
	SCEMU_LED_EDIT1, SCEMU_LED_EDIT2, SCEMU_LED_EDIT3, SCEMU_LED_USER_INST, SCEMU_LED_USER_INST_RED
};

static const uint8_t LED_ROW_SC55MK2[] =
{
	SCEMU_LED_ALL, SCEMU_LED_MUTE, SCEMU_LED_STANDBY
};

static const uint8_t LED_ROW_GLCD[] =
{
	SCEMU_LED_MUTE, SCEMU_LED_SOLO, SCEMU_LED_EDIT, SCEMU_LED_DRUM, SCEMU_LED_EFFECTS
};

/* the SC-8820's: the two level strips read left to right, the lamp with a name closing each */
static const uint8_t LED_ROW_SC8820[] =
{
	SCEMU_LED_POWER, SCEMU_LED_USB, SCEMU_LED_MAP,
	SCEMU_LED_PART_A1, SCEMU_LED_PART_A2, SCEMU_LED_PART_A3, SCEMU_LED_PART_A4,
	SCEMU_LED_PART_B1, SCEMU_LED_PART_B2, SCEMU_LED_PART_B3, SCEMU_LED_PART_B4
};

/* the panel of the models whose glass is character cells */
static void draw_lcd_panel(build_t *b, const tui_state_t *st)
{
	const scemu_lcd_t *lcd = st->lcd;
	char part[32], inst[80], f[6][32];
	uint16_t bar[16];

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

	rule(b, "┌", "┐", BOX_W);

	static const char *LEFT_LABEL[4] = { "PART", "LEVEL", "REVERB", "KEY SHIFT" };
	static const char *RIGHT_LABEL[4] = { "INSTRUMENT", "PAN", "CHORUS", "MIDI CH" };
	const char *left_value[4] = { part, f[0], f[3], f[4] };
	const char *right_value[4] = { inst, f[1], f[2], f[5] };
	static const int LEFT_W = 11, RIGHT_W = 18;

	for (int row = 0; row < 8; row++)
	{
		int pair = row / 2;
		bool label = (row % 2) == 0;
		put(b, " %s│%s", DIM, OFF);

		if (label)
		{
			put(b, "%s %-*s%-*s%s", LABEL, LEFT_W - 1, LEFT_LABEL[pair], RIGHT_W, RIGHT_LABEL[pair], OFF);
			put(b, "%s%s%s", LIT, row == 0 ? (lr ? " L " : "   ") : "   ", OFF);
		}
		else
		{
			int lw = LEFT_W - 1 - sjis_columns(left_value[pair]);
			int rw = RIGHT_W - sjis_columns(right_value[pair]);
			put(b, "%s %s%s", LIT, left_value[pair], OFF);
			pad(b, lw > 0 ? lw : 0);
			put(b, "%s%s%s", LIT, right_value[pair], OFF);
			pad(b, rw > 0 ? rw : 0);
			put(b, "%s%s%s", LIT, row == 7 && lr ? " R " : "   ", OFF);
		}

		for (int n = 0; n < 16; n++)
		{
			int top = (bar[n] >> (row * 2)) & 1;
			int bottom = (bar[n] >> (row * 2 + 1)) & 1;
			const char *cell = top && bottom ? "█" : top ? "▀" : bottom ? "▄" : NULL;
			if (cell)
				put(b, "%s%s%s", LIT, cell, OFF);
			else
				put(b, "%s·%s", DIM, OFF);
			if (n != 15)
				put(b, " ");
		}
		pad(b, BOX_W - (LEFT_W + RIGHT_W + 3 + 31));
		put(b, "%s│%s\n", DIM, OFF);
	}
	rule(b, "└", "┘", BOX_W);
}

/* the SC-8850's glass: one character cell to a column and two dot rows, cut on
 * the right when the terminal is narrower than the display */
static void draw_glcd_panel(build_t *b, const scemu_glcd_t *glcd, int columns)
{
	static const struct { const char *cell; size_t len; } HALF[4] =
	{
		{ " ", 1 }, { "▀", 3 }, { "▄", 3 }, { "█", 3 }
	};
	int cols = GLCD_COLS;
	if (columns > 0 && columns - 3 < cols)
		cols = columns - 3;
	if (cols < 1)
		return;

	rule(b, "┌", "┐", cols);
	for (int row = 0; row < GLCD_ROWS / 2; row++)
	{
		const uint8_t *top = glcd->bitmap + (size_t)(row * 2) * GLCD_STRIDE;
		const uint8_t *bottom = top + GLCD_STRIDE;
		put(b, " %s│%s%s", DIM, OFF, LIT);
		for (int col = 0; col < cols; col++)
		{
			int bit = 0x80 >> (col % GLCD_DOTS_PER_BYTE), byte = col / GLCD_DOTS_PER_BYTE;
			int k = glcd->display_on ? ((top[byte] & bit) ? 1 : 0) | ((bottom[byte] & bit) ? 2 : 0) : 0;
			put_raw(b, HALF[k].cell, HALF[k].len);
		}
		put(b, "%s%s│%s\n", OFF, DIM, OFF);
	}
	rule(b, "└", "┘", cols);
}

/* the terminal's width, or 0 when it cannot be had */
static int terminal_columns(void)
{
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		return ws.ws_col;
	return 0;
}

static void build_frame(tui_t *t, const tui_state_t *st)
{
	build_t b = { t->frame, t->frame + sizeof(t->frame) };
	char elapsed[16], total[16];

	time_text(elapsed, sizeof(elapsed), st->elapsed);
	time_text(total, sizeof(total), st->total);

	put(&b, "\n");
	put(&b, " %sscplay%s  %s%s%s  %s%.*s%s", PLAIN, OFF, LIT, st->model, OFF, PLAIN,
	    (int)sjis_fit(st->song, 40), st->song, OFF);
	if (st->title && st->title[0])
		put(&b, "  %s%.*s%s", DIM, (int)sjis_fit(st->title, 32), st->title, OFF);
	put(&b, "\n");
	put(&b, "\n");

	if (st->glcd)
		draw_glcd_panel(&b, st->glcd, terminal_columns());
	else if (st->lcd)
		draw_lcd_panel(&b, st);

	put(&b, "\n");
	put(&b, " ");
	const uint8_t *row = st->lamps_only ? LED_ROW_SC8820 : st->glcd ? LED_ROW_GLCD
			: st->standby_lamp ? LED_ROW_SC55MK2 : LED_ROW_LCD;
	int row_count = (int)(st->lamps_only ? sizeof(LED_ROW_SC8820) : st->glcd ? sizeof(LED_ROW_GLCD)
			: st->standby_lamp ? sizeof(LED_ROW_SC55MK2) : sizeof(LED_ROW_LCD));
	for (int k = 0; k < row_count; k++)
	{
		int n = row[k];
		const char *name = LED_NAME[n];
		if (n == SCEMU_LED_SC88_MAP && st->eq_label)
			name = "EQ";
		if (n == SCEMU_LED_USER_INST_RED && !st->has_efx_led)
			continue;
		bool on = (st->leds >> n) & 1;
		if (name[0])
			put(&b, "%s%s %s%s ", on ? LIT : DIM, on ? "●" : "○", name, OFF);
		else
			put(&b, "%s%s%s", on ? LIT : DIM, on ? "●" : "○", OFF);
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

	static const char *KEYS_LCD[] =
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
	static const char *KEYS_GLCD[] =
	{
		"space  pause / resume        q, Esc  quit",
		"← →    PART                  ↑ ↓     UP / DOWN",
		"[ ]    VALUE dial            - =     DEC / INC",
		"1 - 4  F1 to F4              m       INST MAP",
		"e      EDIT / UTIL           d       DRUM",
		"f      EFFECTS               s       SHIFT",
		"o      SOLO                  x       MUTE",
		"ret    ENTER                 bksp    EXIT",
		"v      PREVIEW (volume knob) ?       hide this list",
		NULL
	};
	static const char *KEYS_SC8820[] =
	{
		"space  pause / resume        q, Esc  quit",
		"m      INST MAP              v       PREVIEW (volume knob)",
		"?      hide this list",
		NULL
	};
	if (st->show_keys)
	{
		const char *const *keys = st->lamps_only ? KEYS_SC8820 : st->glcd ? KEYS_GLCD : KEYS_LCD;
		for (int n = 0; keys[n]; n++)
		{
			put(&b, " %s%s%s\n", DIM, keys[n], OFF);
		}
	}
	else if (st->glcd)
		put(&b, " %sspace pause  q quit  ←→ part  ↑↓ move  [ ] value  ? all keys%s\n", DIM, OFF);
	else
		put(&b, " %sspace pause  q quit  ←→ part  ↑↓ instrument  ? all keys%s\n", DIM, OFF);

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
