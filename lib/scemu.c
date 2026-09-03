#include <stdlib.h>
#include <string.h>
#include "scemu_internal.h"

static const char *g_create_error;

scemu_t *scemu_create(scemu_model_t model, const scemu_roms_t *roms, const scemu_config_t *config)
{
	g_create_error = sc88_validate_roms(model, roms);
	if (g_create_error)
		return NULL;

	scemu_t *m = calloc(1, sizeof(*m));
	if (!m)
	{
		g_create_error = "out of memory";
		return NULL;
	}
	m->model = model;
	if (config)
		m->config = *config;

	if (!sc88_init(&m->machine, model, roms, &m->config))
	{
		sc88_release(&m->machine);
		free(m);
		g_create_error = "machine initialisation failed";
		return NULL;
	}
	sc88_reset(&m->machine);
	return m;
}

void scemu_destroy(scemu_t *m)
{
	if (!m)
		return;
	sc88_release(&m->machine);
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
	(void)m;
	return SC88_SAMPLE_RATE;
}

int scemu_output_count(const scemu_t *m)
{
	return m->machine.has_lsp ? 2 : 1;
}

void scemu_reset(scemu_t *m)
{
	sc88_reset(&m->machine);
}

uint64_t scemu_boot(scemu_t *m)
{
	uint64_t start = m->machine.frame;
	uint64_t limit = start + 20 * SC88_SAMPLE_RATE;
	do
		sc88_run_frame(&m->machine);
	while (!sc88_idle(&m->machine) && m->machine.frame < limit);
	return m->machine.frame - start;
}

bool scemu_muted(const scemu_t *m)
{
	return !sc88_idle(&m->machine);
}

void scemu_render(scemu_t *m, int32_t *const out[2], size_t frames)
{
	sc88_t *b = &m->machine;
	for (size_t n = 0; n < frames; n++)
	{
		sc88_run_frame(b);
		if (out[0])
		{
			out[0][2 * n] = b->mute ? 0 : xp_output(&b->xp, b->has_lsp ? 2 : 1);
			out[0][2 * n + 1] = b->mute ? 0 : xp_output(&b->xp, b->has_lsp ? 3 : 4);
		}
		if (out[1] && b->has_lsp)
		{
			out[1][2 * n] = b->mute ? 0 : xp_output(&b->xp, 6);
			out[1][2 * n + 1] = b->mute ? 0 : xp_output(&b->xp, 7);
		}
	}
}

void scemu_midi_write(scemu_t *m, int port, const uint8_t *bytes, size_t count, uint32_t frame_offset)
{
	if (!m->map.map)
	{
		for (size_t n = 0; n < count; n++)
			sc88_queue_midi(&m->machine, port, bytes[n], frame_offset);
		return;
	}
	uint8_t out[MIDI_MAP_MAX_OUT];
	for (size_t n = 0; n < count; n++)
	{
		size_t len = midi_map_filter(&m->map, port, bytes[n], out);
		for (size_t i = 0; i < len; i++)
			sc88_queue_midi(&m->machine, port, out[i], frame_offset);
	}
}

static void send_preset(scemu_t *m)
{
	uint8_t out[MIDI_MAP_MAX_OUT];
	for (int port = 0; port < MIDI_MAP_PORTS; port++)
	{
		size_t len = midi_map_preset(&m->map, port, out);
		for (size_t i = 0; i < len; i++)
			sc88_queue_midi(&m->machine, port, out[i], 0);
	}
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

void scemu_set_midi_out(scemu_t *m, scemu_midi_out_fn fn, void *user)
{
	m->machine.midi_out = fn;
	m->machine.midi_out_user = user;
}

void scemu_button(scemu_t *m, scemu_button_t button, bool down)
{
	sc88_button(&m->machine, button, down);
}

void scemu_set_computer_switch(scemu_t *m, scemu_computer_switch_t sw)
{
	m->machine.computer_switch = sw;
}

uint32_t scemu_leds(const scemu_t *m)
{
	return m->machine.ga.leds;
}

const scemu_lcd_t *scemu_lcd(scemu_t *m)
{
	return &m->machine.lcd.out;
}

void scemu_lcd_ack(scemu_t *m)
{
	m->machine.lcd.out.changed = false;
}

size_t scemu_state_size(const scemu_t *m)
{
	return sc88_state_size(&m->machine);
}

size_t scemu_state_save(const scemu_t *m, void *buffer, size_t size)
{
	return sc88_state_save(&m->machine, buffer, size);
}

bool scemu_state_load(scemu_t *m, const void *buffer, size_t size)
{
	if (!sc88_state_load(&m->machine, buffer, size))
		return false;
	scemu_set_map(m, (scemu_map_t)m->map.map);
	return true;
}

size_t scemu_nvram_size(const scemu_t *m)
{
	(void)m;
	return SC88_SRAM_SIZE;
}

size_t scemu_nvram_get(const scemu_t *m, void *buffer, size_t size)
{
	if (size < SC88_SRAM_SIZE)
		return 0;
	memcpy(buffer, m->machine.sram, SC88_SRAM_SIZE);
	return SC88_SRAM_SIZE;
}

bool scemu_nvram_set(scemu_t *m, const void *buffer, size_t size)
{
	if (size != SC88_SRAM_SIZE)
		return false;
	memcpy(m->machine.sram, buffer, SC88_SRAM_SIZE);
	return true;
}
