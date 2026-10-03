#include <stdlib.h>
#include <string.h>
#include "state.h"

/* ---------------------------------------------------------------- one item */

static void put_item(state_writer_t *w, const state_item_t *it)
{
	if (it->kind == STATE_CUSTOM)
	{
		it->put(w, it->user);
		return;
	}
	const uint8_t *p = it->p;
	if (it->elem == 1 && it->stride == 1)
	{
		if (it->kind == STATE_BOOL)
			for (uint32_t n = 0; n < it->count; n++)
				put8(w, p[n] ? 1 : 0);
		else
			put_bytes(w, p, it->count);
		return;
	}
	for (uint32_t n = 0; n < it->count; n++, p += it->stride)
	{
		uint64_t v = 0;
		memcpy(&v, p, it->elem);
		switch (it->elem)
		{
		case 1: put8(w, it->kind == STATE_BOOL ? (v ? 1 : 0) : (uint8_t)v); break;
		case 2: put16(w, (uint16_t)v); break;
		case 4: put32(w, (uint32_t)v); break;
		default: put64(w, v); break;
		}
	}
}

static void get_item(state_reader_t *r, const state_item_t *it)
{
	if (it->kind == STATE_CUSTOM)
	{
		it->get(r, it->user);
		return;
	}
	uint8_t *p = it->p;
	if (it->kind != STATE_BOOL && it->elem == 1 && it->stride == 1)
	{
		get_bytes(r, p, it->count);
		return;
	}
	for (uint32_t n = 0; n < it->count; n++, p += it->stride)
	{
		if (it->kind == STATE_BOOL)
		{
			*(bool *)p = get8(r) != 0;
			continue;
		}
		uint64_t v;
		switch (it->elem)
		{
		case 1: v = get8(r); break;
		case 2: v = get16(r); break;
		case 4: v = get32(r); break;
		default: v = get64(r); break;
		}
		memcpy(p, &v, it->elem);
	}
}

/* ---------------------------------------------------------------- the whole */

static size_t write_state(const state_registry_t *reg, uint8_t *out)
{
	state_writer_t w = { out, 0 };
	put_bytes(&w, STATE_MAGIC, 8);
	put32(&w, reg->model);
	put32(&w, reg->flags);
	put64(&w, reg->rom_id);
	put64(&w, reg->frame);
	put32(&w, reg->chunk_count);

	for (uint32_t n = 0; n < reg->chunk_count; n++)
	{
		const state_chunk_t *c = &reg->chunks[n];
		put32(&w, c->tag);
		const size_t size_at = w.size;
		put32(&w, 0);
		const size_t start = w.size;
		for (uint32_t k = 0; k < c->count; k++)
			put_item(&w, &reg->items[c->first + k]);
		if (out)
		{
			const uint32_t size = (uint32_t)(w.size - start);
			out[size_at] = (uint8_t)size;
			out[size_at + 1] = (uint8_t)(size >> 8);
			out[size_at + 2] = (uint8_t)(size >> 16);
			out[size_at + 3] = (uint8_t)(size >> 24);
		}
	}
	return w.size;
}

size_t state_size(const state_registry_t *reg)
{
	if (reg->overflow)
		return 0;
	return write_state(reg, NULL);
}

size_t state_save(const state_registry_t *reg, void *buffer, size_t size)
{
	const size_t need = state_size(reg);
	if (!need || size < need)
		return 0;
	return write_state(reg, buffer);
}

static bool get_header(state_reader_t *r, uint32_t *model, uint32_t *flags, uint64_t *rom_id, uint64_t *frame,
                       uint32_t *chunk_count)
{
	char magic[8];
	get_bytes(r, magic, 8);
	*model = get32(r);
	*flags = get32(r);
	*rom_id = get64(r);
	*frame = get64(r);
	*chunk_count = get32(r);
	return r->ok && memcmp(magic, STATE_MAGIC, 8) == 0;
}

/* the chunks the registry knows, read into the machine; a chunk it does not know is skipped */
static bool read_chunks(state_registry_t *reg, const void *buffer, size_t size)
{
	state_reader_t r = { buffer, size, 0, true };
	uint32_t model, flags, chunk_count;
	uint64_t rom_id, frame;
	if (!get_header(&r, &model, &flags, &rom_id, &frame, &chunk_count))
		return false;

	uint32_t seen = 0;
	for (uint32_t n = 0; n < chunk_count; n++)
	{
		const uint32_t tag = get32(&r);
		const uint32_t len = get32(&r);
		if (!r.ok || len > r.size - r.at)
			return false;
		for (uint32_t k = 0; k < reg->chunk_count; k++)
		{
			if (reg->chunks[k].tag != tag)
				continue;
			state_reader_t body = { r.p + r.at, len, 0, true };
			const state_chunk_t *c = &reg->chunks[k];
			for (uint32_t i = 0; i < c->count; i++)
				get_item(&body, &reg->items[c->first + i]);
			if (!body.ok || body.at != body.size)
				return false;
			seen |= 1u << k;
		}
		r.at += len;
	}
	const uint32_t need = reg->chunk_count == 32 ? ~0u : (1u << reg->chunk_count) - 1;
	if ((seen & need) != need)
		return false;

	for (uint32_t n = 0; n < reg->restore_count; n++)
		if (!reg->restore[n].fn(reg->restore[n].user))
			return false;
	return true;
}

bool state_load(state_registry_t *reg, const void *buffer, size_t size)
{
	state_reader_t r = { buffer, size, 0, true };
	uint32_t model, flags, chunk_count;
	uint64_t rom_id, frame;
	if (reg->overflow || !get_header(&r, &model, &flags, &rom_id, &frame, &chunk_count)
	    || model != reg->model || flags != reg->flags || rom_id != reg->rom_id)
		return false;

	/* the machine's own state first, so a stream that turns out to be bad leaves it as it was */
	const size_t need = state_size(reg);
	uint8_t *snapshot = malloc(need);
	if (!snapshot || state_save(reg, snapshot, need) != need)
	{
		free(snapshot);
		return false;
	}

	const bool ok = read_chunks(reg, buffer, size);
	if (!ok)
		read_chunks(reg, snapshot, need);
	free(snapshot);
	return ok;
}

bool state_info(const void *buffer, size_t size, scemu_model_t *model, uint64_t *rom_id, uint64_t *frame)
{
	state_reader_t r = { buffer, size, 0, true };
	uint32_t m, flags, chunk_count;
	uint64_t id, at;
	if (!get_header(&r, &m, &flags, &id, &at, &chunk_count) || m >= SCEMU_MODEL_COUNT)
		return false;
	if (model)
		*model = (scemu_model_t)m;
	if (rom_id)
		*rom_id = id;
	if (frame)
		*frame = at;
	return true;
}
