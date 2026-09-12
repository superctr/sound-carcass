#ifndef SCEMU_STATE_H
#define SCEMU_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "scemu.h"

/* The state stream: a header naming the model and the ROM set, then tagged chunks, every field
   little-endian at a fixed width, so a state does not depend on the host's layout, endianness or
   build.  A board registers its chunks and every chip registers its own items into them, and the
   writer and the reader walk that registration; a chip whose state is not an array of cells — a
   ring saved from its head — registers a pair of callbacks instead.  Unknown chunks are skipped,
   missing ones refused. */

#define STATE_MAGIC "SCEMUST2"

#define STATE_TAG(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* ---------------------------------------------------------------- the stream */

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

static inline void put_bytes(state_writer_t *w, const void *data, size_t n)
{
	if (w->p)
		memcpy(w->p + w->size, data, n);
	w->size += n;
}

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

/* ---------------------------------------------------------------- what is registered */

enum { STATE_PLAIN, STATE_BOOL, STATE_CUSTOM };

typedef void (*state_put_fn)(state_writer_t *w, void *user);
typedef void (*state_get_fn)(state_reader_t *r, void *user);

/* `count` cells of `elem` bytes, `stride` apart, so one field of an array of structs is one item */
typedef struct state_item
{
	void *p;
	uint32_t elem, count, stride;
	uint8_t kind;
	state_put_fn put;
	state_get_fn get;
	void *user;
} state_item_t;

typedef struct state_chunk
{
	uint32_t tag;
	uint32_t first, count;
} state_chunk_t;

#define STATE_MAX_CHUNKS 32
#define STATE_MAX_ITEMS 1024
#define STATE_MAX_RESTORE 24

typedef struct state_restore
{
	bool (*fn)(void *user);
	void *user;
} state_restore_t;

typedef struct state_registry
{
	uint32_t model, flags;
	uint64_t rom_id, frame;
	state_chunk_t chunks[STATE_MAX_CHUNKS];
	uint32_t chunk_count;
	state_item_t items[STATE_MAX_ITEMS];
	uint32_t item_count;
	state_restore_t restore[STATE_MAX_RESTORE];
	uint32_t restore_count;
	bool overflow;
} state_registry_t;

static inline void state_begin(state_registry_t *reg, uint32_t model, uint64_t rom_id, uint64_t frame)
{
	memset(reg, 0, sizeof(*reg));
	reg->model = model;
	reg->rom_id = rom_id;
	reg->frame = frame;
}

/* a board with a part the model alone does not name sets a flag a load has to match */
static inline void state_flags(state_registry_t *reg, uint32_t flags)
{
	reg->flags |= flags;
}

/* the chunk the items that follow belong to */
static inline void state_chunk(state_registry_t *reg, const char *tag)
{
	if (reg->chunk_count >= STATE_MAX_CHUNKS)
	{
		reg->overflow = true;
		return;
	}
	state_chunk_t *c = &reg->chunks[reg->chunk_count++];
	c->tag = STATE_TAG(tag[0], tag[1], tag[2], tag[3]);
	c->first = reg->item_count;
	c->count = 0;
}

static inline state_item_t *state_add(state_registry_t *reg)
{
	if (reg->item_count >= STATE_MAX_ITEMS || reg->chunk_count == 0)
	{
		reg->overflow = true;
		return NULL;
	}
	reg->chunks[reg->chunk_count - 1].count++;
	state_item_t *it = &reg->items[reg->item_count++];
	memset(it, 0, sizeof(*it));
	return it;
}

static inline void state_cells(state_registry_t *reg, void *p, size_t elem, size_t count, size_t stride, int kind)
{
	state_item_t *it = state_add(reg);
	if (!it)
		return;
	if (elem != 1 && elem != 2 && elem != 4 && elem != 8)
	{
		reg->overflow = true;
		return;
	}
	it->p = p;
	it->elem = (uint32_t)elem;
	it->count = (uint32_t)count;
	it->stride = (uint32_t)stride;
	it->kind = (uint8_t)kind;
}

static inline void state_custom(state_registry_t *reg, state_put_fn put, state_get_fn get, void *user)
{
	state_item_t *it = state_add(reg);
	if (!it)
		return;
	it->kind = STATE_CUSTOM;
	it->put = put;
	it->get = get;
	it->user = user;
}

/* run once every chunk is read, in registration order; false refuses the stream */
static inline void state_after_load(state_registry_t *reg, bool (*fn)(void *user), void *user)
{
	if (reg->restore_count >= STATE_MAX_RESTORE)
	{
		reg->overflow = true;
		return;
	}
	reg->restore[reg->restore_count].fn = fn;
	reg->restore[reg->restore_count].user = user;
	reg->restore_count++;
}

/* one cell, a whole array, `n` cells behind a pointer, one field of `n` structs, one bool */
#define state_var(reg, x)            state_cells((reg), &(x), sizeof(x), 1, sizeof(x), STATE_PLAIN)
#define state_array(reg, a)          state_cells((reg), (a), sizeof((a)[0]), sizeof(a) / sizeof((a)[0]), sizeof((a)[0]), STATE_PLAIN)
#define state_block(reg, p, n)       state_cells((reg), (p), sizeof((p)[0]), (n), sizeof((p)[0]), STATE_PLAIN)
#define state_field(reg, a, n, f)    state_cells((reg), &(a)[0].f, sizeof((a)[0].f), (n), sizeof((a)[0]), STATE_PLAIN)
#define state_bool(reg, x)           state_cells((reg), &(x), 1, 1, 1, STATE_BOOL)
#define state_bool_field(reg, a, n, f) state_cells((reg), &(a)[0].f, 1, (n), sizeof((a)[0]), STATE_BOOL)

/* ---------------------------------------------------------------- the whole stream */

size_t state_size(const state_registry_t *reg);
size_t state_save(const state_registry_t *reg, void *buffer, size_t size);
bool state_load(state_registry_t *reg, const void *buffer, size_t size);
/* what a stream is for, from its header alone */
bool state_info(const void *buffer, size_t size, scemu_model_t *model, uint64_t *rom_id, uint64_t *frame);

#endif
