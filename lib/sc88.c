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

static void cpu_irq(void *user, int line, bool state)
{
	sc88_t *b = user;
	h8500_set_irq(&b->cpu, line, state);
}

static void ga_irq(void *user, bool state) { cpu_irq(user, H8500_IRQ0, state); }
static void xp_irq(void *user, bool state) { cpu_irq(user, H8500_IRQ1, state); }
static void sub_irq(void *user, bool state) { cpu_irq(user, H8500_IRQ2, state); }

static void sub_midi_out(void *user, uint8_t byte)
{
	sc88_t *b = user;
	if (b->midi_out)
		b->midi_out(&byte, 1, b->midi_out_user);
}

static void xp_serial_out(void *user, int channel, int32_t word)
{
	sc88_t *b = user;
	if (!b->has_lsp)
		return;
	lsp_serial_write(&b->lsp, channel, word);
	if (channel == 1)
		lsp_run_sample(&b->lsp);
}

static int32_t xp_serial_in(void *user, int channel)
{
	sc88_t *b = user;
	if (!b->has_lsp)
		return 0;
	return -(lsp_serial_read(&b->lsp, channel) >> 9);
}

static uint8_t bus_read8(void *user, uint32_t address)
{
	sc88_t *b = user;
	if (address < b->program_rom_size)
		return b->program_rom[address];
	return 0xff;
}

static void bus_write8(void *user, uint32_t address, uint8_t data)
{
	(void)user;
	(void)address;
	(void)data;
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
	sc88_t *b = user;
	if (port == H8500_PORT4)
		return b->port4;
	return 0xff;
}

static void bus_write_port(void *user, int port, uint8_t data, uint8_t ddr)
{
	sc88_t *b = user;
	if (port == H8500_PORT4)
	{
		b->port4 = (uint8_t)((b->port4 & ~ddr) | (data & ddr));
		b->mute = (b->port4 & 0x04) != 0;
	}
}

static uint16_t bus_read_adc(void *user, int channel)
{
	sc88_t *b = user;
	if (channel == 0)
		return 0x3ff;
	return (uint16_t)(0x3ff - 0x155 * (int)b->computer_switch);
}

static void bus_sci_tx(void *user, int channel, uint8_t byte)
{
	sc88_t *b = user;
	if (channel == 0 && b->midi_out)
		b->midi_out(&byte, 1, b->midi_out_user);
}

bool sc88_init(sc88_t *b, scemu_model_t model, const scemu_roms_t *roms, const scemu_config_t *config)
{
	memset(b, 0, sizeof(*b));
	b->model = model;
	jit_alloc_init(&b->jit, config);

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

	xp_link_t link = { xp_irq, xp_serial_out, xp_serial_in, b };
	if (!xp_init(&b->xp, &link, &b->jit, b->wave_rom, b->wave_rom_size))
		return false;

	b->has_lsp = (model == SCEMU_MODEL_SC88PRO || model == SCEMU_MODEL_VEGSPRO);
	b->has_panel = (model != SCEMU_MODEL_VEGSPRO);
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
	b->port4 = 0;
	b->mute = true;
	b->leds = 0;
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
	(void)b;
	return true;
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

/* Matrix position of each button: row (strobe) and bit (return line). */
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
