#include <stdlib.h>
#include <string.h>
#include "sc88.h"
#include "wave_rom.h"

typedef struct rom_layout
{
	size_t program_size;
	bool program_word_swapped;
	int wave_count;
	size_t wave_size;
} rom_layout_t;

static const rom_layout_t LAYOUT[SCEMU_MODEL_COUNT] =
{
	[SCEMU_MODEL_SC88]    = { 0x080000, false, 4, 0x200000 },
	[SCEMU_MODEL_SC88VL]  = { 0x080000, true,  4, 0x200000 },
	[SCEMU_MODEL_SC88PRO] = { 0x100000, false, 5, 0x400000 },
	[SCEMU_MODEL_VEGSPRO] = { 0x100000, true,  3, 0 },
};

static const size_t VEGSPRO_WAVE[3] = { 0x800000, 0x800000, 0x400000 };

static const sc88_map_t MAP_SC88 = { 0x080000, { 0x008000, 0x0e8000 }, 0x0e0000, 0x0f0000, 0x0fc100, 0 };
static const sc88_map_t MAP_PRO = { 0xc00000, { 0, 0 }, 0xc80000, 0xe00000, 0xefc100, 0xf00000 };

const char *sc88_validate_roms(scemu_model_t model, const scemu_roms_t *roms)
{
	if (model < 0 || model >= SCEMU_MODEL_COUNT)
		return "unknown model";
	const rom_layout_t *l = &LAYOUT[model];
	if (!roms || !roms->program_rom)
		return "no program ROM";
	if (roms->program_rom_size != l->program_size)
		return "program ROM has the wrong size";
	if (roms->wave_rom_count != l->wave_count)
		return "wrong number of wave ROMs";
	for (int n = 0; n < l->wave_count; n++)
	{
		size_t want = l->wave_size ? l->wave_size : VEGSPRO_WAVE[n];
		if (!roms->wave_rom[n])
			return "missing wave ROM";
		if (roms->wave_rom_size[n] != want)
			return "wave ROM has the wrong size";
	}
	return NULL;
}

/* ---------------------------------------------------------------- wiring */

static void ga_irq(void *user, bool state)
{
	sc88_t *b = user;
	h8500_set_irq(&b->cpu, H8500_IRQ0, state);
}

static void xp_irq(void *user, bool state)
{
	sc88_t *b = user;
	b->xp_int = state;
	h8500_set_irq(&b->cpu, H8500_IRQ1, state);
}

static void sub_irq(void *user, bool state)
{
	sc88_t *b = user;
	h8500_set_irq(&b->cpu, H8500_IRQ2, state);
}

static void sub_midi_out(void *user, uint8_t byte)
{
	sc88_t *b = user;
	if (b->midi_out)
		b->midi_out(&byte, 1, b->midi_out_user);
}

static void xp_serial_out(void *user, int channel, int32_t word)
{
	sc88_t *b = user;
	lsp_serial_write(&b->lsp, channel, word);
	if (channel == 1)
		lsp_run_sample(&b->lsp);
}

static int32_t xp_serial_in(void *user, int channel)
{
	sc88_t *b = user;
	return b->lsp_mute ? 0 : -(lsp_serial_read(&b->lsp, channel ^ 1) >> 9);
}

/* ---------------------------------------------------------------- the H8's bus */

enum { DEV_NONE, DEV_ROM, DEV_SRAM, DEV_XP, DEV_SUB, DEV_GA, DEV_LSP };

static int decode(const sc88_t *b, uint32_t address, uint32_t *offset)
{
	const sc88_map_t *m = &b->map;
	for (int n = 0; n < 2; n++)
	{
		if (m->sram_alias[n] && address >= m->sram_alias[n] && address < m->sram_alias[n] + 0x8000)
		{
			*offset = 0x8000 + (address - m->sram_alias[n]);
			return DEV_SRAM;
		}
	}
	if (address < b->program_rom_size)
	{
		*offset = address;
		return DEV_ROM;
	}
	if (address >= m->sram_base && address < m->sram_base + SC88_SRAM_SIZE)
	{
		*offset = address - m->sram_base;
		return DEV_SRAM;
	}
	if (address >= m->xp_base && address < m->xp_base + 0x4000)
	{
		*offset = address - m->xp_base;
		return DEV_XP;
	}
	if (b->has_panel && address >= m->sub_base && address < m->sub_base + 0x100)
	{
		*offset = address - m->sub_base;
		return DEV_SUB;
	}
	if (address >= m->ga_base && address < m->ga_base + 0x100)
	{
		*offset = address - m->ga_base;
		return DEV_GA;
	}
	if (b->has_lsp && address >= m->lsp_base && address < m->lsp_base + 0x100)
	{
		*offset = address - m->lsp_base;
		return DEV_LSP;
	}
	return DEV_NONE;
}

static uint16_t bus_read16(void *user, uint32_t address)
{
	sc88_t *b = user;
	uint32_t offset;
	switch (decode(b, address, &offset))
	{
	case DEV_ROM:  return (uint16_t)((b->program_rom[offset] << 8) | b->program_rom[offset + 1]);
	case DEV_SRAM: return (uint16_t)((b->sram[offset] << 8) | b->sram[offset + 1]);
	case DEV_XP:   return xp_read(&b->xp, offset >> 1);
	case DEV_SUB:  return (uint16_t)((sub_hle_read(&b->sub, offset) << 8) | sub_hle_read(&b->sub, offset + 1));
	case DEV_GA:   return (uint16_t)((gate_array_read(&b->ga, offset) << 8) | gate_array_read(&b->ga, offset + 1));
	case DEV_LSP:  return (uint16_t)((lsp_host_read(&b->lsp, offset) << 8) | lsp_host_read(&b->lsp, offset + 1));
	default:       return 0;
	}
}

static uint8_t bus_read8(void *user, uint32_t address)
{
	sc88_t *b = user;
	uint32_t offset;
	switch (decode(b, address, &offset))
	{
	case DEV_ROM:  return b->program_rom[offset];
	case DEV_SRAM: return b->sram[offset];
	case DEV_XP:
	{
		const uint16_t word = xp_read(&b->xp, offset >> 1);
		return (uint8_t)((address & 1) ? word : word >> 8);
	}
	case DEV_SUB:  return sub_hle_read(&b->sub, offset);
	case DEV_GA:   return gate_array_read(&b->ga, offset);
	case DEV_LSP:  return lsp_host_read(&b->lsp, offset);
	default:       return 0;
	}
}

static void bus_write8(void *user, uint32_t address, uint8_t data)
{
	sc88_t *b = user;
	uint32_t offset;
	switch (decode(b, address, &offset))
	{
	case DEV_SRAM: b->sram[offset] = data; break;
	case DEV_XP:   xp_write(&b->xp, offset >> 1, (uint16_t)(data * 0x101), (address & 1) ? 0x00ff : 0xff00); break;
	case DEV_SUB:  sub_hle_write(&b->sub, offset, data); break;
	case DEV_GA:   gate_array_write(&b->ga, offset, data); break;
	case DEV_LSP:  lsp_host_write(&b->lsp, offset, data); break;
	default: break;
	}
}

static void bus_write16(void *user, uint32_t address, uint16_t data)
{
	sc88_t *b = user;
	uint32_t offset;
	switch (decode(b, address, &offset))
	{
	case DEV_SRAM:
		b->sram[offset] = (uint8_t)(data >> 8);
		b->sram[offset + 1] = (uint8_t)data;
		break;
	case DEV_XP:
		xp_write(&b->xp, offset >> 1, data, 0xffff);
		break;
	default:
		bus_write8(user, address, (uint8_t)(data >> 8));
		bus_write8(user, address + 1, (uint8_t)data);
		break;
	}
}

static uint8_t bus_read_port(void *user, int port)
{
	sc88_t *b = user;
	if (port == H8500_PORT8)
		return b->xp_int ? 0xfd : 0xff;
	return 0xff;
}

static void bus_write_port(void *user, int port, uint8_t data, uint8_t ddr)
{
	sc88_t *b = user;
	switch (port)
	{
	case H8500_PORT3:
		if (ddr & 0x80)
			b->lsp_mute = !(data & 0x80);
		break;
	case H8500_PORT4:
		if (ddr & 0x04)
			b->mute = !(data & 0x04);
		if (ddr & 0x01)
			sub_hle_reset_w(&b->sub, (data & 0x01) != 0);
		break;
	default:
		break;
	}
}

static uint16_t bus_read_adc(void *user, int channel)
{
	sc88_t *b = user;
	if (channel == 0)
		return 0x2a0;
	return b->computer_switch != SCEMU_COMPUTER_MIDI ? 0x3ff : 0;
}

static void bus_sci_tx(void *user, int channel, uint8_t byte)
{
	sc88_t *b = user;
	if (!b->has_panel && channel == H8500_SCI1 && b->midi_out)
		b->midi_out(&byte, 1, b->midi_out_user);
}

/* ---------------------------------------------------------------- lifetime */

bool sc88_init(sc88_t *b, scemu_model_t model, const scemu_roms_t *roms, const scemu_config_t *config)
{
	memset(b, 0, sizeof(*b));
	b->model = model;
	jit_alloc_init(&b->jit, config);

	const bool sc88 = (model == SCEMU_MODEL_SC88 || model == SCEMU_MODEL_SC88VL);
	b->map = sc88 ? MAP_SC88 : MAP_PRO;
	b->has_lsp = !sc88;
	b->has_panel = (model != SCEMU_MODEL_VEGSPRO);

	const rom_layout_t *l = &LAYOUT[model];
	b->program_rom_size = l->program_size;
	b->program_rom = malloc(b->program_rom_size);
	if (!b->program_rom)
		return false;
	memcpy(b->program_rom, roms->program_rom, b->program_rom_size);
	if (l->program_word_swapped)
	{
		for (size_t n = 0; n + 1 < b->program_rom_size; n += 2)
		{
			uint8_t t = b->program_rom[n];
			b->program_rom[n] = b->program_rom[n + 1];
			b->program_rom[n + 1] = t;
		}
	}

	b->wave_rom_size = wave_rom_size(model);
	b->wave_rom = malloc(b->wave_rom_size);
	if (!b->wave_rom || !wave_rom_build(model, roms, b->wave_rom, b->wave_rom_size))
		return false;

	h8500_bus_t bus =
	{
		bus_read8, bus_write8, bus_read16, bus_write16,
		bus_read_port, bus_write_port, bus_read_adc, bus_sci_tx, b
	};
	h8500_init(&b->cpu, &bus);
	if (sc88)
	{
		h8500_map(&b->cpu, 0x000000, 0x8000, b->program_rom, false);
		h8500_map(&b->cpu, 0x008000, 0x8000, b->sram + 0x8000, true);
		h8500_map(&b->cpu, 0x010000, (uint32_t)b->program_rom_size - 0x10000, b->program_rom + 0x10000, false);
	}
	else
		h8500_map(&b->cpu, 0x000000, (uint32_t)b->program_rom_size, b->program_rom, false);
	h8500_map(&b->cpu, b->map.sram_base, SC88_SRAM_SIZE, b->sram, true);

	xp_link_t link = { xp_irq, b->has_lsp ? xp_serial_out : NULL, b->has_lsp ? xp_serial_in : NULL, b };
	if (!xp_init(&b->xp, &link, &b->jit, b->wave_rom, b->wave_rom_size, wave_rom_chip_size(model)))
		return false;

	if (b->has_lsp)
	{
		if (!lsp_init(&b->lsp, &b->jit))
			return false;
		xp_set_serial_words(&b->xp, 0x01, 0x05);
	}

	lcd_init(&b->lcd);
	gate_array_init(&b->ga, &b->lcd, ga_irq, b);
	sub_hle_init(&b->sub, sub_irq, sub_midi_out, b);
	return true;
}

void sc88_release(sc88_t *b)
{
	if (b->has_lsp)
		lsp_release(&b->lsp);
	xp_release(&b->xp);
	free(b->wave_rom);
	free(b->program_rom);
	b->wave_rom = NULL;
	b->program_rom = NULL;
}

void sc88_reset(sc88_t *b)
{
	b->cpu_half_cycles = 0;
	b->xp_int = false;
	b->mute = true;
	b->lsp_mute = true;
	b->frame = 0;
	b->midi_head = b->midi_count = 0;
	xp_reset(&b->xp);
	if (b->has_lsp)
		lsp_reset(&b->lsp);
	lcd_reset(&b->lcd);
	gate_array_reset(&b->ga);
	sub_hle_reset(&b->sub);
	h8500_reset(&b->cpu);
}

/* ---------------------------------------------------------------- the frame */

static void deliver_midi(sc88_t *b)
{
	while (b->midi_count)
	{
		sc88_midi_event_t *e = &b->midi_queue[b->midi_head];
		if (e->frame > b->frame)
			break;
		if (b->has_panel)
			sub_hle_midi_byte(&b->sub, e->port, e->byte);
		else
			h8500_sci_rx(&b->cpu, e->port, e->byte);
		b->midi_head = (b->midi_head + 1) % SC88_MIDI_QUEUE_SIZE;
		b->midi_count--;
	}
}

void sc88_run_frame(sc88_t *b)
{
	deliver_midi(b);

	b->cpu_half_cycles += SC88_H8_HALF_CYCLES_PER_FRAME;
	int cycles = (int)(b->cpu_half_cycles >> 1);
	b->cpu_half_cycles &= 1;
	int ran = h8500_run(&b->cpu, cycles);
	if (ran > cycles)
		b->cpu_half_cycles -= (uint32_t)(ran - cycles) * 2;

	if (b->has_panel)
	{
		sub_hle_frame(&b->sub);
		gate_array_frame(&b->ga);
	}
	xp_run_frame(&b->xp);
	b->frame++;
}

bool sc88_idle(const sc88_t *b)
{
	return !b->mute && (!b->has_lsp || !b->lsp_mute);
}

void sc88_queue_midi(sc88_t *b, int port, uint8_t byte, uint32_t frame_offset)
{
	if (b->midi_count >= SC88_MIDI_QUEUE_SIZE)
		return;
	uint32_t slot = (b->midi_head + b->midi_count) % SC88_MIDI_QUEUE_SIZE;
	b->midi_queue[slot].frame = (uint32_t)b->frame + frame_offset;
	b->midi_queue[slot].port = (uint8_t)(port & 1);
	b->midi_queue[slot].byte = byte;
	b->midi_count++;
}

/* Matrix position of each button: strobe row and return bit. */
static const uint8_t BUTTON_ROW[SCEMU_BUTTON_COUNT] =
{
	0, 0, 0, 0, 0,
	2, 1, 0, 0, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1,
	3, 3, 3, 3, 3, 3, 3, 3
};

static const uint8_t BUTTON_BIT[SCEMU_BUTTON_COUNT] =
{
	6, 5, 2, 1, 7,
	6, 6, 3, 4, 4, 5, 4, 5, 2, 3, 2, 3, 0, 1, 0, 1,
	0, 1, 2, 3, 4, 5, 6, 7
};

void sc88_button(sc88_t *b, scemu_button_t button, bool down)
{
	if (button < 0 || button >= SCEMU_BUTTON_COUNT || !b->has_panel)
		return;
	sub_hle_set_key(&b->sub, BUTTON_ROW[button], BUTTON_BIT[button], down);
}

/* ---------------------------------------------------------------- state */

#define STATE_MAGIC "SCEMU1"

typedef struct state_header
{
	char magic[8];
	uint32_t model;
	uint32_t machine_size;
	uint32_t xp_eram_size;
	uint32_t lsp_eram_size;
} state_header_t;

size_t sc88_state_size(const sc88_t *b)
{
	return sizeof(state_header_t) + sizeof(sc88_t) + XP_ERAM_SIZE * sizeof(int32_t) + (b->has_lsp ? LSP_ERAM_SIZE * sizeof(int32_t) : 0);
}

size_t sc88_state_save(const sc88_t *b, void *buffer, size_t size)
{
	const size_t need = sc88_state_size(b);
	if (size < need)
		return 0;
	uint8_t *p = buffer;
	state_header_t h;
	memset(&h, 0, sizeof(h));
	memcpy(h.magic, STATE_MAGIC, sizeof(STATE_MAGIC));
	h.model = (uint32_t)b->model;
	h.machine_size = (uint32_t)sizeof(sc88_t);
	h.xp_eram_size = XP_ERAM_SIZE * sizeof(int32_t);
	h.lsp_eram_size = b->has_lsp ? LSP_ERAM_SIZE * sizeof(int32_t) : 0;
	memcpy(p, &h, sizeof(h));
	p += sizeof(h);
	memcpy(p, b, sizeof(*b));
	p += sizeof(*b);
	memcpy(p, b->xp.eram, h.xp_eram_size);
	p += h.xp_eram_size;
	if (b->has_lsp)
		memcpy(p, b->lsp.eram, h.lsp_eram_size);
	return need;
}

bool sc88_state_load(sc88_t *b, const void *buffer, size_t size)
{
	state_header_t h;
	if (size < sizeof(h))
		return false;
	memcpy(&h, buffer, sizeof(h));
	if (memcmp(h.magic, STATE_MAGIC, sizeof(STATE_MAGIC)) != 0 || h.model != (uint32_t)b->model
		|| h.machine_size != sizeof(sc88_t) || h.xp_eram_size != XP_ERAM_SIZE * sizeof(int32_t)
		|| h.lsp_eram_size != (b->has_lsp ? LSP_ERAM_SIZE * sizeof(int32_t) : 0)
		|| size < sc88_state_size(b))
		return false;

	uint8_t *program_rom = b->program_rom;
	uint8_t *wave_rom = b->wave_rom;
	h8500_bus_t bus = b->cpu.bus;
	h8500_region_t regions[H8500_MAX_REGIONS];
	memcpy(regions, b->cpu.regions, sizeof(regions));
	int region_count = b->cpu.region_count;
	xp_link_t xp_link = b->xp.link;
	const uint8_t *xp_wave = b->xp.wave;
	int32_t *xp_eram = b->xp.eram;
	jit_code_t xp_code[2] = { b->xp.code[0], b->xp.code[1] };
	int xp_live = b->xp.live;
	xp_frame_fn xp_frame = b->xp.frame;
	int32_t *lsp_eram = b->lsp.eram;
	jit_code_t lsp_code[2] = { b->lsp.code[0], b->lsp.code[1] };
	int lsp_live = b->lsp.live;
	lsp_sample_fn lsp_sample = b->lsp.sample;
	scemu_midi_out_fn midi_out = b->midi_out;
	void *midi_out_user = b->midi_out_user;

	const uint8_t *p = (const uint8_t *)buffer + sizeof(h);
	memcpy(b, p, sizeof(*b));
	p += sizeof(*b);

	b->program_rom = program_rom;
	b->wave_rom = wave_rom;
	b->cpu.bus = bus;
	memcpy(b->cpu.regions, regions, sizeof(regions));
	b->cpu.region_count = region_count;
	b->xp.link = xp_link;
	b->xp.wave = xp_wave;
	b->xp.eram = xp_eram;
	b->xp.jit = &b->jit;
	b->xp.code[0] = xp_code[0];
	b->xp.code[1] = xp_code[1];
	b->xp.live = xp_live;
	b->xp.frame = xp_frame;
	b->xp.program_dirty = true;
	b->lsp.eram = lsp_eram;
	b->lsp.jit = &b->jit;
	b->lsp.code[0] = lsp_code[0];
	b->lsp.code[1] = lsp_code[1];
	b->lsp.live = lsp_live;
	b->lsp.sample = lsp_sample;
	b->lsp.dirty = true;
	b->ga.lcd = &b->lcd;
	b->ga.irq = ga_irq;
	b->ga.user = b;
	b->sub.irq = sub_irq;
	b->sub.midi_out = sub_midi_out;
	b->sub.user = b;
	b->midi_out = midi_out;
	b->midi_out_user = midi_out_user;

	memcpy(b->xp.eram, p, h.xp_eram_size);
	p += h.xp_eram_size;
	if (b->has_lsp)
		memcpy(b->lsp.eram, p, h.lsp_eram_size);
	return true;
}
