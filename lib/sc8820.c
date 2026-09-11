#include <stdlib.h>
#include <string.h>
#include "scemu_internal.h"
#include "wave_rom.h"

#define PE_MUTE_RELEASE 0x0200
#define PE_POWER_LAMP 0x0010
#define PE_MAP_KEY 0x0040
#define PE_PREVIEW_KEY 0x0080
#define PE_ROW0 0x8000
#define PE_ROW1 0x4000
#define PA_ROW2 0x8000
#define PE_COLUMNS 0x000f
#define PE_IDLE_PINS 0x00c0

#define ROM_ID_SEED 0xcbf29ce484222325ull

const char *sc8820_validate_roms(scemu_model_t model, const scemu_roms_t *roms)
{
	if (model != SCEMU_MODEL_SC8820)
		return "not an SC-8820";
	if (!roms->boot_rom || roms->boot_rom_size != SC8820_BOOT_ROM_SIZE)
		return "CPU ROM must be 64 KB";
	if (!roms->program_rom || roms->program_rom_size != SC8820_PROGRAM_ROM_SIZE)
		return "program flash must be 2 MB";
	if (roms->tone_rom || roms->tone_rom_size)
		return "the SC-8820 has no separate tone flash";
	if (roms->wave_rom_count != 2 || !roms->wave_rom[0] || !roms->wave_rom[1]
	    || roms->wave_rom_size[0] != 0x1000000 || roms->wave_rom_size[1] != 0x800000)
		return "wave ROMs must be 16 MB and 8 MB";
	return NULL;
}

/* ---------------------------------------------------------------- the panel */

static void panel_update(sc8820_panel_t *p)
{
	static const uint8_t COLUMN_LED[SC8820_PANEL_ROWS][4] =
	{
		{ SCEMU_LED_PART_A2, SCEMU_LED_PART_A1, SCEMU_LED_PART_A4, SCEMU_LED_PART_A3 },
		{ SCEMU_LED_PART_B2, SCEMU_LED_PART_B1, SCEMU_LED_PART_B4, SCEMU_LED_PART_B3 },
		{ SCEMU_LED_MAP, 0, SCEMU_LED_USB, 0 },
	};
	uint32_t leds = 0;
	for (int row = 0; row < SC8820_PANEL_ROWS; row++)
		for (int column = 0; column < 4; column++)
			if (((p->column[row] >> column) & 1) && COLUMN_LED[row][column])
				leds |= 1u << COLUMN_LED[row][column];
	if (!(p->pe & PE_POWER_LAMP))
		leds |= 1u << SCEMU_LED_POWER;
	p->leds = leds;
}

static void panel_sample(sc8820_panel_t *p)
{
	const bool row[SC8820_PANEL_ROWS] = { !(p->pe & PE_ROW0), !(p->pe & PE_ROW1), !(p->pa & PA_ROW2) };
	for (int n = 0; n < SC8820_PANEL_ROWS; n++)
		if (row[n])
			p->column[n] = (uint8_t)(p->pe & PE_COLUMNS);
	panel_update(p);
}

static void panel_frame(sc8820_panel_t *p)
{
	if (p->written)
	{
		p->written = false;
		return;
	}
	panel_sample(p);
}

/* the keys are the host's, held through a reset as through the power switch */
static void panel_reset(sc8820_panel_t *p)
{
	const bool map_key = p->map_key, preview_key = p->preview_key;
	memset(p, 0, sizeof(*p));
	p->pe = 0xffff;
	p->pa = 0xffff;
	p->map_key = map_key;
	p->preview_key = preview_key;
}

/* ---------------------------------------------------------------- the USB controller's mailboxes */

/* the controller's two events are edges on IRQ2 (a byte waits in UIPC(1)) and IRQ1 (UIPC(0) took
 * the byte), which the CPU latches */
static void uipc_rx_event(void *user)
{
	sc8820_t *b = user;
	sh2_set_irq(&b->cpu, SH2_IRQ2, true);
	sh2_set_irq(&b->cpu, SH2_IRQ2, false);
}

static void uipc_tx_event(void *user)
{
	sc8820_t *b = user;
	sh2_set_irq(&b->cpu, SH2_IRQ1, true);
	sh2_set_irq(&b->cpu, SH2_IRQ1, false);
}

static void uipc_midi_out(void *user, int port, const uint8_t *bytes, size_t count)
{
	sc8820_t *b = user;
	if (b->midi_out)
		b->midi_out(port, bytes, count, b->midi_out_user);
}

static scemu_computer_switch_t uipc_computer_switch(void *user)
{
	return ((sc8820_t *)user)->computer_switch;
}

/* ---------------------------------------------------------------- the chip's wiring */

static void xp_irq(void *user, bool state)
{
	sc8820_t *b = user;
	sh2_set_irq(&b->cpu, SH2_IRQ0, state);
}

/* the one chip puts the insertion effect on the lines the SC-8850 keeps for the link between its
 * two: the send leaves on port B and the return comes back on port A */
static void xp_port_out(void *user, int port, int32_t word)
{
	sc8820_t *b = user;
	if ((port >> 1) != XP_PORT_B)
		return;
	lsp_serial_write(&b->lsp, port & 1, word);
	if (port & 1)
		lsp_run_sample(&b->lsp);
}

static int32_t xp_port_a_in(void *user, int strobe)
{
	sc8820_t *b = user;
	return -(lsp_serial_read(&b->lsp, strobe & 1) >> 8);
}

/* ---------------------------------------------------------------- the SH-2's bus */

enum { DEV_NONE, DEV_DRAM, DEV_UIPC0, DEV_UIPC1, DEV_LSP, DEV_XP, DEV_FLASH };

static int decode(uint32_t address, uint32_t *offset)
{
	if (address & 0x01000000)
	{
		*offset = address & (SC8820_DRAM_SIZE - 1);
		return DEV_DRAM;
	}
	switch ((address >> 22) & 3)
	{
	case 3:
		*offset = address & (SC8820_PROGRAM_ROM_SIZE - 1);
		return DEV_FLASH;
	case 1:
		switch ((address >> 18) & 0xf)
		{
		case 0x4: *offset = address & 0xf; return DEV_LSP;
		case 0x5: *offset = address & 1; return DEV_UIPC0;
		case 0x6: *offset = address & 1; return DEV_UIPC1;
		default: return DEV_NONE;
		}
	case 2:
		*offset = address & 0x3fff;
		return (address & 0xffc000) == 0x00900000 ? DEV_XP : DEV_NONE;
	default:
		return DEV_NONE;
	}
}

static uint8_t bus_read8(void *user, uint32_t address)
{
	sc8820_t *b = user;
	uint32_t offset;
	switch (decode(address, &offset))
	{
	case DEV_DRAM:  return b->dram[offset];
	case DEV_UIPC0: return uipc_read(&b->uipc, 0, offset);
	case DEV_UIPC1: return uipc_read(&b->uipc, 1, offset);
	case DEV_LSP:   return lsp_host_read(&b->lsp, offset);
	case DEV_XP:
	{
		const uint16_t word = xp_read(&b->xp, offset >> 1);
		return (uint8_t)((address & 1) ? word : word >> 8);
	}
	case DEV_FLASH:
	{
		const uint16_t word = flash_read(&b->flash, offset);
		return (uint8_t)((address & 1) ? word : word >> 8);
	}
	default:        return 0;
	}
}

static uint16_t bus_read16(void *user, uint32_t address)
{
	sc8820_t *b = user;
	uint32_t offset;
	switch (decode(address, &offset))
	{
	case DEV_DRAM:  return (uint16_t)((b->dram[offset] << 8) | b->dram[offset + 1]);
	case DEV_XP:    return xp_read(&b->xp, offset >> 1);
	case DEV_FLASH: return flash_read(&b->flash, offset);
	case DEV_NONE:  return 0;
	default:        return (uint16_t)((bus_read8(user, address) << 8) | bus_read8(user, address + 1));
	}
}

static void flash_bus_write(sc8820_t *b, uint32_t offset, uint16_t data)
{
	flash_t *f = &b->flash;
	flash_write(f, offset, data);
	for (int n = 0; n < b->cpu.region_count; n++)
	{
		sh2_region_t *r = &b->cpu.regions[n];
		if (r->data == f->data && r->bypass != !flash_in_array(f))
		{
			r->bypass = !flash_in_array(f);
			sh2_jit_remap(&b->cpu);
		}
	}
	if (f->erased)
	{
		f->erased = false;
		b->code_flush_pending = true;
	}
}

static void bus_write8(void *user, uint32_t address, uint8_t data)
{
	sc8820_t *b = user;
	uint32_t offset;
	switch (decode(address, &offset))
	{
	case DEV_DRAM:  b->dram[offset] = data; break;
	case DEV_LSP:   lsp_host_write(&b->lsp, offset, data); b->tg_written = b->frame; break;
	case DEV_UIPC0: uipc_write(&b->uipc, 0, offset, data); break;
	case DEV_UIPC1: uipc_write(&b->uipc, 1, offset, data); break;
	case DEV_XP:    xp_write(&b->xp, offset >> 1, (uint16_t)(data * 0x101), (address & 1) ? 0x00ff : 0xff00); b->tg_written = b->frame; break;
	case DEV_FLASH: flash_bus_write(b, offset, data); break;
	default: break;
	}
}

static void bus_write16(void *user, uint32_t address, uint16_t data)
{
	sc8820_t *b = user;
	uint32_t offset;
	switch (decode(address, &offset))
	{
	case DEV_DRAM:
		b->dram[offset] = (uint8_t)(data >> 8);
		b->dram[offset + 1] = (uint8_t)data;
		break;
	case DEV_XP:
		xp_write(&b->xp, offset >> 1, data, 0xffff);
		b->tg_written = b->frame;
		break;
	case DEV_FLASH:
		flash_bus_write(b, offset, data);
		break;
	case DEV_NONE:
		break;
	default:
		bus_write8(user, address, (uint8_t)(data >> 8));
		bus_write8(user, address + 1, (uint8_t)data);
		break;
	}
}

static uint16_t bus_read_port(void *user, int port)
{
	sc8820_t *b = user;
	if (port != SH2_PORT_E)
		return 0;
	return (uint16_t)(PE_IDLE_PINS & ~(b->panel.map_key ? PE_MAP_KEY : 0) & ~(b->panel.preview_key ? PE_PREVIEW_KEY : 0));
}

static void bus_write_port(void *user, int port, uint16_t data, uint16_t ior)
{
	sc8820_t *b = user;
	if (port == SH2_PORT_E)
	{
		if (ior & PE_MUTE_RELEASE)
			b->mute = !(data & PE_MUTE_RELEASE);
		b->panel.pe = (uint16_t)((data & ior) | ~ior);
		b->panel.written = true;
		panel_update(&b->panel);
	}
	else if (port == SH2_PORT_A)
	{
		b->panel.pa = (uint16_t)((data & ior) | ~ior);
		b->panel.written = true;
	}
}

static uint16_t bus_read_adc(void *user, int channel)
{
	(void)user;
	(void)channel;
	return 0;
}

static void bus_sci_tx(void *user, int channel, uint8_t byte)
{
	sc8820_t *b = user;
	if (channel == SH2_SCI0 && b->midi_out)
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

static uint8_t *copy_rom(const void *data, size_t size)
{
	uint8_t *rom = malloc(size);
	if (rom)
		memcpy(rom, data, size);
	return rom;
}

bool sc8820_init(sc8820_t *b, scemu_model_t model, const scemu_roms_t *roms, const scemu_config_t *config)
{
	memset(b, 0, sizeof(*b));
	midi_queue_init(&b->midi, SC8820_MIDI_PORTS, SC8820_SAMPLE_RATE);
	jit_alloc_init(&b->jit, config);

	uint64_t id = ROM_ID_SEED;
	id = hash_add(id, roms->boot_rom, roms->boot_rom_size);
	id = hash_add(id, roms->program_rom, roms->program_rom_size);
	for (int n = 0; n < roms->wave_rom_count; n++)
		id = hash_add(id, roms->wave_rom[n], roms->wave_rom_size[n]);
	b->rom_id = id;

	b->boot_rom = copy_rom(roms->boot_rom, SC8820_BOOT_ROM_SIZE);
	b->program_rom = copy_rom(roms->program_rom, SC8820_PROGRAM_ROM_SIZE);
	if (!b->boot_rom || !b->program_rom)
		return false;
	flash_init(&b->flash, b->program_rom, SC8820_PROGRAM_ROM_SIZE, SC8820_FLASH_DEVICE);

	b->wave_rom_size = wave_rom_size(model);
	b->wave_rom = malloc(b->wave_rom_size);
	if (!b->wave_rom || !wave_rom_build(model, roms, b->wave_rom, b->wave_rom_size))
		return false;

	sh2_bus_t bus =
	{
		bus_read8, bus_read16, NULL, bus_write8, bus_write16, NULL,
		bus_read_port, bus_write_port, bus_read_adc, bus_sci_tx, b
	};
	sh2_init(&b->cpu, &bus);
	sh2_map(&b->cpu, 0x00000000, SC8820_BOOT_ROM_SIZE, b->boot_rom, false);
	sh2_map(&b->cpu, 0x00d00000, SC8820_PROGRAM_ROM_SIZE, b->program_rom, false);
	sh2_map(&b->cpu, 0x01000000, SC8820_DRAM_SIZE, b->dram, true);
	sh2_jit_attach(&b->cpu, &b->jit);
	if (config->sh2_interpreter)
		b->cpu.jit_enabled = false;

	xp_link_t link = { xp_irq, xp_port_out, xp_port_a_in, NULL, b };
	if (!xp_init(&b->xp, &link, &b->jit, b->wave_rom, b->wave_rom_size, wave_rom_chip_size(model)))
		return false;
	if (!lsp_init(&b->lsp, &b->jit))
		return false;
	uipc_link_t uipc_link = { uipc_rx_event, uipc_tx_event, uipc_midi_out, uipc_computer_switch, b };
	uipc_init(&b->uipc, &uipc_link, true);
	return true;
}

void sc8820_release(sc8820_t *b)
{
	sh2_jit_detach(&b->cpu);
	lsp_release(&b->lsp);
	xp_release(&b->xp);
	free(b->wave_rom);
	free(b->program_rom);
	free(b->boot_rom);
	b->wave_rom = NULL;
	b->program_rom = NULL;
	b->boot_rom = NULL;
}

void sc8820_reset(sc8820_t *b)
{
	b->cpu_overshoot = 0;
	b->mute = true;
	b->frame = 0;
	b->tg_written = 0;
	b->code_flush_pending = false;
	uipc_reset(&b->uipc);
	midi_queue_reset(&b->midi);
	xp_reset(&b->xp);
	lsp_reset(&b->lsp);
	flash_reset(&b->flash);
	panel_reset(&b->panel);
	for (int n = 0; n < b->cpu.region_count; n++)
		b->cpu.regions[n].bypass = false;
	sh2_jit_remap(&b->cpu);
	sh2_reset(&b->cpu);
}

/* ---------------------------------------------------------------- the frame */

/* the one jack is port A; on the USB position the controller carries A and B, and whatever else
 * is offered goes where the firmware puts a cable it has no group for */
static bool midi_take(void *user, int port, uint8_t byte)
{
	sc8820_t *b = user;
	if (uipc_host(&b->uipc))
	{
		if (!uipc_online(&b->uipc))
			return true;
		if (!uipc_can_take(&b->uipc))
			return false;
		uipc_take_midi(&b->uipc, port, byte);
		return true;
	}
	if (port != SCEMU_MIDI_IN_A)
		return true;
	if (b->cpu.sci[SH2_SCI0].rx_pending)
		return false;
	sh2_sci_rx(&b->cpu, SH2_SCI0, byte);
	return true;
}

void sc8820_deliver_midi(sc8820_t *b)
{
	midi_queue_deliver(&b->midi, (uint32_t)b->frame, midi_take, b);
}

void sc8820_run_frame(sc8820_t *b)
{
	sc8820_deliver_midi(b);

	const int cycles = (int)(SC8820_SH2_CYCLES_PER_FRAME - b->cpu_overshoot);
	const int ran = sh2_run(&b->cpu, cycles);
	if (b->code_flush_pending)
	{
		b->code_flush_pending = false;
		sh2_jit_flush(&b->cpu);
	}
	b->cpu_overshoot = ran > cycles ? (uint32_t)(ran - cycles) : 0;

	panel_frame(&b->panel);
	uipc_frame(&b->uipc);
	xp_run_frame(&b->xp);
	b->frame++;
}

bool sc8820_idle(const sc8820_t *b)
{
	return !b->mute && b->frame - b->tg_written >= SC8820_IDLE_FRAMES;
}

void sc8820_queue_midi(sc8820_t *b, int port, uint8_t byte, uint32_t frame_offset)
{
	midi_queue_push(&b->midi, port, byte, (uint32_t)b->frame + frame_offset);
}

void sc8820_button(sc8820_t *b, scemu_button_t button, bool down)
{
	if (button == SCEMU_BUTTON_MAP)
		b->panel.map_key = down;
	else if (button == SCEMU_BUTTON_PREVIEW)
		b->panel.preview_key = down;
}

/* ---------------------------------------------------------------- the board as the API sees it */

static const char *ops_validate_roms(scemu_model_t model, const scemu_roms_t *roms) { return sc8820_validate_roms(model, roms); }
static bool ops_init(void *b, scemu_model_t model, const scemu_roms_t *roms, const scemu_config_t *config) { return sc8820_init(b, model, roms, config); }
static void ops_release(void *b) { sc8820_release(b); }
static void ops_reset(void *b) { sc8820_reset(b); }
static void ops_run_frame(void *b) { sc8820_run_frame(b); }
static bool ops_idle(const void *b) { return sc8820_idle(b); }
static uint64_t ops_frame(const void *b) { return ((const sc8820_t *)b)->frame; }
static uint64_t ops_rom_id(const void *b) { return ((const sc8820_t *)b)->rom_id; }
static uint32_t ops_sample_rate(const void *b) { (void)b; return SC8820_SAMPLE_RATE; }
static int ops_output_count(const void *b) { (void)b; return 1; }

/* the chip's program puts the one DAC on words 4 and 5 */
static int32_t ops_output(const void *board, int pair, int channel)
{
	const sc8820_t *b = board;
	(void)pair;
	if (b->mute)
		return 0;
	return xp_output(&b->xp, 4 + channel);
}

static midi_queue_t *ops_midi(void *b) { return &((sc8820_t *)b)->midi; }
static void ops_set_midi_out(void *board, scemu_midi_out_fn fn, void *user)
{
	sc8820_t *b = board;
	b->midi_out = fn;
	b->midi_out_user = user;
}
static void ops_button(void *b, scemu_button_t button, bool down) { sc8820_button(b, button, down); }
static void ops_set_computer_switch(void *b, scemu_computer_switch_t sw) { ((sc8820_t *)b)->computer_switch = sw; }
static uint32_t ops_leds(const void *b) { return ((const sc8820_t *)b)->panel.leds; }

/* the flash is never written by the firmware: no settings memory */
static size_t ops_nvram_size(const void *b) { (void)b; return 0; }
static size_t ops_nvram_get(const void *b, void *buffer, size_t size) { (void)b; (void)buffer; (void)size; return 0; }
static bool ops_nvram_set(void *b, const void *buffer, size_t size) { (void)b; (void)buffer; (void)size; return false; }

static size_t ops_state_size(const void *b) { return sc8820_state_size(b); }
static size_t ops_state_save(const void *b, void *buffer, size_t size) { return sc8820_state_save(b, buffer, size); }
static bool ops_state_load(void *b, const void *buffer, size_t size) { return sc8820_state_load(b, buffer, size); }

const board_ops_t sc8820_board_ops =
{
	ops_validate_roms, ops_init, ops_release, ops_reset, ops_run_frame, ops_idle, ops_frame, ops_rom_id, ops_sample_rate,
	ops_output_count, ops_output,
	ops_midi, ops_set_midi_out,
	ops_button, ops_set_computer_switch, ops_leds, NULL, NULL, NULL,
	ops_nvram_size, ops_nvram_get, ops_nvram_set,
	ops_state_size, ops_state_save, ops_state_load,
};
