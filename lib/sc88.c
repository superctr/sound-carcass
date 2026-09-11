#include <stdlib.h>
#include <string.h>
#include "scemu_internal.h"
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

/* ---------------------------------------------------------------- the gate array */

static void ga_update_int(sc88_ga_t *ga)
{
	ga->irq(ga->user, (ga->int_pending & ~ga->int_mask) != 0);
}

void sc88_ga_init(sc88_ga_t *ga, lcd_t *lcd, void (*irq)(void *user, bool state), void *user)
{
	memset(ga, 0, sizeof(*ga));
	ga->lcd = lcd;
	ga->irq = irq;
	ga->user = user;
}

void sc88_ga_reset(sc88_ga_t *ga)
{
	memset(ga->regs, 0, sizeof(ga->regs));
	ga->int_pending = 0;
	ga->int_mask = 0;
	ga->leds = 0;
	ga->lcd_fifo_count = 0;
	ga->lcd_command_pending = false;
	ga->lcd_busy_frames = 0;
	ga_update_int(ga);
}

uint8_t sc88_ga_read(sc88_ga_t *ga, uint32_t offset)
{
	offset &= 0xff;
	if (offset == 0x04)
	{
		const uint8_t active = ga->int_pending & ~ga->int_mask;
		for (int source = 0; source < 8; source++)
		{
			if ((active >> source) & 1)
			{
				ga->int_pending &= (uint8_t)~(1u << source);
				ga_update_int(ga);
				return (uint8_t)(source + 1);
			}
		}
		return 0;
	}
	return ga->regs[offset];
}

static void ga_update_leds(sc88_ga_t *ga)
{
	const uint8_t data = ga->regs[0x00];
	const uint8_t commons = ga->regs[0x01];
	ga->leds = (uint16_t)((commons & 1) ? 0 : data);
	if (commons & 2)
		ga->leds |= 0x100;
}

void sc88_ga_write(sc88_ga_t *ga, uint32_t offset, uint8_t data)
{
	offset &= 0xff;
	ga->regs[offset] = data;
	switch (offset)
	{
	case 0x00:
	case 0x01:
		ga_update_leds(ga);
		break;
	case 0x05:
		ga->int_mask = data;
		ga_update_int(ga);
		break;
	case 0x1e:
	{
		int bytes = ga->lcd_fifo_count;
		if (ga->lcd_command_pending)
		{
			lcd_command(ga->lcd, ga->regs[0x1f]);
			bytes++;
		}
		for (int i = 0; i < ga->lcd_fifo_count; i++)
			lcd_data(ga->lcd, ga->lcd_fifo[i]);
		ga->lcd_command_pending = false;
		ga->lcd_fifo_count = 0;
		ga->lcd_busy_frames = (uint32_t)((40 * (bytes + 1) * 32 + 999) / 1000);
		break;
	}
	case 0x1f:
		ga->lcd_command_pending = true;
		break;
	default:
		if (offset >= 0x20 && offset <= 0x2c && ga->lcd_fifo_count < 13)
			ga->lcd_fifo[ga->lcd_fifo_count++] = data;
		break;
	}
}

void sc88_ga_frame(sc88_ga_t *ga)
{
	if (ga->lcd_busy_frames && !--ga->lcd_busy_frames)
	{
		ga->int_pending |= 1;
		ga_update_int(ga);
	}
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
		b->midi_out(SCEMU_MIDI_IN_A, &byte, 1, b->midi_out_user);
}

/* Port B's SDOB carries the EFX send to the LSP's TRR: the XP's port carries the top 18 bits of its word,
   which are the top 18 of the LSP's; TRS0 comes back eight bits down and inverted */
static void xp_port_out(void *user, int port, int32_t word)
{
	sc88_t *b = user;
	if ((port >> 1) != XP_PORT_B)
		return;
	lsp_serial_write(&b->lsp, port & 1, word);
	if (port & 1)
		lsp_run_sample(&b->lsp);
}

static int32_t xp_port_a_in(void *user, int strobe)
{
	sc88_t *b = user;
	return b->lsp_mute ? 0 : -(lsp_serial_read(&b->lsp, strobe & 1) >> 8);
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
	case DEV_GA:   return (uint16_t)((sc88_ga_read(&b->ga, offset) << 8) | sc88_ga_read(&b->ga, offset + 1));
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
	case DEV_GA:   return sc88_ga_read(&b->ga, offset);
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
	case DEV_GA:   sc88_ga_write(&b->ga, offset, data); break;
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
	if (port == H8500_PORT5)
		return 0x2d;
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
		b->midi_out(SCEMU_MIDI_IN_A, &byte, 1, b->midi_out_user);
}

/* ---------------------------------------------------------------- lifetime */

static uint64_t hash_add(uint64_t h, const void *data, size_t size)
{
	const uint8_t *p = data;
	for (size_t n = 0; n + 8 <= size; n += 8)
	{
		uint64_t v;
		memcpy(&v, p + n, 8);
		h = (h ^ v) * 0x100000001b3ull;
	}
	for (size_t n = size & ~(size_t)7; n < size; n++)
		h = (h ^ p[n]) * 0x100000001b3ull;
	return h ^ (uint64_t)size;
}

bool sc88_init(sc88_t *b, scemu_model_t model, const scemu_roms_t *roms, const scemu_config_t *config)
{
	memset(b, 0, sizeof(*b));
	b->model = model;
	midi_queue_init(&b->midi, SC88_MIDI_PORTS, SC88_SAMPLE_RATE);
	jit_alloc_init(&b->jit, config);

	const bool sc88 = (model == SCEMU_MODEL_SC88 || model == SCEMU_MODEL_SC88VL);
	b->map = sc88 ? MAP_SC88 : MAP_PRO;
	b->has_lsp = !sc88;
	b->has_panel = (model != SCEMU_MODEL_VEGSPRO);

	uint64_t id = 0xcbf29ce484222325ull;
	id = hash_add(id, roms->program_rom, roms->program_rom_size);
	for (int n = 0; n < roms->wave_rom_count; n++)
		id = hash_add(id, roms->wave_rom[n], roms->wave_rom_size[n]);
	b->rom_id = id;

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
#ifdef SCEMU_H8500_JIT
	h8500_jit_attach(&b->cpu, &b->jit);
	if (config->h8500_interpreter || (getenv("SCEMU_H8500_JIT") && !atoi(getenv("SCEMU_H8500_JIT"))))
		b->cpu.jit_enabled = false;
#endif

	xp_link_t link = { xp_irq, b->has_lsp ? xp_port_out : NULL, b->has_lsp ? xp_port_a_in : NULL, NULL, b };
	if (!xp_init(&b->xp, &link, &b->jit, b->wave_rom, b->wave_rom_size, wave_rom_chip_size(model)))
		return false;

	if (b->has_lsp)
	{
		if (!lsp_init(&b->lsp, &b->jit))
			return false;
	}

	lcd_init(&b->lcd);
	sc88_ga_init(&b->ga, &b->lcd, ga_irq, b);
	sub_hle_init(&b->sub, sub_irq, sub_midi_out, b);
	return true;
}

void sc88_release(sc88_t *b)
{
	h8500_jit_detach(&b->cpu);
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
	midi_queue_reset(&b->midi);
	xp_reset(&b->xp);
	if (b->has_lsp)
		lsp_reset(&b->lsp);
	lcd_reset(&b->lcd);
	sc88_ga_reset(&b->ga);
	sub_hle_reset(&b->sub);
	h8500_reset(&b->cpu);
}

/* ---------------------------------------------------------------- the frame */

static bool midi_take(void *user, int port, uint8_t byte)
{
	sc88_t *b = user;
	if (b->has_panel)
	{
		if (!sub_hle_ready(&b->sub))
			return false;
		sub_hle_midi_byte(&b->sub, port, byte);
		return true;
	}
	if (b->cpu.sci[port].rx_pending)
		return false;
	h8500_sci_rx(&b->cpu, port, byte);
	return true;
}

void sc88_deliver_midi(sc88_t *b)
{
	midi_queue_deliver(&b->midi, (uint32_t)b->frame, midi_take, b);
}

void sc88_run_frame(sc88_t *b)
{
	sc88_deliver_midi(b);

	b->cpu_half_cycles += SC88_H8_HALF_CYCLES_PER_FRAME;
	int cycles = (int)(b->cpu_half_cycles >> 1);
	b->cpu_half_cycles &= 1;
	int ran = h8500_run(&b->cpu, cycles);
	if (ran > cycles)
		b->cpu_half_cycles -= (uint32_t)(ran - cycles) * 2;

	if (b->has_panel)
	{
		sub_hle_frame(&b->sub);
		sc88_ga_frame(&b->ga);
	}
	xp_run_frame(&b->xp);
	b->frame++;
}

bool sc88_idle(const sc88_t *b)
{
	return !b->mute;
}

void sc88_queue_midi(sc88_t *b, int port, uint8_t byte, uint32_t frame_offset)
{
	midi_queue_push(&b->midi, port, byte, (uint32_t)b->frame + frame_offset);
}

/* Matrix position of each button: strobe row and return bit. */
static const uint8_t BUTTON_ROW[SCEMU_BUTTON_F1] =
{
	0, 0, 0, 0, 0,
	2, 1, 0, 0, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1,
	3, 3, 3, 3, 3, 3, 3, 3
};

static const uint8_t BUTTON_BIT[SCEMU_BUTTON_F1] =
{
	6, 5, 2, 1, 7,
	6, 6, 3, 4, 4, 5, 4, 5, 2, 3, 2, 3, 0, 1, 0, 1,
	0, 1, 2, 3, 4, 5, 6, 7
};

void sc88_button(sc88_t *b, scemu_button_t button, bool down)
{
	/* the SC-88VL's POWER/STANDBY key is SW401, the position its siblings leave free */
	if (button == SCEMU_BUTTON_POWER && b->model == SCEMU_MODEL_SC88VL)
	{
		sub_hle_set_key(&b->sub, 0, 0, down);
		return;
	}
	if (button < 0 || button >= SCEMU_BUTTON_F1 || !b->has_panel)
		return;
	sub_hle_set_key(&b->sub, BUTTON_ROW[button], BUTTON_BIT[button], down);
}

/* ---------------------------------------------------------------- the board as the API sees it */

static const char *ops_validate_roms(scemu_model_t model, const scemu_roms_t *roms) { return sc88_validate_roms(model, roms); }
static bool ops_init(void *b, scemu_model_t model, const scemu_roms_t *roms, const scemu_config_t *config) { return sc88_init(b, model, roms, config); }
static void ops_release(void *b) { sc88_release(b); }
static void ops_reset(void *b) { sc88_reset(b); }
static void ops_run_frame(void *b) { sc88_run_frame(b); }
static bool ops_idle(const void *b) { return sc88_idle(b); }
static uint64_t ops_frame(const void *b) { return ((const sc88_t *)b)->frame; }
static uint64_t ops_rom_id(const void *b) { return ((const sc88_t *)b)->rom_id; }
static uint32_t ops_sample_rate(const void *b) { (void)b; return SC88_SAMPLE_RATE; }
static int ops_output_count(const void *b) { return ((const sc88_t *)b)->has_lsp ? 2 : 1; }

/* one DAC on SDOC, the frame half picking left or right; the Pro's OUTPUT2 is the pair after it, on SDOD */
static int32_t ops_output(const void *board, int pair, int channel)
{
	const sc88_t *b = board;
	if (b->mute)
		return 0;
	return xp_output(&b->xp, (pair ? 4 : 2) + channel);
}

static midi_queue_t *ops_midi(void *b) { return &((sc88_t *)b)->midi; }
static void ops_set_midi_out(void *board, scemu_midi_out_fn fn, void *user)
{
	sc88_t *b = board;
	b->midi_out = fn;
	b->midi_out_user = user;
}
static void ops_button(void *b, scemu_button_t button, bool down) { sc88_button(b, button, down); }
static void ops_set_computer_switch(void *b, scemu_computer_switch_t sw) { ((sc88_t *)b)->computer_switch = sw; }
/* The eight data lines are the same lamps on all three, and the second common is not:
   the SC-88Pro's drives its lens's red die, the SC-88VL's its STANDBY lamp, and the
   SC-88 leaves it parked. */
static uint32_t ops_leds(const void *board)
{
	const sc88_t *b = board;
	const uint32_t leds = b->ga.leds;
	if (b->model == SCEMU_MODEL_SC88PRO)
		return leds;
	const uint32_t standby = (b->model == SCEMU_MODEL_SC88VL && (leds & 0x100)) ? 1u << SCEMU_LED_STANDBY : 0;
	return (leds & 0xff) | standby;
}
static scemu_lcd_t *ops_lcd(void *b) { return &((sc88_t *)b)->lcd.out; }

static size_t ops_nvram_size(const void *b) { (void)b; return SC88_SRAM_SIZE; }
static size_t ops_nvram_get(const void *board, void *buffer, size_t size)
{
	if (size < SC88_SRAM_SIZE)
		return 0;
	memcpy(buffer, ((const sc88_t *)board)->sram, SC88_SRAM_SIZE);
	return SC88_SRAM_SIZE;
}
static bool ops_nvram_set(void *board, const void *buffer, size_t size)
{
	if (size != SC88_SRAM_SIZE)
		return false;
	memcpy(((sc88_t *)board)->sram, buffer, SC88_SRAM_SIZE);
	return true;
}

static size_t ops_state_size(const void *b) { return sc88_state_size(b); }
static size_t ops_state_save(const void *b, void *buffer, size_t size) { return sc88_state_save(b, buffer, size); }
static bool ops_state_load(void *b, const void *buffer, size_t size) { return sc88_state_load(b, buffer, size); }

const board_ops_t sc88_board_ops =
{
	ops_validate_roms, ops_init, ops_release, ops_reset, ops_run_frame, ops_idle, ops_frame, ops_rom_id, ops_sample_rate,
	ops_output_count, ops_output,
	ops_midi, ops_set_midi_out,
	ops_button, ops_set_computer_switch, ops_leds, ops_lcd, NULL, NULL,
	ops_nvram_size, ops_nvram_get, ops_nvram_set,
	ops_state_size, ops_state_save, ops_state_load,
};
