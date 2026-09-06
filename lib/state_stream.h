#ifndef SCEMU_STATE_STREAM_H
#define SCEMU_STATE_STREAM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "scemu.h"

/* The state stream every board writes: a header naming the model and the ROM set, then tagged
   chunks, every field little-endian at a fixed width, so a state does not depend on the host's
   layout, endianness or build.  Unknown chunks are skipped, missing ones refused. */

#define STATE_MAGIC "SCEMUST1"
#define STATE_HEADER_SIZE 36

#define STATE_TAG(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

typedef struct state_writer
{
	uint8_t *p;
	size_t size;
} state_writer_t;

static inline void put8(state_writer_t *w, uint8_t v)
{
	if (w->p)
		w->p[w->size] = v;
	w->size++;
}

static inline void put16(state_writer_t *w, uint16_t v) { put8(w, (uint8_t)v); put8(w, (uint8_t)(v >> 8)); }
static inline void put32(state_writer_t *w, uint32_t v) { put16(w, (uint16_t)v); put16(w, (uint16_t)(v >> 16)); }
static inline void put64(state_writer_t *w, uint64_t v) { put32(w, (uint32_t)v); put32(w, (uint32_t)(v >> 32)); }
static inline void put_i32(state_writer_t *w, int32_t v) { put32(w, (uint32_t)v); }
static inline void put_bool(state_writer_t *w, bool v) { put8(w, v ? 1 : 0); }

static inline void put_bytes(state_writer_t *w, const void *data, size_t n)
{
	if (w->p)
		memcpy(w->p + w->size, data, n);
	w->size += n;
}

static inline void put_u16s(state_writer_t *w, const uint16_t *v, size_t n) { for (size_t i = 0; i < n; i++) put16(w, v[i]); }
static inline void put_u32s(state_writer_t *w, const uint32_t *v, size_t n) { for (size_t i = 0; i < n; i++) put32(w, v[i]); }
static inline void put_i32s(state_writer_t *w, const int32_t *v, size_t n) { for (size_t i = 0; i < n; i++) put_i32(w, v[i]); }

typedef struct state_reader
{
	const uint8_t *p;
	size_t size, at;
	bool ok;
} state_reader_t;

static inline uint8_t get8(state_reader_t *r)
{
	if (r->at >= r->size)
	{
		r->ok = false;
		return 0;
	}
	return r->p[r->at++];
}

static inline uint16_t get16(state_reader_t *r) { uint16_t lo = get8(r); return (uint16_t)(lo | (get8(r) << 8)); }
static inline uint32_t get32(state_reader_t *r) { uint32_t lo = get16(r); return lo | ((uint32_t)get16(r) << 16); }
static inline uint64_t get64(state_reader_t *r) { uint64_t lo = get32(r); return lo | ((uint64_t)get32(r) << 32); }
static inline int32_t get_i32(state_reader_t *r) { return (int32_t)get32(r); }
static inline bool get_bool(state_reader_t *r) { return get8(r) != 0; }

static inline void get_bytes(state_reader_t *r, void *data, size_t n)
{
	if (r->at + n > r->size)
	{
		r->ok = false;
		memset(data, 0, n);
		r->at = r->size;
		return;
	}
	memcpy(data, r->p + r->at, n);
	r->at += n;
}

static inline void get_u16s(state_reader_t *r, uint16_t *v, size_t n) { for (size_t i = 0; i < n; i++) v[i] = get16(r); }
static inline void get_u32s(state_reader_t *r, uint32_t *v, size_t n) { for (size_t i = 0; i < n; i++) v[i] = get32(r); }
static inline void get_i32s(state_reader_t *r, int32_t *v, size_t n) { for (size_t i = 0; i < n; i++) v[i] = get_i32(r); }

typedef struct state_header
{
	uint32_t model, flags, chunk_count;
	uint64_t rom_id, frame;
} state_header_t;

static inline void put_header(state_writer_t *w, const state_header_t *h)
{
	put_bytes(w, STATE_MAGIC, 8);
	put32(w, h->model);
	put32(w, h->flags);
	put64(w, h->rom_id);
	put64(w, h->frame);
	put32(w, h->chunk_count);
}

static inline bool get_header(state_reader_t *r, state_header_t *h)
{
	char magic[8];
	get_bytes(r, magic, 8);
	h->model = get32(r);
	h->flags = get32(r);
	h->rom_id = get64(r);
	h->frame = get64(r);
	h->chunk_count = get32(r);
	return r->ok && memcmp(magic, STATE_MAGIC, 8) == 0;
}

static inline void put_chunk(state_writer_t *w, uint32_t tag, const state_writer_t *body)
{
	put32(w, tag);
	put32(w, (uint32_t)body->size);
	put_bytes(w, body->p, body->size);
}

/* what a stream is for, from its header alone */
static inline bool state_info(const void *buffer, size_t size, scemu_model_t *model, uint64_t *rom_id, uint64_t *frame)
{
	state_reader_t r = { buffer, size, 0, true };
	state_header_t h;
	if (!get_header(&r, &h) || h.model >= SCEMU_MODEL_COUNT)
		return false;
	if (model)
		*model = (scemu_model_t)h.model;
	if (rom_id)
		*rom_id = h.rom_id;
	if (frame)
		*frame = h.frame;
	return true;
}

#endif
