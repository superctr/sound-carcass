/* scplay: Standard MIDI File reader, type 0 and 1, with the tempo map resolved
 * to frames of the machine's own sample clock; and a writer for what the
 * machine's inputs took.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdbool.h>
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

static smf_event_t *smf_add(smf_t *s, unsigned track)
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
	e->track = (uint16_t)track;
	return e;
}

static int parse_track(smf_t *s, unsigned track, const uint8_t *p, const uint8_t *end, char *text, size_t text_size)
{
	uint64_t tick = 0;
	uint8_t running = 0;
	uint8_t port = SMF_PORT_UNSET;
	const uint8_t *start = p;
	while (p < end)
	{
		size_t had = s->count;
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
				smf_event_t *e = smf_add(s, track);
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
			smf_event_t *e = smf_add(s, track);
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
			smf_event_t *e = smf_add(s, track);
			if (!e)
				return 0;
			e->tick = tick;
			e->port = port;
			e->status[0] = status;
			for (int n = 0; n < data && p < end; n++)
				e->status[1 + n] = *p++;
			e->length = (uint16_t)(1 + data);
		}
		if (s->count > had)
			s->events[s->count - 1].offset = (uint32_t)(p - start);
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
		s->tracks = (uint16_t)(t + 1);
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

/* ---------------------------------------------------------------- the writer */

#define TAKE_DIVISION 480

typedef struct bytes
{
	uint8_t *p;
	size_t used, cap;
	uint64_t tick;            /* of the last event written, for the deltas */
	bool failed;
} bytes_t;

static void put(bytes_t *b, const uint8_t *p, size_t n)
{
	if (b->failed)
		return;
	if (b->used + n > b->cap)
	{
		size_t want = b->cap ? b->cap * 2 : 1024;
		while (want < b->used + n)
			want *= 2;
		uint8_t *grown = realloc(b->p, want);
		if (!grown)
		{
			b->failed = true;
			return;
		}
		b->p = grown;
		b->cap = want;
	}
	memcpy(b->p + b->used, p, n);
	b->used += n;
}

static void put_byte(bytes_t *b, uint8_t v) { put(b, &v, 1); }

static void put_be32(bytes_t *b, uint32_t v)
{
	uint8_t p[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
	put(b, p, 4);
}

static void put_vlq(bytes_t *b, uint64_t v)
{
	uint8_t p[10];
	int n = 0;
	p[n++] = v & 0x7f;
	for (v >>= 7; v; v >>= 7)
		p[n++] = 0x80 | (v & 0x7f);
	while (n)
		put_byte(b, p[--n]);
}

/* a delta time up to a tick, which never goes backwards */
static void put_delta(bytes_t *b, uint64_t tick)
{
	put_vlq(b, tick > b->tick ? tick - b->tick : 0);
	if (tick > b->tick)
		b->tick = tick;
}

static uint64_t tick_of_frame(uint64_t frame, uint32_t rate, int tempo)
{
	uint64_t per_minute = (uint64_t)rate * 60;
	return rate ? (frame * TAKE_DIVISION * (uint64_t)tempo + per_minute / 2) / per_minute : 0;
}

/* the data bytes a status byte takes; -1 for one the file cannot hold */
static int data_bytes(uint8_t status)
{
	switch (status & 0xf0)
	{
	case 0xc0: case 0xd0: return 1;
	case 0xf0:
		switch (status)
		{
		case 0xf1: case 0xf3: return 1;
		case 0xf2: return 2;
		default: return -1;
		}
	default: return 2;
	}
}

/* a chunk's messages onto a track at a tick, whole ones only: running status is written out,
 * real-time bytes and system common messages are left out */
static void put_chunk(bytes_t *t, uint64_t tick, const uint8_t *p, size_t n)
{
	uint8_t running = 0;
	size_t i = 0;
	while (i < n)
	{
		uint8_t c = p[i];
		if (c >= 0xf8)
		{
			i++;
			continue;
		}
		if (c == 0xf0)
		{
			size_t end = i + 1;
			while (end < n && p[end] != 0xf7)
				end++;
			put_delta(t, tick);
			put_byte(t, 0xf0);
			put_vlq(t, end - i);   /* the bytes after F0, the F7 included */
			put(t, p + i + 1, end - i - 1);
			put_byte(t, 0xf7);
			i = end < n ? end + 1 : n;
			running = 0;
			continue;
		}
		uint8_t status;
		if (c & 0x80)
		{
			status = c;
			i++;
			if (status < 0xf0)
				running = status;
		}
		else if (running)
			status = running;
		else
		{
			i++;
			continue;
		}
		int want = data_bytes(status);
		if (want < 0 || i + (size_t)want > n)
		{
			if (want < 0)
				continue;
			break;
		}
		put_delta(t, tick);
		put_byte(t, status);
		put(t, p + i, (size_t)want);
		i += (size_t)want;
	}
}

static void put_track(bytes_t *file, const bytes_t *track)
{
	static const uint8_t head[4] = { 'M', 'T', 'r', 'k' };
	put(file, head, 4);
	put_be32(file, (uint32_t)track->used);
	put(file, track->p, track->used);
}

uint8_t *smf_write_takes(const smf_take_t *takes, size_t count, const uint8_t *bytes, uint64_t frames, uint32_t rate,
                         int tempo, size_t *size)
{
	if (tempo < 1)
		tempo = 120;
	bytes_t track[SMF_PORTS];
	memset(track, 0, sizeof(track));
	for (size_t n = 0; n < count; n++)
	{
		const smf_take_t *k = &takes[n];
		if (k->port >= SMF_PORTS)
			continue;
		bytes_t *t = &track[k->port];
		if (!t->used)
		{
			static const uint8_t port[4] = { 0x00, 0xff, 0x21, 0x01 };
			put(t, port, 4);
			put_byte(t, k->port);
		}
		put_chunk(t, tick_of_frame(k->frame, rate, tempo), bytes + k->at, k->length);
	}
	uint64_t end = tick_of_frame(frames, rate, tempo);
	int tracks = 0;
	for (int n = 0; n < SMF_PORTS; n++)
		if (track[n].used > 5)   /* more than its port event */
			tracks++;
	bytes_t file;
	memset(&file, 0, sizeof(file));
	if (tracks)
	{
		static const uint8_t head[8] = { 'M', 'T', 'h', 'd', 0, 0, 0, 6 };
		uint32_t quarter = 60000000u / (uint32_t)tempo;   /* microseconds */
		const uint8_t conductor[15] = {
			0x00, 0xff, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08,   /* 4/4 */
			0x00, 0xff, 0x51, 0x03, (uint8_t)(quarter >> 16), (uint8_t)(quarter >> 8), (uint8_t)quarter
		};
		static const uint8_t finish[3] = { 0xff, 0x2f, 0x00 };
		put(&file, head, 8);
		put_byte(&file, 0);
		put_byte(&file, 1);
		put_byte(&file, 0);
		put_byte(&file, (uint8_t)(tracks + 1));
		put_byte(&file, TAKE_DIVISION >> 8);
		put_byte(&file, TAKE_DIVISION & 0xff);
		bytes_t lead;
		memset(&lead, 0, sizeof(lead));
		put(&lead, conductor, sizeof(conductor));
		put_delta(&lead, end);
		put(&lead, finish, 3);
		put_track(&file, &lead);
		free(lead.p);
		for (int n = 0; n < SMF_PORTS; n++)
			if (track[n].used > 5)
			{
				put_delta(&track[n], end);
				put(&track[n], finish, 3);
				put_track(&file, &track[n]);
			}
	}
	for (int n = 0; n < SMF_PORTS; n++)
		free(track[n].p);
	if (!tracks || file.failed)
	{
		free(file.p);
		*size = 0;
		return NULL;
	}
	*size = file.used;
	return file.p;
}
