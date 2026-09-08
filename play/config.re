/* The players' settings file.  The format is in config.h; this is a token
 * lexer and a table of typed slots, so a new key is one more row.
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
#include <sys/stat.h>
#include "config.h"

/* ------------------------------------------------------------------ keys */

typedef enum
{
	SLOT_TEXT,
	SLOT_INT,
	SLOT_BOOL,
	SLOT_REAL,
} slot_t;

typedef struct key
{
	const char *name;
	slot_t slot;
	unsigned short offset, size;
	double low, high;          /* the range a number is clamped to */
	const char *const *words;  /* the words a text key takes, NULL terminated */
	const int *numbers;        /* the numbers an int key takes, -1 terminated */
	const char *group;
} config_key_t;

static const char *const model_words[] = { "", "sc88pro", "sc88", "sc88vl", "sc8850", "sc55mk2", NULL };
static const char *const reset_words[] = { "none", "gm", "gs", "gm2", "sc88-single", "sc88-double", NULL };
static const char *const map_words[] = { "native", "sc55", "sc88", "sc88pro", "sc8850", NULL };
static const int size_numbers[] = { 4, 8, -1 };
static const int rate_numbers[] = { 0, 31250, 38400, -1 };
static const int audio_rate_numbers[] = { 0, 32000, 44100, 48000, -1 };
static const char *const computer_words[] = { "midi", "pc1", "pc2", "mac", "usb", NULL };

static const char group_machine[] = "the machine: which module, where its ROMs are, and what it remembers";
static const char group_window[] = "the window: 4 the small panel, 8 twice as large, and what the"
                                  " pointer's buttons do";
static const char group_computer[] = "the rear COMPUTER switch of each system: midi, pc1, pc2, mac (usb on the SC-8850)";
static const char group_audio[] = "audio: the output device, the rate asked of it (0 the machine's own), its buffer"
                                  " and the knob";
static const char group_midi[] = "MIDI: the host ports (C and D only on an SC-8850 on USB), and the speed of the inputs";
static const char group_song[] = "each song: the reset that precedes it and the tail that follows it";

#define FIELD(f) (unsigned short)offsetof(scgui_config_t, f), (unsigned short)sizeof(((scgui_config_t *)0)->f)

static const config_key_t keys[] = {
	{ "model",            SLOT_TEXT, FIELD(model),         0, 0,     model_words,    NULL,         group_machine },
	{ "rom",              SLOT_TEXT, FIELD(rom),           0, 0,     NULL,           NULL,         group_machine },
	{ "map",              SLOT_TEXT, FIELD(map),           0, 0,     map_words,      NULL,         group_machine },
	{ "keep_settings",    SLOT_BOOL, FIELD(keep_settings), 0, 0,     NULL,           NULL,         group_machine },
	{ "computer_sc55mk2", SLOT_TEXT, FIELD(computer[CONFIG_ROW_SC55MK2]), 0, 0, computer_words, NULL, group_computer },
	{ "computer_sc88",    SLOT_TEXT, FIELD(computer[CONFIG_ROW_SC88]),    0, 0, computer_words, NULL, group_computer },
	{ "computer_sc88vl",  SLOT_TEXT, FIELD(computer[CONFIG_ROW_SC88VL]),  0, 0, computer_words, NULL, group_computer },
	{ "computer_sc88pro", SLOT_TEXT, FIELD(computer[CONFIG_ROW_SC88PRO]), 0, 0, computer_words, NULL, group_computer },
	{ "computer_sc8850",  SLOT_TEXT, FIELD(computer[CONFIG_ROW_SC8850]),  0, 0, computer_words, NULL, group_computer },
	{ "size",             SLOT_INT,  FIELD(size),          0, 0,     NULL,           size_numbers, group_window },
	{ "swap_buttons",     SLOT_BOOL, FIELD(swap_buttons),  0, 0,     NULL,           NULL,         group_window },
	{ "audio_device",     SLOT_TEXT, FIELD(audio_device),  0, 0,     NULL,           NULL,         group_audio },
	{ "audio_rate",       SLOT_INT,  FIELD(audio_rate),    0, 0,     NULL,     audio_rate_numbers, group_audio },
	{ "audio_block",      SLOT_INT,  FIELD(audio_block),   64, 1024, NULL,           NULL,         group_audio },
	{ "volume",           SLOT_REAL, FIELD(volume),        0, 1,     NULL,           NULL,         group_audio },
	{ "midi_in_a",        SLOT_TEXT, FIELD(midi[0]),       0, 0,     NULL,           NULL,         group_midi },
	{ "midi_in_b",        SLOT_TEXT, FIELD(midi[1]),       0, 0,     NULL,           NULL,         group_midi },
	{ "midi_in_c",        SLOT_TEXT, FIELD(midi[2]),       0, 0,     NULL,           NULL,         group_midi },
	{ "midi_in_d",        SLOT_TEXT, FIELD(midi[3]),       0, 0,     NULL,           NULL,         group_midi },
	{ "midi_out",         SLOT_TEXT, FIELD(midi[4]),       0, 0,     NULL,           NULL,         group_midi },
	{ "song_a",           SLOT_TEXT, FIELD(midi[5]),       0, 0,     NULL,           NULL,         group_midi },
	{ "song_b",           SLOT_TEXT, FIELD(midi[6]),       0, 0,     NULL,           NULL,         group_midi },
	{ "song_c",           SLOT_TEXT, FIELD(midi[7]),       0, 0,     NULL,           NULL,         group_midi },
	{ "song_d",           SLOT_TEXT, FIELD(midi[8]),       0, 0,     NULL,           NULL,         group_midi },
	{ "midi_rate",        SLOT_INT,  FIELD(midi_rate),     0, 0,     NULL,           rate_numbers, group_midi },
	{ "reset",            SLOT_TEXT, FIELD(reset),         0, 0,     reset_words,    NULL,         group_song },
	{ "tail",             SLOT_REAL, FIELD(tail),          0, 300,   NULL,           NULL,         group_song },
};

#undef FIELD

void config_defaults(scgui_config_t *c)
{
	memset(c, 0, sizeof(*c));
	c->size = 4;
	c->audio_block = 256;
	snprintf(c->reset, sizeof(c->reset), "gs");
	snprintf(c->map, sizeof(c->map), "native");
	for (int n = 0; n < CONFIG_SYSTEMS; n++)
		snprintf(c->computer[n], sizeof(c->computer[n]), "midi");
	snprintf(c->computer[CONFIG_ROW_SC8850], sizeof(c->computer[CONFIG_ROW_SC8850]), "usb");
	c->midi_rate = 31250;
	c->volume = 0.75f;
	c->swap_buttons = false;
	c->tail = 4;
}

static const config_key_t *find_key(const char *name)
{
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		if (!strcmp(keys[i].name, name))
			return &keys[i];
	return NULL;
}

/* keys a file written by an older build may still carry: taken and dropped */
static const char *const retired_keys[] = { "knob_notches", NULL };

static bool retired_key(const char *name)
{
	for (int n = 0; retired_keys[n]; n++)
		if (!strcmp(retired_keys[n], name))
			return true;
	return false;
}

/* -------------------------------------------------- numbers without a locale */

static const long long tens[] = { 1, 10, 100, 1000, 10000, 100000, 1000000,
                                  10000000, 100000000, 1000000000 };

static bool parse_real(const char *s, double *out)
{
	const char *p = s;
	double sign = 1;
	unsigned long long mantissa = 0;
	int digits = 0, decimals = 0;
	if (*p == '+' || *p == '-')
		sign = *p++ == '-' ? -1 : 1;
	for (; *p >= '0' && *p <= '9'; p++, digits++)
		if (mantissa < 1000000000000000000ULL)
			mantissa = mantissa * 10 + (unsigned)(*p - '0');
	if (*p == '.')
		for (p++; *p >= '0' && *p <= '9'; p++, digits++)
			if (mantissa < 1000000000000000000ULL)
			{
				mantissa = mantissa * 10 + (unsigned)(*p - '0');
				decimals++;
			}
	if (!digits || *p)
		return false;
	double scale = 1;
	while (decimals-- > 0)
		scale *= 10;
	*out = sign * ((double)mantissa / scale);
	return true;
}

static void format_fixed(char *out, size_t size, double v, int decimals)
{
	const char *sign = "";
	if (v < 0)
	{
		v = -v;
		sign = "-";
	}
	long long scale = tens[decimals];
	long long units = (long long)(v * (double)scale + 0.5);
	snprintf(out, size, "%s%lld.%0*lld", sign, units / scale, decimals, units % scale);
}

/* The fewest decimals that read back as the same float. */
static void format_real(char *out, size_t size, float v)
{
	for (int decimals = 1; decimals < 9; decimals++)
	{
		double back;
		format_fixed(out, size, v, decimals);
		if (parse_real(out, &back) && (float)back == v)
			return;
	}
	format_fixed(out, size, v, 9);
}

/* ----------------------------------------------------------------- the lexer */

typedef enum
{
	T_END,
	T_EOL,
	T_EQUAL,
	T_NAME,
	T_NUMBER,
	T_WORD,
	T_STRING,
	T_BAD,
} token_kind_t;

typedef struct token
{
	token_kind_t kind;
	char text[1024];
} token_t;

typedef struct lexer
{
	const unsigned char *cur, *mark;
	int line;
} lexer_t;

static token_kind_t take(token_t *tok, token_kind_t kind, const unsigned char *from,
                         const unsigned char *to)
{
	size_t used = (size_t)(to - from);
	if (used >= sizeof(tok->text))
		used = sizeof(tok->text) - 1;
	memcpy(tok->text, from, used);
	tok->text[used] = 0;
	tok->kind = kind;
	return kind;
}

static token_kind_t take_string(token_t *tok, const unsigned char *from, const unsigned char *to)
{
	size_t used = 0;
	for (const unsigned char *p = from + 1; p + 1 < to && used + 1 < sizeof(tok->text); p++)
	{
		unsigned char ch = *p;
		if (ch == '\\' && p + 2 < to)
			switch (ch = *++p)
			{
			case 'n': ch = '\n'; break;
			case 't': ch = '\t'; break;
			case 'r': ch = '\r'; break;
			default: break;
			}
		tok->text[used++] = (char)ch;
	}
	tok->text[used] = 0;
	tok->kind = T_STRING;
	return T_STRING;
}

static token_kind_t lex(lexer_t *lx, token_t *tok)
{
	const unsigned char *from;
	for (;;)
	{
		from = lx->cur;
		/*!re2c
			re2c:define:YYCTYPE = "unsigned char";
			re2c:define:YYCURSOR = "lx->cur";
			re2c:define:YYMARKER = "lx->mark";
			re2c:yyfill:enable = 0;

			nul    = "\x00";
			blank  = [ \t\r];
			name   = [a-z_][a-z0-9_]*;
			number = [-+]? ([0-9]+ ("." [0-9]*)? | "." [0-9]+);
			word   = [^ \t\r\n\x00#="]+;
			text   = ["] ([^"\\\n\x00] | [\\] [^\n\x00])* ["];

			nul            { lx->cur--; return take(tok, T_END, from, from); }
			blank+         { continue; }
			"#" [^\n\x00]* { continue; }
			"\n"           { lx->line++; return take(tok, T_EOL, from, from); }
			"="            { return take(tok, T_EQUAL, from, lx->cur); }
			name           { return take(tok, T_NAME, from, lx->cur); }
			number         { return take(tok, T_NUMBER, from, lx->cur); }
			text           { return take_string(tok, from, lx->cur); }
			word           { return take(tok, T_WORD, from, lx->cur); }
			*              { return take(tok, T_BAD, from, lx->cur); }
		*/
	}
}

static void skip_line(lexer_t *lx)
{
	token_t tok;
	for (;;)
	{
		token_kind_t kind = lex(lx, &tok);
		if (kind == T_EOL || kind == T_END)
			return;
	}
}

/* --------------------------------------------------------------- complaints */

static void complain(char *err, size_t err_size, int line, const char *fmt, ...)
{
	if (!err || !err_size)
		return;
	size_t used = strlen(err);
	if (used)
	{
		if (used + 2 >= err_size)
			return;
		err[used++] = '\n';
		err[used] = 0;
	}
	char text[300];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	snprintf(err + used, err_size - used, "line %d: %s", line, text);
}

static void choices_text(const config_key_t *key, char *out, size_t size)
{
	size_t used = 0;
	out[0] = 0;
	for (int i = 0; key->words ? key->words[i] != NULL : key->numbers[i] >= 0; i++)
	{
		if (used + 32 >= size)
			break;
		if (key->words)
			used += (size_t)snprintf(out + used, size - used, "%s\"%s\"", i ? ", " : "", key->words[i]);
		else
			used += (size_t)snprintf(out + used, size - used, "%s%d", i ? ", " : "", key->numbers[i]);
	}
}

/* ----------------------------------------------------------------- the slots */

/* The lowercase words are the file's; a hand-written Yes is taken too. */
static bool same_word(const char *s, const char *lower)
{
	for (; *s && *lower; s++, lower++)
		if ((*s | 0x20) != *lower)
			return false;
	return !*s && !*lower;
}

static bool parse_bool(const char *s, bool *out)
{
	static const char *const yes[] = { "yes", "true", "on", NULL };
	static const char *const no[] = { "no", "false", "off", NULL };
	for (int i = 0; yes[i]; i++)
		if (same_word(s, yes[i]))
			return *out = true;
	for (int i = 0; no[i]; i++)
		if (same_word(s, no[i]))
		{
			*out = false;
			return true;
		}
	return false;
}

static void apply(scgui_config_t *c, const config_key_t *key, const token_t *tok, int line,
                  char *err, size_t err_size)
{
	char *field = (char *)c + key->offset;
	char choices[300];
	switch (key->slot)
	{
	case SLOT_TEXT:
		if (key->words)
		{
			int i;
			for (i = 0; key->words[i] && strcmp(key->words[i], tok->text); i++)
				;
			if (!key->words[i])
			{
				choices_text(key, choices, sizeof(choices));
				complain(err, err_size, line, "%s: \"%s\" is not one of %s; keeping \"%s\"",
				         key->name, tok->text, choices, field);
				return;
			}
		}
		if (strlen(tok->text) >= key->size)
			complain(err, err_size, line, "%s: the value is too long, truncated to %u characters",
			         key->name, (unsigned)key->size - 1);
		snprintf(field, key->size, "%s", tok->text);
		return;

	case SLOT_INT:
	{
		char *end;
		long v = strtol(tok->text, &end, 10);
		if (end == tok->text || *end)
		{
			complain(err, err_size, line, "%s: \"%s\" is not a number", key->name, tok->text);
			return;
		}
		if (key->numbers)
		{
			int i;
			for (i = 0; key->numbers[i] >= 0 && key->numbers[i] != v; i++)
				;
			if (key->numbers[i] < 0)
			{
				choices_text(key, choices, sizeof(choices));
				complain(err, err_size, line, "%s: %ld is not one of %s; keeping %d",
				         key->name, v, choices, *(int *)field);
				return;
			}
		}
		else if (v < key->low || v > key->high)
		{
			long fit = v < key->low ? (long)key->low : (long)key->high;
			complain(err, err_size, line, "%s: %ld is outside %ld..%ld; using %ld",
			         key->name, v, (long)key->low, (long)key->high, fit);
			v = fit;
		}
		*(int *)field = (int)v;
		return;
	}

	case SLOT_BOOL:
		if (!parse_bool(tok->text, (bool *)field))
			complain(err, err_size, line, "%s: \"%s\" is not yes or no", key->name, tok->text);
		return;

	case SLOT_REAL:
	{
		double v;
		if (!parse_real(tok->text, &v))
		{
			complain(err, err_size, line, "%s: \"%s\" is not a number", key->name, tok->text);
			return;
		}
		if (v < key->low || v > key->high)
		{
			char low[32], high[32];
			double fit = v < key->low ? key->low : key->high;
			format_real(low, sizeof(low), (float)key->low);
			format_real(high, sizeof(high), (float)key->high);
			complain(err, err_size, line, "%s: %s is outside %s..%s; using %s", key->name,
			         tok->text, low, high, v < key->low ? low : high);
			v = fit;
		}
		*(float *)field = (float)v;
		return;
	}
	}
}

/* ------------------------------------------------------------------- loading */

static bool parse(scgui_config_t *c, const char *body, char *err, size_t err_size)
{
	lexer_t lx = { (const unsigned char *)body, NULL, 1 };
	token_t tok;
	char name[sizeof(tok.text)];
	for (;;)
	{
		token_kind_t kind = lex(&lx, &tok);
		if (kind == T_END)
			return true;
		if (kind == T_EOL)
			continue;
		int line = lx.line;
		if (kind != T_NAME)
		{
			complain(err, err_size, line, "expected a key name, found \"%s\"", tok.text);
			skip_line(&lx);
			continue;
		}
		snprintf(name, sizeof(name), "%s", tok.text);

		if (lex(&lx, &tok) != T_EQUAL)
		{
			complain(err, err_size, line, "expected '=' after '%s'", name);
			if (tok.kind != T_EOL && tok.kind != T_END)
				skip_line(&lx);
			continue;
		}
		kind = lex(&lx, &tok);
		if (kind == T_EOL || kind == T_END)
		{
			complain(err, err_size, line, "'%s' has no value", name);
			continue;
		}
		if (kind == T_BAD)
		{
			complain(err, err_size, line, "'%s': \"%s\" is not a value", name, tok.text);
			skip_line(&lx);
			continue;
		}

		const config_key_t *key = find_key(name);
		if (key)
			apply(c, key, &tok, line, err, err_size);
		else if (!retired_key(name))
			complain(err, err_size, line, "unknown key '%s'", name);

		kind = lex(&lx, &tok);
		if (kind != T_EOL && kind != T_END)
		{
			complain(err, err_size, line, "'%s': more than one value on the line", name);
			skip_line(&lx);
		}
		if (kind == T_END)
			return true;
	}
}

bool config_load(scgui_config_t *c, const char *path, char *err, size_t err_size)
{
	if (err && err_size)
		err[0] = 0;
	config_defaults(c);

	FILE *fp = fopen(path, "rb");
	if (!fp)
		return errno == ENOENT;
	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (size < 0)
	{
		fclose(fp);
		return false;
	}
	char *body = malloc((size_t)size + 1);
	if (!body)
	{
		fclose(fp);
		return false;
	}
	size_t got = fread(body, 1, (size_t)size, fp);
	bool ok = !ferror(fp);
	fclose(fp);
	body[got] = 0;
	if (ok)
		ok = parse(c, body, err, err_size);
	free(body);
	return ok;
}

/* ------------------------------------------------------------------- writing */

static void write_text(FILE *fp, const char *s)
{
	fputc('"', fp);
	for (; *s; s++)
		switch (*s)
		{
		case '"': fputs("\\\"", fp); break;
		case '\\': fputs("\\\\", fp); break;
		case '\n': fputs("\\n", fp); break;
		case '\t': fputs("\\t", fp); break;
		case '\r': fputs("\\r", fp); break;
		default: fputc(*s, fp); break;
		}
	fputc('"', fp);
}

static void write_keys(FILE *fp, const scgui_config_t *c)
{
	const char *group = NULL;
	char real[32];
	fputs("# scgui's settings, rewritten when it exits.\n", fp);
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
	{
		const config_key_t *key = &keys[i];
		const char *field = (const char *)c + key->offset;
		if (key->group != group)
		{
			group = key->group;
			fprintf(fp, "\n# %s\n", group);
		}
		fprintf(fp, "%s = ", key->name);
		switch (key->slot)
		{
		case SLOT_TEXT: write_text(fp, field); break;
		case SLOT_INT: fprintf(fp, "%d", *(const int *)field); break;
		case SLOT_BOOL: fputs(*(const bool *)field ? "yes" : "no", fp); break;
		case SLOT_REAL:
			format_real(real, sizeof(real), *(const float *)field);
			fputs(real, fp);
			break;
		}
		fputc('\n', fp);
	}
}

bool config_save(const scgui_config_t *c, const char *path)
{
	char tmp[1200];
	if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp))
		return false;
	FILE *fp = fopen(tmp, "wb");
	if (!fp)
		return false;
	write_keys(fp, c);
	bool ok = !ferror(fp);
	ok = fclose(fp) == 0 && ok;
	if (ok && rename(tmp, path) == 0)
		return true;
	remove(tmp);
	return false;
}

/* ----------------------------------------------------------------- the place */

static bool make_dir(const char *path)
{
	return mkdir(path, 0755) == 0 || errno == EEXIST;
}

bool config_path(char *out, size_t size)
{
	char dir[1024];
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (xdg && *xdg)
		snprintf(dir, sizeof(dir), "%s", xdg);
	else if (home && *home)
		snprintf(dir, sizeof(dir), "%s/.config", home);
	else
		return false;
	if (!make_dir(dir))
		return false;
	char sub[1100];
	if ((size_t)snprintf(sub, sizeof(sub), "%s/scemu", dir) >= sizeof(sub) || !make_dir(sub))
		return false;
	return (size_t)snprintf(out, size, "%s/scgui.conf", sub) < size;
}
