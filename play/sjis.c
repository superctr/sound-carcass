/* scplay: Shift-JIS (cp932) text to UTF-8, and the display width of UTF-8.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <string.h>
#include "sjis.h"
#include "sjis_table.h"

#define REPLACEMENT 0xfffdu

static size_t put_utf8(uint32_t code, char *out, size_t size, size_t used)
{
	uint8_t seq[3];
	size_t length;

	if (code < 0x80)
	{
		seq[0] = (uint8_t)code;
		length = 1;
	}
	else if (code < 0x800)
	{
		seq[0] = (uint8_t)(0xc0 | (code >> 6));
		seq[1] = (uint8_t)(0x80 | (code & 0x3f));
		length = 2;
	}
	else
	{
		seq[0] = (uint8_t)(0xe0 | (code >> 12));
		seq[1] = (uint8_t)(0x80 | ((code >> 6) & 0x3f));
		seq[2] = (uint8_t)(0x80 | (code & 0x3f));
		length = 3;
	}

	if (used + length + 1 > size)
		return used;
	memcpy(out + used, seq, length);
	return used + length;
}

bool sjis_is_utf8(const uint8_t *in, size_t length)
{
	size_t n = 0;
	while (n < length)
	{
		uint8_t c = in[n];
		size_t need;
		uint32_t code;

		if (c < 0x80)
		{
			n++;
			continue;
		}
		if ((c & 0xe0) == 0xc0)
		{
			need = 1;
			code = c & 0x1fu;
		}
		else if ((c & 0xf0) == 0xe0)
		{
			need = 2;
			code = c & 0x0fu;
		}
		else if ((c & 0xf8) == 0xf0)
		{
			need = 3;
			code = c & 0x07u;
		}
		else
			return false;

		if (n + need >= length)
			return false;
		for (size_t k = 1; k <= need; k++)
		{
			if ((in[n + k] & 0xc0) != 0x80)
				return false;
			code = (code << 6) | (in[n + k] & 0x3fu);
		}
		if (need == 1 && code < 0x80)
			return false;
		if (need == 2 && (code < 0x800 || (code >= 0xd800 && code < 0xe000)))
			return false;
		if (need == 3 && (code < 0x10000 || code > 0x10ffff))
			return false;
		n += need + 1;
	}
	return true;
}

size_t sjis_decode(const uint8_t *in, size_t length, char *out, size_t size)
{
	size_t used = 0;

	if (!size)
		return 0;
	for (size_t n = 0; n < length; )
	{
		uint8_t c = in[n];
		uint32_t code;

		if (c < 0x80)
		{
			code = c;
			n++;
		}
		else if (c >= 0xa1 && c <= 0xdf)
		{
			code = 0xff61u + c - 0xa1u;
			n++;
		}
		else if (sjis_row_index[c] == 0xff || n + 1 >= length)
		{
			code = REPLACEMENT;
			n++;
		}
		else
		{
			uint8_t trail = in[n + 1];
			if (trail < SJIS_TRAIL_FIRST || trail > SJIS_TRAIL_LAST)
			{
				code = REPLACEMENT;
				n++;
			}
			else
			{
				code = sjis_rows[sjis_row_index[c]][trail - SJIS_TRAIL_FIRST];
				if (!code)
					code = REPLACEMENT;
				n += 2;
			}
		}

		size_t grown = put_utf8(code, out, size, used);
		if (grown == used)
			break;
		used = grown;
	}
	out[used] = 0;
	return used;
}

size_t sjis_text(const uint8_t *in, size_t length, char *out, size_t size)
{
	size_t used;

	if (!size)
		return 0;
	if (sjis_is_utf8(in, length))
	{
		used = length < size - 1 ? length : size - 1;
		if (used < length)
			while (used && (in[used] & 0xc0) == 0x80)
				used--;
		memcpy(out, in, used);
		out[used] = 0;
	}
	else
		used = sjis_decode(in, length, out, size);

	for (size_t n = 0; n < used; n++)
		if ((uint8_t)out[n] < 0x20 || (uint8_t)out[n] == 0x7f)
			out[n] = ' ';
	return used;
}

static size_t utf8_next(const char *p, uint32_t *code)
{
	const uint8_t *b = (const uint8_t *)p;
	uint8_t c = b[0];
	size_t need;
	uint32_t v;

	if (c < 0x80)
	{
		*code = c;
		return 1;
	}
	if ((c & 0xe0) == 0xc0)
	{
		need = 1;
		v = c & 0x1fu;
	}
	else if ((c & 0xf0) == 0xe0)
	{
		need = 2;
		v = c & 0x0fu;
	}
	else if ((c & 0xf8) == 0xf0)
	{
		need = 3;
		v = c & 0x07u;
	}
	else
	{
		*code = REPLACEMENT;
		return 1;
	}
	for (size_t n = 1; n <= need; n++)
	{
		if ((b[n] & 0xc0) != 0x80)
		{
			*code = REPLACEMENT;
			return 1;
		}
		v = (v << 6) | (b[n] & 0x3fu);
	}
	*code = v;
	return need + 1;
}

static int code_columns(uint32_t code)
{
	if (code >= 0x1100 && code <= 0x115f)
		return 2;
	if (code >= 0x2e80 && code <= 0xa4cf)
		return 2;
	if (code >= 0xac00 && code <= 0xd7a3)
		return 2;
	if (code >= 0xf900 && code <= 0xfaff)
		return 2;
	if (code >= 0xfe30 && code <= 0xfe4f)
		return 2;
	if (code >= 0xff00 && code <= 0xff60)
		return 2;
	if (code >= 0xffe0 && code <= 0xffe6)
		return 2;
	return 1;
}

int sjis_columns(const char *text)
{
	int columns = 0;

	if (!text)
		return 0;
	while (*text)
	{
		uint32_t code;
		text += utf8_next(text, &code);
		columns += code_columns(code);
	}
	return columns;
}

size_t sjis_fit(const char *text, int columns)
{
	const char *p = text;
	int used = 0;

	if (!text || columns <= 0)
		return 0;
	while (*p)
	{
		uint32_t code;
		size_t length = utf8_next(p, &code);
		int width = code_columns(code);
		if (used + width > columns)
			break;
		used += width;
		p += length;
	}
	return (size_t)(p - text);
}
