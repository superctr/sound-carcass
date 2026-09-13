/* scplay: Standard MIDI File reader, type 0 and 1, with the tempo map resolved
 * to frames of the machine's own sample clock.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sjis.h"
#include "smf.h"

static uint32_t read_be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t read_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static uint32_t read_vlq(const uint8_t **p, const uint8_t *end)
{
	uint32_t v = 0;
	while (*p < end)
	{
		uint8_t c = *(*p)++;
		v = (v << 7) | (c & 0x7f);
		if (!(c & 0x80))
			break;
	}
	return v;
}

static void take_title(char *out, size_t size, const uint8_t *p, size_t length)
{
	sjis_text(p, length, out, size);
	for (const char *q = out; *q; q++)
		if (*q != ' ')
			return;
	out[0] = 0;
}

static smf_event_t *smf_add(smf_t *s)
{
	if (s->count == s->capacity)
	{
		size_t want = s->capacity ? s->capacity * 2 : 1024;
		smf_event_t *grown = realloc(s->events, want * sizeof(smf_event_t));
		if (!grown)
			return NULL;
		s->events = grown;
		s->capacity = want;
	}
	smf_event_t *e = &s->events[s->count];
	memset(e, 0, sizeof(*e));
	e->order = (uint32_t)s->count++;
	return e;
}

static int parse_track(smf_t *s, unsigned track, const uint8_t *p, const uint8_t *end, char *text, size_t text_size)
{
	uint64_t tick = 0;
	uint8_t running = 0;
	uint8_t port = SMF_PORT_UNSET;
	while (p < end)
	{
		tick += read_vlq(&p, end);
		if (p >= end)
			break;
		uint8_t status = *p;
		if (status == 0xff)
		{
			p++;
			if (p >= end)
				break;
			uint8_t type = *p++;
			uint32_t length = read_vlq(&p, end);
			if (length > (uint32_t)(end - p))
				length = (uint32_t)(end - p);
			if (type == 0x51 && length == 3)
			{
				smf_event_t *e = smf_add(s);
				if (!e)
					return 0;
				e->tick = tick;
				e->tempo_change = 1;
				e->tempo = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
			}
			else if (type == 0x58 && length >= 2 && p[0])
			{
				smf_meter_t *grown = realloc(s->meters, (s->meter_count + 1) * sizeof(smf_meter_t));
				if (!grown)
					return 0;
				s->meters = grown;
				s->meters[s->meter_count].tick = tick;
				s->meters[s->meter_count].beats = p[0];
				s->meters[s->meter_count].beat_log2 = p[1] < 8 ? p[1] : 2;
				s->meter_count++;
			}
			else if (type == 0x21 && length == 1)
			{
				port = p[0];
				if (port < SMF_PORTS)
					s->ports |= (uint8_t)(1u << port);
			}
			else if (type == 0x03 && !s->name[0] && length)
			{
				take_title(s->name, sizeof(s->name), p, length);
				if (track == 0 && tick == 0)
				{
					memset(s->raw_name, ' ', sizeof(s->raw_name));
					memcpy(s->raw_name, p, length < sizeof(s->raw_name) ? length : sizeof(s->raw_name));
				}
			}
			else if (type == 0x01 && track == 0 && !text[0] && length)
				take_title(text, text_size, p, length);
			if (type == 0x2f)
			{
				if (tick > s->last_tick)
					s->last_tick = tick;
				break;
			}
			p += length;
		}
		else if (status == 0xf0 || status == 0xf7)
		{
			p++;
			uint32_t length = read_vlq(&p, end);
			if (length > (uint32_t)(end - p))
				length = (uint32_t)(end - p);
			smf_event_t *e = smf_add(s);
			if (!e)
				return 0;
			e->tick = tick;
			e->port = port;
			e->bytes = p;
			if (status == 0xf0)
			{
				e->status[0] = 0xf0;
				e->length = (uint16_t)(length + 1);
			}
			else
				e->length = (uint16_t)length;
			p += length;
		}
		else
		{
			if (status & 0x80)
			{
				running = status;
				p++;
			}
			else
				status = running;
			if (!status)
				return 0;
			int data = (status >= 0xc0 && status < 0xe0) ? 1 : 2;
			smf_event_t *e = smf_add(s);
			if (!e)
				return 0;
			e->tick = tick;
			e->port = port;
			e->status[0] = status;
			for (int n = 0; n < data && p < end; n++)
				e->status[1 + n] = *p++;
			e->length = (uint16_t)(1 + data);
		}
	}
	return 1;
}

static int compare_events(const void *a, const void *b)
{
	const smf_event_t *x = a, *y = b;
	if (x->tick != y->tick)
		return x->tick < y->tick ? -1 : 1;
	return x->order < y->order ? -1 : x->order > y->order;
}

static int compare_meters(const void *a, const void *b)
{
	const smf_meter_t *x = a, *y = b;
	return x->tick < y->tick ? -1 : x->tick > y->tick;
}

int smf_load(smf_t *s, const char *path, uint32_t rate)
{
	memset(s, 0, sizeof(*s));

	FILE *fp = fopen(path, "rb");
	if (!fp)
		return 0;
	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (size < 14)
	{
		fclose(fp);
		return 0;
	}
	s->data = malloc((size_t)size);
	if (!s->data || fread(s->data, 1, (size_t)size, fp) != (size_t)size)
	{
		fclose(fp);
		smf_free(s);
		return 0;
	}
	fclose(fp);

	const uint8_t *data = s->data;
	if (memcmp(data, "MThd", 4) != 0)
	{
		smf_free(s);
		return 0;
	}
	uint16_t tracks = read_be16(data + 10);
	s->division = read_be16(data + 12);
	if (s->division == 0 || (s->division & 0x8000))
	{
		smf_free(s);
		return 0;
	}
	char text[sizeof(s->name)];
	text[0] = 0;
	const uint8_t *p = data + 8 + read_be32(data + 4);
	for (uint16_t t = 0; t < tracks && p + 8 <= data + size; t++)
	{
		if (memcmp(p, "MTrk", 4) != 0)
			break;
		uint32_t length = read_be32(p + 4);
		const uint8_t *end = p + 8 + length;
		if (end > data + size || end < p)
			end = data + size;
		if (!parse_track(s, t, p + 8, end, text, sizeof(text)))
		{
			smf_free(s);
			return 0;
		}
		p = end;
	}
	if (!s->name[0] && text[0])
		memcpy(s->name, text, strlen(text) + 1);

	qsort(s->events, s->count, sizeof(smf_event_t), compare_events);
	if (s->meter_count)
		qsort(s->meters, s->meter_count, sizeof(smf_meter_t), compare_meters);

	/* the tempo map: one entry at the start, then one per tempo change, each with the seconds
	 * at which it takes hold; every event's frame comes off it */
	s->tempos = malloc((s->count + 1) * sizeof(smf_tempo_t));
	if (!s->tempos)
	{
		smf_free(s);
		return 0;
	}
	s->tempos[0].tick = 0;
	s->tempos[0].seconds = 0;
	s->tempos[0].tempo = 500000;
	s->tempo_count = 1;
	double seconds = 0;
	uint64_t last_tick = 0;
	uint32_t tempo = 500000;
	for (size_t n = 0; n < s->count; n++)
	{
		smf_event_t *e = &s->events[n];
		seconds += (double)(e->tick - last_tick) * tempo / 1e6 / s->division;
		last_tick = e->tick;
		e->frame = (uint32_t)(seconds * rate);
		if (e->tempo_change)
		{
			tempo = e->tempo;
			smf_tempo_t *t = &s->tempos[s->tempo_count - 1];
			if (t->tick != e->tick)
				t = &s->tempos[s->tempo_count++];
			t->tick = e->tick;
			t->seconds = seconds;
			t->tempo = tempo;
		}
		s->last_frame = e->frame;
	}
	if (s->last_tick < last_tick)
		s->last_tick = last_tick;
	return 1;
}

void smf_free(smf_t *s)
{
	free(s->events);
	free(s->data);
	free(s->meters);
	free(s->tempos);
	s->events = NULL;
	s->data = NULL;
	s->meters = NULL;
	s->tempos = NULL;
	s->count = s->capacity = 0;
	s->meter_count = s->tempo_count = 0;
}

/* ---------------------------------------------------------------- bars and the clock */

/* ticks in a bar of a time signature */
static uint64_t bar_ticks(const smf_t *s, const smf_meter_t *m)
{
	uint64_t t = (uint64_t)s->division * 4 * m->beats >> m->beat_log2;
	return t ? t : 1;
}

static const smf_meter_t common_time = { 0, 4, 2 };

/* the signature in force at a tick, and the bar number and tick at which its stretch begins */
static const smf_meter_t *meter_at(const smf_t *s, uint64_t tick, uint32_t *first_bar, uint64_t *from)
{
	const smf_meter_t *m = &common_time;
	uint32_t bar = 1;
	uint64_t start = 0;
	for (size_t n = 0; n < s->meter_count && s->meters[n].tick <= tick; n++)
	{
		const smf_meter_t *next = &s->meters[n];
		/* a change part way through a bar starts a new bar there */
		bar += (uint32_t)((next->tick - start + bar_ticks(s, m) - 1) / bar_ticks(s, m));
		start = next->tick;
		m = next;
	}
	*first_bar = bar;
	*from = start;
	return m;
}

uint32_t smf_bar_at_tick(const smf_t *s, uint64_t tick)
{
	uint32_t bar;
	uint64_t from;
	const smf_meter_t *m = meter_at(s, tick, &bar, &from);
	return bar + (uint32_t)((tick - from) / bar_ticks(s, m));
}

uint64_t smf_tick_of_bar(const smf_t *s, uint32_t bar)
{
	const smf_meter_t *m = &common_time;
	uint32_t first = 1;
	uint64_t start = 0;
	if (bar < 1)
		bar = 1;
	for (size_t n = 0; n < s->meter_count; n++)
	{
		const smf_meter_t *next = &s->meters[n];
		uint32_t at = first + (uint32_t)((next->tick - start + bar_ticks(s, m) - 1) / bar_ticks(s, m));
		if (at > bar)
			break;
		first = at;
		start = next->tick;
		m = next;
	}
	return start + (uint64_t)(bar - first) * bar_ticks(s, m);
}

static const smf_tempo_t *tempo_before_tick(const smf_t *s, uint64_t tick)
{
	size_t lo = 0, hi = s->tempo_count;
	while (hi - lo > 1)
	{
		size_t mid = lo + (hi - lo) / 2;
		if (s->tempos[mid].tick <= tick)
			lo = mid;
		else
			hi = mid;
	}
	return &s->tempos[lo];
}

uint32_t smf_tempo_at_tick(const smf_t *s, uint64_t tick)
{
	return s->tempo_count ? tempo_before_tick(s, tick)->tempo : 500000;
}

uint64_t smf_frame_at_tick(const smf_t *s, uint64_t tick, uint32_t rate)
{
	if (!s->tempo_count)
		return 0;
	const smf_tempo_t *t = tempo_before_tick(s, tick);
	double seconds = t->seconds + (double)(tick - t->tick) * t->tempo / 1e6 / s->division;
	return (uint64_t)(seconds * rate);
}

uint64_t smf_tick_at_frame(const smf_t *s, uint64_t frame, uint32_t rate)
{
	if (!s->tempo_count)
		return 0;
	double seconds = (double)frame / rate;
	size_t lo = 0, hi = s->tempo_count;
	while (hi - lo > 1)
	{
		size_t mid = lo + (hi - lo) / 2;
		if (s->tempos[mid].seconds <= seconds)
			lo = mid;
		else
			hi = mid;
	}
	const smf_tempo_t *t = &s->tempos[lo];
	return t->tick + (uint64_t)((seconds - t->seconds) * 1e6 / t->tempo * s->division);
}
