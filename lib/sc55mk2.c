#include <stdlib.h>
#include <string.h>
#include "scemu_internal.h"
#include "wave_rom.h"

const char *sc55mk2_validate_roms(scemu_model_t model, const scemu_roms_t *roms)
{
	if (model != SCEMU_MODEL_SC55MK2)
		return "unknown model";
	if (!roms || !roms->program_rom)
		return "no program ROM";
	if (roms->program_rom_size != SC55MK2_PROGRAM_ROM_SIZE)
		return "program ROM has the wrong size";
	if (!roms->boot_rom)
		return "no internal ROM";
	if (roms->boot_rom_size != SC55MK2_INTERNAL_ROM_SIZE)
		return "internal ROM has the wrong size";
	if (roms->wave_rom_count != 2)
		return "wrong number of wave ROMs";
	if (!roms->wave_rom[0] || !roms->wave_rom[1])
		return "missing wave ROM";
	if (roms->wave_rom_size[0] != 0x200000 || roms->wave_rom_size[1] != 0x100000)
		return "wave ROM has the wrong size";
	return NULL;
}

/* ---------------------------------------------------------------- wiring */

static void gp_irq(void *user, bool state)
{
	sc55mk2_t *b = user;
	h8500_set_irq(&b->cpu, H8500_IRQ0, state);
}

static void sub_attention(void *user)
{
	sc55mk2_t *b = user;
	if (((b->int_enable >> 4) & 1) && b->int_trigger == 0)
	{
		b->int_trigger = 5;
		h8500_set_irq(&b->cpu, H8500_IRQ1, true);
	}
}

static void sub_midi_out(void *user, uint8_t byte)
{
	sc55mk2_t *b = user;
	if (b->midi_out)
		b->midi_out(SCEMU_MIDI_IN_A, &byte, 1, b->midi_out_user);
}

/* ---------------------------------------------------------------- the GP-4's system registers */

static uint8_t sysreg_r(sc55mk2_t *b, uint32_t offset)
{
	if (offset != 2)
		return 0xff;
	const uint8_t trigger = b->int_trigger;
	b->int_trigger = 0;
	h8500_set_irq(&b->cpu, H8500_IRQ1, false);
	return trigger;
}

static void sysreg_w(sc55mk2_t *b, uint32_t offset, uint8_t data)
{
	switch (offset)
	{
	case 1:
	{
		b->sys_control = data;
		const bool on = b->lcd_powered && !(data & 0x01);
		if (b->lcd.out.display_on != on)
		{
			b->lcd.out.display_on = on;
			b->lcd.out.changed = true;
		}
		break;
	}
	case 2:
		b->int_enable = data;
		b->int_trigger = 0;
		h8500_set_irq(&b->cpu, H8500_IRQ1, false);
		break;
	case 4:
		lcd_command(&b->lcd, data);
		b->lcd_powered = b->lcd.out.display_on;
		b->lcd.out.display_on = b->lcd_powered && !(b->sys_control & 0x01);
		break;
	case 5:
		lcd_data(&b->lcd, data);
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- the H8's bus */

enum { DEV_NONE, DEV_ROM, DEV_SRAM, DEV_GP, DEV_SYSREG, DEV_SUB };

/* the program ROM's A18 is driven by CPU A19, so its address is (A19 << 18) | (A17..A0) */
static int decode(uint32_t address, uint32_t *offset)
{
	const uint32_t page = address >> 16;
	if (address < SC55MK2_INTERNAL_ROM_SIZE)
	{
		*offset = address;
		return DEV_ROM;
	}
	if (address < 0x0e000)
	{
		*offset = address - 0x08000;
		return DEV_SRAM;
	}
	if (address < 0x0e400)
	{
		*offset = address & 0x3f;
		return DEV_GP;
	}
	if (address < 0x0e408)
	{
		*offset = address - 0x0e400;
		return DEV_SYSREG;
	}
	if (address >= 0x0ec00 && address < 0x0ed00)
	{
		*offset = address - 0x0ec00;
		return DEV_SUB;
	}
	if (page == 0x0a || page == 0x0b)
	{
		*offset = address & 0x7fff;
		return DEV_SRAM;
	}
	if ((page >= 1 && page <= 4) || page == 8 || page == 9 || page == 0x0e || page == 0x0f)
	{
		*offset = ((address >> 19) << 18) | (address & 0x3ffff);
		return DEV_ROM;
	}
	return DEV_NONE;
}

static const uint8_t *rom_of(const sc55mk2_t *b, uint32_t address, uint32_t offset)
{
	return address < SC55MK2_INTERNAL_ROM_SIZE ? b->internal_rom + offset : b->program_rom + offset;
}

static uint8_t bus_read8(void *user, uint32_t address)
{
	sc55mk2_t *b = user;
	uint32_t offset;
	switch (decode(address, &offset))
	{
	case DEV_ROM:    return *rom_of(b, address, offset);
	case DEV_SRAM:   return b->sram[offset];
	case DEV_GP:     return gp_read(&b->gp, (uint8_t)offset);
	case DEV_SYSREG: return sysreg_r(b, offset);
	case DEV_SUB:    return sub55_hle_read(&b->sub, offset);
	default:         return 0;
	}
}

static void bus_write8(void *user, uint32_t address, uint8_t data)
{
	sc55mk2_t *b = user;
	uint32_t offset;
	switch (decode(address, &offset))
	{
	case DEV_SRAM:   b->sram[offset] = data; break;
	case DEV_GP:     gp_write(&b->gp, (uint8_t)offset, data); b->gp_written = b->frame; break;
	case DEV_SYSREG: sysreg_w(b, offset, data); break;
	case DEV_SUB:    sub55_hle_write(&b->sub, offset, data); break;
	default: break;
	}
}

static uint16_t bus_read16(void *user, uint32_t address)
{
	return (uint16_t)((bus_read8(user, address) << 8) | bus_read8(user, address + 1));
}

static void bus_write16(void *user, uint32_t address, uint16_t data)
{
	bus_write8(user, address, (uint8_t)(data >> 8));
	bus_write8(user, address + 1, (uint8_t)data);
}

static uint8_t bus_read_port(void *user, int port)
{
	(void)user;
	return port == H8500_PORT9 ? 0x02 : 0xff;
}

static void bus_write_port(void *user, int port, uint8_t data, uint8_t ddr)
{
	(void)user;
	(void)port;
	(void)data;
	(void)ddr;
}

/* the four positions of the rear selector as the resistor ladder presents them */
static const uint8_t COMPUTER_LADDER[4] = { 3, 1, 2, 0 };

static uint16_t bus_read_adc(void *user, int channel)
{
	sc55mk2_t *b = user;
	if (channel != 7)
		return 0;
	switch ((b->sys_control >> 2) & 7)
	{
	case 0: return 0x2a0;
	case 2: return (uint16_t)(COMPUTER_LADDER[b->computer_switch & 3] * 0x155);
	default: return 0;
	}
}

static void bus_sci_tx(void *user, int channel, uint8_t byte)
{
	(void)user;
	(void)channel;
	(void)byte;
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

bool sc55mk2_init(sc55mk2_t *b, scemu_model_t model, const scemu_roms_t *roms, const scemu_config_t *config)
{
	memset(b, 0, sizeof(*b));
	b->model = model;
	midi_queue_init(&b->midi, SC55MK2_MIDI_PORTS, SC55MK2_SAMPLE_RATE);
	jit_alloc_init(&b->jit, config);

	uint64_t id = 0xcbf29ce484222325ull;
	id = hash_add(id, roms->boot_rom, roms->boot_rom_size);
	id = hash_add(id, roms->program_rom, roms->program_rom_size);
	for (int n = 0; n < roms->wave_rom_count; n++)
		id = hash_add(id, roms->wave_rom[n], roms->wave_rom_size[n]);
	b->rom_id = id;

	b->internal_rom = malloc(SC55MK2_INTERNAL_ROM_SIZE);
	b->program_rom = malloc(SC55MK2_PROGRAM_ROM_SIZE);
	if (!b->internal_rom || !b->program_rom)
		return false;
	memcpy(b->internal_rom, roms->boot_rom, SC55MK2_INTERNAL_ROM_SIZE);
	memcpy(b->program_rom, roms->program_rom, SC55MK2_PROGRAM_ROM_SIZE);

	b->wave_rom_size = wave_rom_size(model);
	b->wave_rom = malloc(b->wave_rom_size);
	if (!b->wave_rom || !wave_rom_build(model, roms, b->wave_rom, b->wave_rom_size))
		return false;

	h8500_bus_t bus =
	{
		bus_read8, bus_write8, bus_read16, bus_write16,
		bus_read_port, bus_write_port, bus_read_adc, bus_sci_tx, b
	};
	h8500_init_model(&b->cpu, &bus, H8500_H8532);
	h8500_map(&b->cpu, 0x00000, SC55MK2_INTERNAL_ROM_SIZE, b->internal_rom, false);
	h8500_map(&b->cpu, 0x08000, 0x6000, b->sram, true);
	h8500_map(&b->cpu, 0x10000, 0x30000, b->program_rom + 0x10000, false);
	h8500_map(&b->cpu, 0x40000, 0x10000, b->program_rom, false);
	h8500_map(&b->cpu, 0x80000, 0x20000, b->program_rom + 0x40000, false);
	h8500_map(&b->cpu, 0xa0000, SC55MK2_SRAM_SIZE, b->sram, true);
	h8500_map(&b->cpu, 0xe0000, 0x20000, b->program_rom + 0x60000, false);
#ifdef SCEMU_H8500_JIT
	h8500_jit_attach(&b->cpu, &b->jit);
	if (config->h8500_interpreter || (getenv("SCEMU_H8500_JIT") && !atoi(getenv("SCEMU_H8500_JIT"))))
		b->cpu.jit_enabled = false;
#endif

	gp_link_t link = { gp_irq, b };
	gp_init(&b->gp, &link, true, b->wave_rom, b->wave_rom_size);

	lcd_init(&b->lcd);
	sub55_hle_init(&b->sub, SC55MK2_SAMPLE_RATE, sub_attention, sub_midi_out, b);
	return true;
}

void sc55mk2_release(sc55mk2_t *b)
{
	h8500_jit_detach(&b->cpu);
	free(b->wave_rom);
	free(b->program_rom);
	free(b->internal_rom);
	b->wave_rom = NULL;
	b->program_rom = NULL;
	b->internal_rom = NULL;
}

static bool sram_blank(const sc55mk2_t *b)
{
	for (size_t n = 0; n < SC55MK2_SRAM_SIZE; n++)
		if (b->sram[n])
			return false;
	return true;
}

void sc55mk2_reset(sc55mk2_t *b)
{
	b->cpu_quarter_cycles = 0;
	b->gp_pair = 1;
	b->sys_control = 0x03;
	b->int_enable = 0;
	b->int_trigger = 0;
	b->gp_written = 0;
	b->lcd_powered = false;
	b->frame = 0;
	midi_queue_reset(&b->midi);
	gp_reset(&b->gp);
	lcd_reset(&b->lcd);
	sub55_hle_reset(&b->sub);
	h8500_reset(&b->cpu);

	/* a machine whose battery SRAM has never been written runs the panel's own factory
	   setup: INSTRUMENT < and > held from the reset, then ALL at the prompt */
	b->factory = SC55MK2_FACTORY_NONE;
	b->factory_frames = 0;
	if (sram_blank(b))
	{
		b->factory = SC55MK2_FACTORY_PROMPT;
		sc55mk2_button(b, SCEMU_BUTTON_INSTRUMENT_LEFT, true);
		sc55mk2_button(b, SCEMU_BUTTON_INSTRUMENT_RIGHT, true);
	}
}

/* ---------------------------------------------------------------- the frame */

/* the rear selector decides which port the machine listens on: the MIDI position
   puts MIDI IN 1 on the sub's UART 2, the other three make the computer port the input */
static bool midi_take(void *user, int port, uint8_t byte)
{
	sc55mk2_t *b = user;
	const int source = (port == 0 && b->computer_switch != SCEMU_COMPUTER_MIDI)
		? SUB55_COMPUTER : (port == 0 ? SUB55_MIDI1 : SUB55_MIDI2);
	if (!sub55_hle_ready(&b->sub, source))
		return false;
	sub55_hle_midi_byte(&b->sub, source, byte);
	return true;
}

/* the analog mute released and the chip left alone: the machine has finished whatever
   it was doing */
static bool settled(const sc55mk2_t *b)
{
	return !(b->sys_control & 0x02) && b->frame - b->gp_written >= SC55MK2_IDLE_FRAMES;
}

bool sc55mk2_idle(const sc55mk2_t *b)
{
	return b->factory == SC55MK2_FACTORY_NONE && settled(b);
}

/* a battery SRAM handed to the machine replaces the blank one the setup was for */
static void factory_cancel(sc55mk2_t *b)
{
	if (b->factory == SC55MK2_FACTORY_PROMPT)
	{
		sc55mk2_button(b, SCEMU_BUTTON_INSTRUMENT_LEFT, false);
		sc55mk2_button(b, SCEMU_BUTTON_INSTRUMENT_RIGHT, false);
	}
	if (b->factory == SC55MK2_FACTORY_CONFIRM)
		sc55mk2_button(b, SCEMU_BUTTON_ALL, false);
	b->factory = SC55MK2_FACTORY_NONE;
	b->factory_frames = 0;
}

static void factory_step(sc55mk2_t *b)
{
	switch (b->factory)
	{
	case SC55MK2_FACTORY_PROMPT:
		if (!settled(b))
			break;
		sc55mk2_button(b, SCEMU_BUTTON_INSTRUMENT_LEFT, false);
		sc55mk2_button(b, SCEMU_BUTTON_INSTRUMENT_RIGHT, false);
		sc55mk2_button(b, SCEMU_BUTTON_ALL, true);
		b->factory_frames = SC55MK2_KEY_FRAMES;
		b->factory = SC55MK2_FACTORY_CONFIRM;
		break;
	case SC55MK2_FACTORY_CONFIRM:
		if (--b->factory_frames)
			break;
		sc55mk2_button(b, SCEMU_BUTTON_ALL, false);
		b->factory_frames = SC55MK2_SAMPLE_RATE;
		b->factory = SC55MK2_FACTORY_REBUILD;
		break;
	case SC55MK2_FACTORY_REBUILD:
		if (!settled(b))
		{
			b->factory_frames = SC55MK2_IDLE_FRAMES;
			b->factory = SC55MK2_FACTORY_SETTLE;
		}
		else if (!--b->factory_frames)
			b->factory = SC55MK2_FACTORY_NONE;
		break;
	case SC55MK2_FACTORY_SETTLE:
		if (!settled(b))
			b->factory_frames = SC55MK2_IDLE_FRAMES;
		else if (!--b->factory_frames)
			b->factory = SC55MK2_FACTORY_NONE;
		break;
	case SC55MK2_FACTORY_NONE:
		break;
	}
}

void sc55mk2_run_frame(sc55mk2_t *b)
{
	midi_queue_deliver(&b->midi, (uint32_t)b->frame, midi_take, b);

	b->cpu_quarter_cycles += SC55MK2_H8_QUARTER_CYCLES_PER_FRAME;
	int cycles = (int)(b->cpu_quarter_cycles >> 2);
	b->cpu_quarter_cycles &= 3;
	int ran = h8500_run(&b->cpu, cycles);
	if (ran > cycles)
		b->cpu_quarter_cycles -= (uint32_t)(ran - cycles) * 4;

	sub55_hle_frame(&b->sub);

	b->gp_pair ^= 1;
	if (b->gp_pair == 0)
		gp_run_frame(&b->gp);

	b->frame++;

	if (b->factory != SC55MK2_FACTORY_NONE)
		factory_step(b);
}

void sc55mk2_queue_midi(sc55mk2_t *b, int port, uint8_t byte, uint32_t frame_offset)
{
	midi_queue_push(&b->midi, port, byte, (uint32_t)b->frame + frame_offset);
}

/* Matrix position of each key this panel has: strobe row and return bit, with
   bit 8 marking the positions it has at all. */
#define KEY(row, bit) (0x100u | ((row) << 4) | (bit))

static const uint16_t BUTTON_POS[SCEMU_BUTTON_COUNT] =
{
	[SCEMU_BUTTON_ALL] = KEY(0, 6),
	[SCEMU_BUTTON_MUTE] = KEY(0, 5),
	[SCEMU_BUTTON_PART_LEFT] = KEY(2, 6),
	[SCEMU_BUTTON_PART_RIGHT] = KEY(1, 6),
	[SCEMU_BUTTON_INSTRUMENT_LEFT] = KEY(0, 3),
	[SCEMU_BUTTON_INSTRUMENT_RIGHT] = KEY(0, 4),
	[SCEMU_BUTTON_LEVEL_LEFT] = KEY(2, 4),
	[SCEMU_BUTTON_LEVEL_RIGHT] = KEY(2, 5),
	[SCEMU_BUTTON_PAN_LEFT] = KEY(1, 4),
	[SCEMU_BUTTON_PAN_RIGHT] = KEY(1, 5),
	[SCEMU_BUTTON_REVERB_LEFT] = KEY(2, 2),
	[SCEMU_BUTTON_REVERB_RIGHT] = KEY(2, 3),
	[SCEMU_BUTTON_CHORUS_LEFT] = KEY(1, 2),
	[SCEMU_BUTTON_CHORUS_RIGHT] = KEY(1, 3),
	[SCEMU_BUTTON_KEY_SHIFT_LEFT] = KEY(2, 0),
	[SCEMU_BUTTON_KEY_SHIFT_RIGHT] = KEY(2, 1),
	[SCEMU_BUTTON_MIDI_CH_LEFT] = KEY(1, 0),
	[SCEMU_BUTTON_MIDI_CH_RIGHT] = KEY(1, 1),
	[SCEMU_BUTTON_POWER] = KEY(0, 0),
};

void sc55mk2_button(sc55mk2_t *b, scemu_button_t button, bool down)
{
	if (button < 0 || button >= SCEMU_BUTTON_COUNT || !BUTTON_POS[button])
		return;
	sub55_hle_set_key(&b->sub, (BUTTON_POS[button] >> 4) & 3, BUTTON_POS[button] & 7, down);
}

/* ---------------------------------------------------------------- the board as the API sees it */

static const char *ops_validate_roms(scemu_model_t model, const scemu_roms_t *roms) { return sc55mk2_validate_roms(model, roms); }
static bool ops_init(void *b, scemu_model_t model, const scemu_roms_t *roms, const scemu_config_t *config) { return sc55mk2_init(b, model, roms, config); }
static void ops_release(void *b) { sc55mk2_release(b); }
static void ops_reset(void *b) { sc55mk2_reset(b); }
static void ops_run_frame(void *b) { sc55mk2_run_frame(b); }
static bool ops_idle(const void *b) { return sc55mk2_idle(b); }
static uint64_t ops_frame(const void *b) { return ((const sc55mk2_t *)b)->frame; }
static uint64_t ops_rom_id(const void *b) { return ((const sc55mk2_t *)b)->rom_id; }
static uint32_t ops_sample_rate(const void *b) { (void)b; return SC55MK2_SAMPLE_RATE; }
static int ops_output_count(const void *b) { (void)b; return 1; }

/* the chip's word sits in the 20-bit mix left-aligned by the output stage's shift, so
   full scale is 1 << 18 there and 1 << 23 in the word the host is handed */
static int32_t ops_output(const void *board, int pair, int channel)
{
	const sc55mk2_t *b = board;
	if (pair != 0 || (b->sys_control & 0x02))
		return 0;
	return gp_output(&b->gp, b->gp_pair, channel) * 32;
}

static midi_queue_t *ops_midi(void *b) { return &((sc55mk2_t *)b)->midi; }
static void ops_set_midi_out(void *board, scemu_midi_out_fn fn, void *user)
{
	sc55mk2_t *b = board;
	b->midi_out = fn;
	b->midi_out_user = user;
}


static void ops_button(void *b, scemu_button_t button, bool down) { sc55mk2_button(b, button, down); }
static void ops_set_computer_switch(void *b, scemu_computer_switch_t sw) { ((sc55mk2_t *)b)->computer_switch = sw; }

static uint32_t ops_leds(const void *board)
{
	const sc55mk2_t *b = board;
	const uint8_t lamps = sub55_hle_leds(&b->sub);
	return (uint32_t)((((lamps >> 0) & 1u) << SCEMU_LED_ALL)
		| (((lamps >> 1) & 1u) << SCEMU_LED_MUTE)
		| (((lamps >> 2) & 1u) << SCEMU_LED_STANDBY));
}

static scemu_lcd_t *ops_lcd(void *b) { return &((sc55mk2_t *)b)->lcd.out; }

static size_t ops_nvram_size(const void *b) { (void)b; return SC55MK2_SRAM_SIZE; }
static size_t ops_nvram_get(const void *board, void *buffer, size_t size)
{
	if (size < SC55MK2_SRAM_SIZE)
		return 0;
	memcpy(buffer, ((const sc55mk2_t *)board)->sram, SC55MK2_SRAM_SIZE);
	return SC55MK2_SRAM_SIZE;
}
static bool ops_nvram_set(void *board, const void *buffer, size_t size)
{
	sc55mk2_t *b = board;
	if (size != SC55MK2_SRAM_SIZE)
		return false;
	memcpy(b->sram, buffer, SC55MK2_SRAM_SIZE);
	factory_cancel(b);
	return true;
}

static size_t ops_state_size(const void *b) { return sc55mk2_state_size(b); }
static size_t ops_state_save(const void *b, void *buffer, size_t size) { return sc55mk2_state_save(b, buffer, size); }
static bool ops_state_load(void *b, const void *buffer, size_t size) { return sc55mk2_state_load(b, buffer, size); }

const board_ops_t sc55mk2_board_ops =
{
	ops_validate_roms, ops_init, ops_release, ops_reset, ops_run_frame, ops_idle, ops_frame, ops_rom_id, ops_sample_rate,
	ops_output_count, ops_output,
	ops_midi, ops_set_midi_out,
	ops_button, ops_set_computer_switch, ops_leds, ops_lcd, NULL, NULL,
	ops_nvram_size, ops_nvram_get, ops_nvram_set,
	ops_state_size, ops_state_save, ops_state_load,
};
