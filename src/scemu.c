#include <stdlib.h>
#include <string.h>
#include "scemu_internal.h"

static const char *g_create_error;

static const board_ops_t *board_ops_for(scemu_model_t model)
{
	switch (model)
	{
	case SCEMU_MODEL_SC88:
	case SCEMU_MODEL_SC88VL:
	case SCEMU_MODEL_SC88PRO:
	case SCEMU_MODEL_VEGSPRO:
		return &sc88_board_ops;
	case SCEMU_MODEL_SC8850:
		return &sc8850_board_ops;
	case SCEMU_MODEL_SC8820:
		return &sc8820_board_ops;
	case SCEMU_MODEL_SC55MK2:
		return &sc55mk2_board_ops;
	case SCEMU_MODEL_SC55:
		return &sc55_board_ops;
	default:
		return NULL;
	}
}

scemu_t *scemu_create(scemu_model_t model, const scemu_roms_t *roms, const scemu_config_t *config)
{
	const board_ops_t *ops = board_ops_for(model);
	if (!ops)
	{
		g_create_error = "unknown model";
		return NULL;
	}
	g_create_error = ops->validate_roms(model, roms);
	if (g_create_error)
		return NULL;

	scemu_t *m = calloc(1, sizeof(*m));
	if (!m)
	{
		g_create_error = "out of memory";
		return NULL;
	}
	m->model = model;
	m->ops = ops;
	if (config)
		m->config = *config;

	if (!ops->init(&m->board, model, roms, &m->config))
	{
		ops->release(&m->board);
		free(m);
		g_create_error = "machine initialisation failed";
		return NULL;
	}
	ops->reset(&m->board);
	return m;
}

void scemu_destroy(scemu_t *m)
{
	if (!m)
		return;
	m->ops->release(&m->board);
	free(m);
}

const char *scemu_error(const scemu_t *m)
{
	return m ? m->error : g_create_error;
}

scemu_model_t scemu_model(const scemu_t *m)
{
	return m->model;
}

uint32_t scemu_sample_rate(const scemu_t *m)
{
	return m->ops->sample_rate(&m->board);
}

int scemu_output_count(const scemu_t *m)
{
	return m->ops->output_count(&m->board);
}

void scemu_reset(scemu_t *m)
{
	m->ops->reset(&m->board);
}

uint64_t scemu_boot(scemu_t *m)
{
	const uint64_t start = m->ops->frame(&m->board);
	const uint64_t limit = start + 20 * m->ops->sample_rate(&m->board);
	do
		m->ops->run_frame(&m->board);
	while (!m->ops->idle(&m->board) && m->ops->frame(&m->board) < limit);
	return m->ops->frame(&m->board) - start;
}

bool scemu_muted(const scemu_t *m)
{
	return !m->ops->idle(&m->board);
}

void scemu_render(scemu_t *m, int32_t *const out[2], size_t frames)
{
	const board_ops_t *ops = m->ops;
	void *b = &m->board;
	const int pairs = ops->output_count(b);
	for (size_t n = 0; n < frames; n++)
	{
		ops->run_frame(b);
		for (int pair = 0; pair < pairs; pair++)
		{
			if (!out[pair])
				continue;
			out[pair][2 * n] = ops->output(b, pair, 0);
			out[pair][2 * n + 1] = ops->output(b, pair, 1);
		}
	}
}

void scemu_midi_write(scemu_t *m, int port, const uint8_t *bytes, size_t count, uint32_t frame_offset)
{
	midi_queue_t *q = m->ops->midi(&m->board);
	const uint32_t frame = (uint32_t)m->ops->frame(&m->board) + frame_offset;
	if (!m->map.map)
	{
		for (size_t n = 0; n < count; n++)
			midi_queue_push(q, port, bytes[n], frame);
		return;
	}
	uint8_t out[MIDI_MAP_MAX_OUT];
	for (size_t n = 0; n < count; n++)
	{
		size_t len = midi_map_filter(&m->map, port, bytes[n], out);
		for (size_t i = 0; i < len; i++)
			midi_queue_push(q, port, out[i], frame);
	}
}

static void send_preset(scemu_t *m)
{
	midi_queue_t *q = m->ops->midi(&m->board);
	const uint32_t frame = (uint32_t)m->ops->frame(&m->board);
	uint8_t out[MIDI_MAP_MAX_OUT];
	for (int port = 0; port < MIDI_MAP_PORTS; port++)
	{
		size_t len = midi_map_preset(&m->map, port, out);
		for (size_t i = 0; i < len; i++)
			midi_queue_push(q, port, out[i], frame);
	}
}

void scemu_set_midi_rate(scemu_t *m, uint32_t baud)
{
	m->ops->midi(&m->board)->baud = baud;
}

uint32_t scemu_midi_rate(const scemu_t *m)
{
	return m->ops->midi((void *)&m->board)->baud;
}

void scemu_set_map(scemu_t *m, scemu_map_t map)
{
	midi_map_reset(&m->map, (uint8_t)map);
	if (map != SCEMU_MAP_NATIVE)
		send_preset(m);
}

scemu_map_t scemu_map(const scemu_t *m)
{
	return (scemu_map_t)m->map.map;
}

size_t scemu_map_selection(scemu_map_t map, uint8_t *out, size_t size)
{
	uint8_t bytes[MIDI_MAP_MAX_OUT];
	midi_map_t f;
	if (map == SCEMU_MAP_NATIVE)
		return 0;
	midi_map_reset(&f, (uint8_t)map);
	const size_t n = midi_map_preset(&f, 0, bytes);
	if (n > size)
		return 0;
	memcpy(out, bytes, n);
	return n;
}

void scemu_set_midi_out(scemu_t *m, scemu_midi_out_fn fn, void *user)
{
	m->ops->set_midi_out(&m->board, fn, user);
}

void scemu_button(scemu_t *m, scemu_button_t button, bool down)
{
	m->ops->button(&m->board, button, down);
}

void scemu_set_computer_switch(scemu_t *m, scemu_computer_switch_t sw)
{
	m->ops->set_computer_switch(&m->board, sw);
}

uint32_t scemu_leds(const scemu_t *m)
{
	return m->ops->leds(&m->board);
}

const scemu_lcd_t *scemu_lcd(scemu_t *m)
{
	return m->ops->lcd ? m->ops->lcd(&m->board) : NULL;
}

void scemu_lcd_ack(scemu_t *m)
{
	scemu_lcd_t *lcd = m->ops->lcd ? m->ops->lcd(&m->board) : NULL;
	if (lcd)
		lcd->changed = false;
}

const scemu_glcd_t *scemu_glcd(scemu_t *m)
{
	return m->ops->glcd ? m->ops->glcd(&m->board) : NULL;
}

void scemu_glcd_ack(scemu_t *m)
{
	scemu_glcd_t *glcd = m->ops->glcd ? m->ops->glcd(&m->board) : NULL;
	if (glcd)
		glcd->changed = false;
}

void scemu_dial(scemu_t *m, int steps)
{
	if (m->ops->dial)
		m->ops->dial(&m->board, steps);
}

/* what the board and its chips put in a state, asked for afresh each time, so the registration
   never outlives the machine it points into */
static state_registry_t *registry(const scemu_t *m)
{
	state_registry_t *reg = malloc(sizeof(*reg));
	if (!reg)
		return NULL;
	state_begin(reg, (uint32_t)m->model, m->ops->rom_id(&m->board), m->ops->frame(&m->board));
	m->ops->state_register((void *)&m->board, reg);
	return reg;
}

size_t scemu_state_size(const scemu_t *m)
{
	state_registry_t *reg = registry(m);
	if (!reg)
		return 0;
	const size_t size = state_size(reg);
	free(reg);
	return size;
}

size_t scemu_state_save(const scemu_t *m, void *buffer, size_t size)
{
	state_registry_t *reg = registry(m);
	if (!reg)
		return 0;
	const size_t written = state_save(reg, buffer, size);
	free(reg);
	return written;
}

bool scemu_state_load(scemu_t *m, const void *buffer, size_t size)
{
	state_registry_t *reg = registry(m);
	if (!reg)
		return false;
	const bool ok = state_load(reg, buffer, size);
	free(reg);
	if (!ok)
		return false;
	scemu_set_map(m, (scemu_map_t)m->map.map);
	return true;
}

bool scemu_state_info(const void *buffer, size_t size, scemu_model_t *model, uint64_t *rom_id, uint64_t *frame)
{
	return state_info(buffer, size, model, rom_id, frame);
}

uint64_t scemu_rom_id(const scemu_t *m)
{
	return m->ops->rom_id(&m->board);
}

size_t scemu_nvram_size(const scemu_t *m)
{
	return m->ops->nvram_size(&m->board);
}

size_t scemu_nvram_get(const scemu_t *m, void *buffer, size_t size)
{
	return m->ops->nvram_get(&m->board, buffer, size);
}

bool scemu_nvram_set(scemu_t *m, const void *buffer, size_t size)
{
	return m->ops->nvram_set(&m->board, buffer, size);
}
