#include <stdlib.h>
#include <string.h>
#include "scemu_internal.h"
#include "wave_rom.h"

const char *sc55_validate_roms(scemu_model_t model, const scemu_roms_t *roms)
{
	if (model != SCEMU_MODEL_SC55)
		return "unknown model";
	if (!roms || !roms->program_rom)
		return "no program ROM";
	if (roms->program_rom_size != SC55_PROGRAM_ROM_SIZE)
		return "program ROM has the wrong size";
	if (!roms->boot_rom)
		return "no internal ROM";
	if (roms->boot_rom_size != SC55_INTERNAL_ROM_SIZE)
		return "internal ROM has the wrong size";
	if (roms->wave_rom_count != 3)
		return "wrong number of wave ROMs";
	for (int n = 0; n < 3; n++)
	{
		if (!roms->wave_rom[n])
			return "missing wave ROM";
		if (roms->wave_rom_size[n] != SC55_WAVE_ROM_SIZE)
			return "wave ROM has the wrong size";
	}
	return NULL;
}

/* ---------------------------------------------------------------- the gate array's interrupts */

/* IRQ1 is edge-triggered, so the gate array drops INTOUT with every acknowledge and raises it
   again while another enabled source is waiting */
static void update_irq1(sc55_t *b)
{
	h8500_set_irq(&b->cpu, H8500_IRQ1, (b->int_pending & ~b->int_mask) != 0);
}

static void ga_int(sc55_t *b, int source)
{
	b->int_pending |= (uint8_t)(1u << (source - 1));
	update_irq1(b);
}

/* the number of the lowest enabled source waiting; reading it acknowledges it */
static uint8_t ga_source(sc55_t *b)
{
	const uint8_t active = b->int_pending & (uint8_t)~b->int_mask;
	if (!active)
		return 0;
	int source = 1;
	while (!(active & (1u << (source - 1))))
		source++;
	b->int_pending &= (uint8_t)~(1u << (source - 1));
	h8500_set_irq(&b->cpu, H8500_IRQ1, false);
	update_irq1(b);
	return (uint8_t)source;
}

static void gp_irq(void *user, bool state)
{
	sc55_t *b = user;
	h8500_set_irq(&b->cpu, H8500_IRQ0, state);
}

/* the 8251's RXRDY reaches INT4 through an inverter, and the gate array latches the edge */
static void usart_changed(sc55_t *b)
{
	const bool ready = i8251_rxrdy(&b->usart);
	if (ready && !b->rxrdy)
		ga_int(b, 4);
	b->rxrdy = ready;
}

/* ---------------------------------------------------------------- the gate array's pins and registers */

/* SC3 powers the LCD unit, and the output stage's mute follows the same supply */
static bool powered(const sc55_t *b)
{
	return (b->scan & 0x08) != 0;
}

static void update_display(sc55_t *b)
{
	const bool on = b->lcd_powered && powered(b);
	if (b->lcd.out.display_on != on)
	{
		b->lcd.out.display_on = on;
		b->lcd.out.changed = true;
	}
}

/* an access to 0F000-0F0FF drives its low address byte onto SC7-SC0: rows in bits 2-0,
   active low, the LCD supply in bit 3 and the STANDBY, MUTE and ALL lamps in 4-6 */
static uint8_t scan(sc55_t *b, uint8_t address)
{
	b->scan = address;
	update_display(b);
	uint8_t data = 0xff;
	for (int row = 0; row < 3; row++)
		if (!(address & (1u << row)))
			data &= b->keys[row];
	return data;
}

static uint8_t ga_r(sc55_t *b, uint32_t offset)
{
	return offset == 6 ? ga_source(b) : 0xff;
}

static void ga_w(sc55_t *b, uint32_t offset, uint8_t data)
{
	switch (offset)
	{
	case 4:
		lcd_data(&b->lcd, data);
		b->lcd_done = SC55_LCD_DONE_FRAMES;
		break;
	case 5:
		lcd_command(&b->lcd, data);
		b->lcd_powered = b->lcd.out.display_on;
		update_display(b);
		b->lcd_done = SC55_LCD_DONE_FRAMES;
		break;
	case 7:
		b->int_mask = data;
		update_irq1(b);
		break;
	default:
		break;
	}
}

/* ---------------------------------------------------------------- the H8's bus */

enum { DEV_NONE, DEV_ROM, DEV_SRAM, DEV_USART, DEV_GP, DEV_SCAN, DEV_GA };

/* the program ROM decodes A17-A0; the SRAM takes A14-A0 */
static int decode(uint32_t address, uint32_t *offset)
{
	const uint32_t page = address >> 16;
	if (page == 0)
	{
		if (address < SC55_INTERNAL_ROM_SIZE)
		{
			*offset = address;
			return DEV_ROM;
		}
		if (address < 0x0d000)
		{
			*offset = address & 0x7fff;
			return DEV_SRAM;
		}
		if (address < 0x0d002)
		{
			*offset = address & 1;
			return DEV_USART;
		}
		if (address >= 0x0e000 && address < 0x0e040)
		{
			*offset = address & 0x3f;
			return DEV_GP;
		}
		if (address >= 0x0f000 && address < 0x0f100)
		{
			*offset = address & 0xff;
			return DEV_SCAN;
		}
		if (address >= 0x0f100 && address < 0x0f200)
		{
			*offset = address & 0xff;
			return DEV_GA;
		}
		return DEV_NONE;
	}
	if (page < 8)
	{
		*offset = address & 0x3ffff;
		return DEV_ROM;
	}
	return DEV_NONE;
}

static uint8_t bus_read8(void *user, uint32_t address)
{
	sc55_t *b = user;
	uint32_t offset;
	switch (decode(address, &offset))
	{
	case DEV_ROM:
		return address < SC55_INTERNAL_ROM_SIZE ? b->internal_rom[offset] : b->program_rom[offset];
	case DEV_SRAM:
		return b->sram[offset];
	case DEV_USART:
	{
		const uint8_t data = i8251_read(&b->usart, (int)offset);
		usart_changed(b);
		return data;
	}
	case DEV_GP:
		return gp_read(&b->gp, (uint8_t)offset);
	case DEV_SCAN:
		return scan(b, (uint8_t)offset);
	case DEV_GA:
		return ga_r(b, offset);
	default:
		return 0xff;
	}
}

static void bus_write8(void *user, uint32_t address, uint8_t data)
{
	sc55_t *b = user;
	uint32_t offset;
	switch (decode(address, &offset))
	{
	case DEV_SRAM:
		b->sram[offset] = data;
		break;
	case DEV_USART:
		i8251_write(&b->usart, (int)offset, data);
		usart_changed(b);
		break;
	case DEV_GP:
		gp_write(&b->gp, (uint8_t)offset, data);
		b->gp_written = b->frame;
		break;
	case DEV_SCAN:
		scan(b, (uint8_t)offset);
		break;
	case DEV_GA:
		ga_w(b, offset, data);
		break;
	default:
		break;
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

/* port 7 is tied to ground */
static uint8_t bus_read_port(void *user, int port)
{
	(void)user;
	return port == H8500_PORT7 ? 0x00 : 0xff;
}

static void bus_write_port(void *user, int port, uint8_t data, uint8_t ddr)
{
	(void)user;
	(void)port;
	(void)data;
	(void)ddr;
}

/* AN0-AN6 are the remote decoder's data lines, idle low; AN7 the backup battery at 3.2 V */
static uint16_t bus_read_adc(void *user, int channel)
{
	(void)user;
	return channel == 7 ? 0x2a0 : 0;
}

/* MIDI OUT is the SCI's transmitter */
static void bus_sci_tx(void *user, int channel, uint8_t byte)
{
	sc55_t *b = user;
	if (channel == H8500_SCI1 && b->midi_out)
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

bool sc55_init(sc55_t *b, scemu_model_t model, const scemu_roms_t *roms, const scemu_config_t *config)
{
	memset(b, 0, sizeof(*b));
	b->model = model;
	midi_queue_init(&b->midi, SC55_MIDI_PORTS, SC55_SAMPLE_RATE);
	jit_alloc_init(&b->jit, config);

	uint64_t id = 0xcbf29ce484222325ull;
	id = hash_add(id, roms->boot_rom, roms->boot_rom_size);
	id = hash_add(id, roms->program_rom, roms->program_rom_size);
	for (int n = 0; n < roms->wave_rom_count; n++)
		id = hash_add(id, roms->wave_rom[n], roms->wave_rom_size[n]);
	b->rom_id = id;

	b->internal_rom = malloc(SC55_INTERNAL_ROM_SIZE);
	b->program_rom = malloc(SC55_PROGRAM_ROM_SIZE);
	if (!b->internal_rom || !b->program_rom)
		return false;
	memcpy(b->internal_rom, roms->boot_rom, SC55_INTERNAL_ROM_SIZE);
	memcpy(b->program_rom, roms->program_rom, SC55_PROGRAM_ROM_SIZE);

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
	h8500_map(&b->cpu, 0x00000, SC55_INTERNAL_ROM_SIZE, b->internal_rom, false);
	h8500_map(&b->cpu, 0x08000, 0x5000, b->sram, true);
	h8500_map(&b->cpu, 0x10000, 0x30000, b->program_rom + 0x10000, false);
	h8500_map(&b->cpu, 0x40000, SC55_PROGRAM_ROM_SIZE, b->program_rom, false);
#ifdef SCEMU_H8500_JIT
	h8500_jit_attach(&b->cpu, &b->jit);
	if (config->h8500_interpreter || (getenv("SCEMU_H8500_JIT") && !atoi(getenv("SCEMU_H8500_JIT"))))
		b->cpu.jit_enabled = false;
#endif

	gp_link_t link = { gp_irq, b };
	gp_init(&b->gp, &link, false, b->wave_rom, b->wave_rom_size);

	lcd_init(&b->lcd);
	memset(b->keys, 0xff, sizeof(b->keys));
	return true;
}

void sc55_release(sc55_t *b)
{
	h8500_jit_detach(&b->cpu);
	free(b->wave_rom);
	free(b->program_rom);
	free(b->internal_rom);
	b->wave_rom = NULL;
	b->program_rom = NULL;
	b->internal_rom = NULL;
}

void sc55_reset(sc55_t *b)
{
	b->cpu_quarter_cycles = 0;
	b->gp_pair = 1;
	b->scan = 0xf7;
	b->int_mask = 0xff;
	b->int_pending = 0;
	b->lcd_done = 0;
	b->rxrdy = false;
	b->gp_written = 0;
	b->lcd_powered = false;
	b->frame = 0;
	midi_queue_reset(&b->midi);
	gp_reset(&b->gp);
	lcd_reset(&b->lcd);
	i8251_reset(&b->usart);
	h8500_reset(&b->cpu);
	h8500_set_irq(&b->cpu, H8500_IRQ0, false);
	h8500_set_irq(&b->cpu, H8500_IRQ1, false);
}

/* ---------------------------------------------------------------- the frame */

/* IN 1 is the SCI's receiver; IN 2 the 8251, which takes what the wire brings and loses a
   byte it has no room or no receiver for */
static bool midi_take(void *user, int port, uint8_t byte)
{
	sc55_t *b = user;
	if (port == 0)
	{
		if (b->cpu.sci[H8500_SCI1].rx_pending)
			return false;
		h8500_sci_rx(&b->cpu, H8500_SCI1, byte);
		return true;
	}
	i8251_receive(&b->usart, byte);
	usart_changed(b);
	return true;
}

bool sc55_idle(const sc55_t *b)
{
	return powered(b) && b->frame - b->gp_written >= SC55_IDLE_FRAMES;
}

void sc55_run_frame(sc55_t *b)
{
	midi_queue_deliver(&b->midi, (uint32_t)b->frame, midi_take, b);

	b->cpu_quarter_cycles += SC55_H8_QUARTER_CYCLES_PER_FRAME;
	int cycles = (int)(b->cpu_quarter_cycles >> 2);
	b->cpu_quarter_cycles &= 3;
	int ran = h8500_run(&b->cpu, cycles);
	if (ran > cycles)
		b->cpu_quarter_cycles -= (uint32_t)(ran - cycles) * 4;

	if (b->lcd_done && !--b->lcd_done)
		ga_int(b, 1);

	b->gp_pair ^= 1;
	if (b->gp_pair == 0)
		gp_run_frame(&b->gp);

	b->frame++;
}

void sc55_queue_midi(sc55_t *b, int port, uint8_t byte, uint32_t frame_offset)
{
	midi_queue_push(&b->midi, port, byte, (uint32_t)b->frame + frame_offset);
}

/* Matrix position of each key this panel has: strobe row and return bit, with
   bit 8 marking the positions it has at all.  The SC-55mkII's matrix, key for key. */
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

void sc55_button(sc55_t *b, scemu_button_t button, bool down)
{
	if (button < 0 || button >= SCEMU_BUTTON_COUNT || !BUTTON_POS[button])
		return;
	const int row = (BUTTON_POS[button] >> 4) & 3;
	const uint8_t bit = (uint8_t)(1u << (BUTTON_POS[button] & 7));
	if (down)
		b->keys[row] &= (uint8_t)~bit;
	else
		b->keys[row] |= bit;
}

/* ---------------------------------------------------------------- the board as the API sees it */

static const char *ops_validate_roms(scemu_model_t model, const scemu_roms_t *roms) { return sc55_validate_roms(model, roms); }
static bool ops_init(void *b, scemu_model_t model, const scemu_roms_t *roms, const scemu_config_t *config) { return sc55_init(b, model, roms, config); }
static void ops_release(void *b) { sc55_release(b); }
static void ops_reset(void *b) { sc55_reset(b); }
static void ops_run_frame(void *b) { sc55_run_frame(b); }
static bool ops_idle(const void *b) { return sc55_idle(b); }
static uint64_t ops_frame(const void *b) { return ((const sc55_t *)b)->frame; }
static uint64_t ops_rom_id(const void *b) { return ((const sc55_t *)b)->rom_id; }
static uint32_t ops_sample_rate(const void *b) { (void)b; return SC55_SAMPLE_RATE; }
static int ops_output_count(const void *b) { (void)b; return 1; }

/* the chip's word sits in the 20-bit mix as on the SC-55mkII, full scale 1 << 23 in the word
   the host is handed.  The DAC width this firmware selects adds a constant 1 << 12 to it,
   which the output stage's coupling capacitors never pass */
static int32_t ops_output(const void *board, int pair, int channel)
{
	const sc55_t *b = board;
	if (pair != 0 || !powered(b))
		return 0;
	int32_t word = gp_output(&b->gp, b->gp_pair, channel);
	if ((b->gp.output_config & 0x30) == 0x30)
		word -= 1 << 12;
	return word * 32;
}

static midi_queue_t *ops_midi(void *b) { return &((sc55_t *)b)->midi; }
static void ops_set_midi_out(void *board, scemu_midi_out_fn fn, void *user)
{
	sc55_t *b = board;
	b->midi_out = fn;
	b->midi_out_user = user;
}

static void ops_button(void *b, scemu_button_t button, bool down) { sc55_button(b, button, down); }
static void ops_set_computer_switch(void *b, scemu_computer_switch_t sw) { (void)b; (void)sw; }

static uint32_t ops_leds(const void *board)
{
	const sc55_t *b = board;
	const uint8_t lamps = (uint8_t)~b->scan;
	return (uint32_t)((((lamps >> 6) & 1u) << SCEMU_LED_ALL)
		| (((lamps >> 5) & 1u) << SCEMU_LED_MUTE)
		| (((lamps >> 4) & 1u) << SCEMU_LED_STANDBY));
}

static scemu_lcd_t *ops_lcd(void *b) { return &((sc55_t *)b)->lcd.out; }

static size_t ops_nvram_size(const void *b) { (void)b; return SC55_SRAM_SIZE; }
static size_t ops_nvram_get(const void *board, void *buffer, size_t size)
{
	if (size < SC55_SRAM_SIZE)
		return 0;
	memcpy(buffer, ((const sc55_t *)board)->sram, SC55_SRAM_SIZE);
	return SC55_SRAM_SIZE;
}
static bool ops_nvram_set(void *board, const void *buffer, size_t size)
{
	if (size != SC55_SRAM_SIZE)
		return false;
	memcpy(((sc55_t *)board)->sram, buffer, SC55_SRAM_SIZE);
	return true;
}

static size_t ops_state_size(const void *b) { return sc55_state_size(b); }
static size_t ops_state_save(const void *b, void *buffer, size_t size) { return sc55_state_save(b, buffer, size); }
static bool ops_state_load(void *b, const void *buffer, size_t size) { return sc55_state_load(b, buffer, size); }

const board_ops_t sc55_board_ops =
{
	ops_validate_roms, ops_init, ops_release, ops_reset, ops_run_frame, ops_idle, ops_frame, ops_rom_id, ops_sample_rate,
	ops_output_count, ops_output,
	ops_midi, ops_set_midi_out,
	ops_button, ops_set_computer_switch, ops_leds, ops_lcd, NULL, NULL,
	ops_nvram_size, ops_nvram_get, ops_nvram_set,
	ops_state_size, ops_state_save, ops_state_load,
};
