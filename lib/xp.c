#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stddef.h>
#include "xp.h"

static const int16_t interp_weights[3][128] = {
	{
		3385, 3401, 3417, 3432, 3448, 3463, 3478, 3492, 3506, 3521, 3534, 3548, 3562, 3575, 3588, 3601,
		3614, 3626, 3638, 3650, 3662, 3673, 3685, 3696, 3707, 3718, 3728, 3739, 3749, 3759, 3768, 3778,
		3787, 3796, 3805, 3814, 3823, 3831, 3839, 3847, 3855, 3863, 3870, 3878, 3885, 3892, 3899, 3905,
		3912, 3918, 3924, 3930, 3936, 3942, 3948, 3953, 3958, 3963, 3968, 3973, 3978, 3983, 3987, 3991,
		3995, 4000, 4004, 4007, 4011, 4015, 4018, 4022, 4025, 4028, 4031, 4034, 4037, 4040, 4042, 4045,
		4047, 4050, 4052, 4054, 4057, 4059, 4061, 4063, 4064, 4066, 4068, 4070, 4071, 4073, 4074, 4076,
		4077, 4078, 4079, 4081, 4082, 4083, 4084, 4085, 4086, 4086, 4087, 4088, 4089, 4089, 4090, 4091,
		4091, 4092, 4092, 4093, 4093, 4094, 4094, 4094, 4094, 4095, 4095, 4095, 4095, 4095, 4095, 4095,
	},
	{
		 710,  726,  742,  758,  775,  792,  809,  826,  844,  861,  879,  897,  915,  933,  952,  971,
		 990, 1009, 1028, 1047, 1067, 1087, 1106, 1126, 1147, 1167, 1188, 1208, 1229, 1250, 1271, 1292,
		1314, 1335, 1357, 1379, 1400, 1423, 1445, 1467, 1489, 1512, 1534, 1557, 1580, 1602, 1625, 1648,
		1671, 1695, 1718, 1741, 1764, 1788, 1811, 1835, 1858, 1882, 1906, 1929, 1953, 1977, 2000, 2024,
		2048, 2069, 2095, 2119, 2143, 2166, 2190, 2214, 2237, 2261, 2284, 2308, 2331, 2355, 2378, 2401,
		2425, 2448, 2471, 2494, 2517, 2539, 2562, 2585, 2607, 2630, 2652, 2674, 2696, 2718, 2740, 2762,
		2783, 2805, 2826, 2847, 2868, 2889, 2910, 2931, 2951, 2971, 2991, 3011, 3031, 3051, 3070, 3089,
		3108, 3127, 3146, 3164, 3182, 3200, 3218, 3236, 3253, 3271, 3288, 3304, 3321, 3338, 3354, 3370,
	},
	{
		   0,    0,    0,    1,    1,    1,    2,    2,    3,    3,    3,    4,    4,    5,    5,    6,
		   6,    7,    8,    8,    9,   10,   10,   11,   12,   13,   14,   15,   16,   17,   18,   19,
		  20,   22,   23,   24,   26,   27,   29,   30,   32,   34,   36,   38,   40,   42,   44,   46,
		  49,   51,   53,   56,   59,   62,   65,   68,   71,   74,   77,   81,   84,   88,   92,   96,
		 100,  104,  109,  113,  118,  122,  127,  132,  137,  143,  148,  154,  160,  165,  171,  178,
		 184,  191,  197,  204,  211,  219,  226,  234,  241,  249,  257,  266,  274,  283,  292,  301,
		 310,  319,  329,  339,  349,  359,  369,  380,  391,  402,  413,  424,  436,  448,  460,  472,
		 484,  497,  510,  523,  536,  549,  563,  577,  591,  605,  619,  634,  648,  663,  679,  694,
	},
};

static const uint32_t hold_masks[4] = { 0, 7, 31, 127 };

static inline int32_t wrap_add(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static inline int32_t clamp24(int64_t v) { return (int32_t)(v > 0x7fffff ? 0x7fffff : v < -0x800000 ? -0x800000 : v); }
static inline int32_t wrap24(int32_t v) { return (int32_t)((uint32_t)v << 8) >> 8; }
static inline int32_t min32(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t max32(int32_t a, int32_t b) { return a > b ? a : b; }
static inline bool bit(uint32_t v, int n) { return (v >> n) & 1; }

/* ---------------------------------------------------------------- ramps */

static int ramp_rate(const xp_ramp_t *r) { return r->control & 0xfff; }
static uint32_t ramp_hold_mask(const xp_ramp_t *r) { return hold_masks[(r->control >> 12) & 3]; }

static xp_law_t ramp_control_law(const xp_ramp_t *r)
{
	switch (r->control >> 14)
	{
	case 0: return XP_LAW_EXPONENTIAL;
	case 1: return XP_LAW_LINEAR;
	default: return XP_LAW_S_CURVE;
	}
}

static void ramp_arm(xp_ramp_t *r, xp_law_t law)
{
	switch (law)
	{
	case XP_LAW_LINEAR:
		r->step = (int32_t)(((int64_t)r->target - r->current) * ramp_rate(r) >> 13);
		if (r->step == 0 && r->target > r->current)
			r->step = 1;
		r->active = r->current != r->target;
		break;
	case XP_LAW_EXPONENTIAL:
		r->active = r->current != r->target;
		break;
	case XP_LAW_S_CURVE:
		r->target = 0;
		r->step = 0;
		r->midpoint = r->current / 2;
		r->active = r->current != 0;
		break;
	}
}

static void ramp_seed(xp_ramp_t *r, int32_t value, xp_law_t law)
{
	r->current = value;
	r->previous = value;
	r->counter = 0;
	r->accumulator = (int32_t)((uint32_t)value << 10);
	ramp_arm(r, law);
}

static void ramp_retarget(xp_ramp_t *r, int32_t value, xp_law_t law)
{
	r->target = value;
	r->accumulator = (int32_t)((uint32_t)r->current << 10);
	ramp_arm(r, law);
}

static void ramp_configure(xp_ramp_t *r, uint16_t value, xp_law_t law)
{
	r->control = value;
	ramp_arm(r, law);
}

static bool ramp_update(xp_ramp_t *r, xp_law_t law)
{
	r->previous = r->current;
	if (!r->active)
		return false;
	r->counter++;
	if (r->counter & ramp_hold_mask(r))
		return false;
	switch (law)
	{
	case XP_LAW_LINEAR:
		r->current += r->step;
		r->current = (r->step > 0) ? min32(r->current, r->target) : max32(r->current, r->target);
		if (r->current == r->target)
		{
			r->step = 0;
			r->active = false;
		}
		break;
	case XP_LAW_EXPONENTIAL:
	{
		const int32_t error = (int16_t)(((int32_t)((uint32_t)r->target << 10) - r->accumulator) >> 13);
		int32_t delta = error * ramp_rate(r);
		delta = (delta < 0) ? min32(delta, -0x400) : max32(delta, 0x400);
		r->accumulator += delta;
		r->current = r->accumulator >> 10;
		r->active = r->current != r->target;
		break;
	}
	case XP_LAW_S_CURVE:
		if (r->current > r->midpoint)
		{
			r->step -= ramp_rate(r);
			r->current += r->step;
		}
		else
		{
			r->step += ramp_rate(r);
			r->current = (r->step > 0) ? 0 : r->current + r->step;
		}
		if (r->current == 0)
			r->active = false;
		break;
	}
	return !r->active;
}

static int32_t ramp_value_at(const xp_ramp_t *r, int phase, int period)
{
	return r->previous + (int32_t)(((int64_t)(r->current - r->previous) * (phase + 1)) / period);
}

static int16_t ramp_coefficient_at(const xp_ramp_t *r, int phase, int period)
{
	int32_t v = ramp_value_at(r, phase, period) >> 3;
	return (int16_t)(v > 0x7fff ? 0x7fff : v < -0x8000 ? -0x8000 : v);
}

/* ---------------------------------------------------------------- device */

static int32_t exp_decode(const xp_t *xp, int32_t value)
{
	if (value == 0)
		return 0;
	const int index = (value >> 6) & 0xff;
	const int fraction = value & 0x3f;
	int32_t v = xp->exp_table[index] * (64 - fraction) + xp->exp_table[index + 1] * fraction;
	v = (v + (v < 0 ? 63 : 0)) >> 6;
	return v >> (15 - ((value >> 14) & 15));
}

bool xp_init(xp_t *xp, const xp_link_t *link, jit_alloc_t *jit, const uint8_t *wave, size_t wave_size, uint32_t chip_size)
{
	memset(xp, 0, sizeof(*xp));
	xp->link = *link;
	xp->jit = jit;
	xp->wave = wave;
	xp->wave_size = wave_size;
	xp->wave_chip_size = chip_size;
	xp->eram = calloc(XP_ERAM_SIZE, sizeof(int32_t));
	if (!xp->eram)
		return false;
	for (int i = 0; i <= 256; i++)
		xp->exp_table[i] = (int32_t)floor(exp2(17.0 + i / 256.0));
	return true;
}

void xp_release(xp_t *xp)
{
	jit_code_free(xp->jit, &xp->code[0]);
	jit_code_free(xp->jit, &xp->code[1]);
	xp->frame = NULL;
	free(xp->eram);
	xp->eram = NULL;
}

static void update_int(xp_t *xp)
{
	const bool state = xp->irq_count != 0;
	if (state != xp->int_state)
	{
		xp->int_state = state;
		xp->link.irq(xp->link.user, state);
	}
}

void xp_reset(xp_t *xp)
{
	memset(xp->regs, 0, sizeof(xp->regs));
	memset(xp->eram, 0, XP_ERAM_SIZE * sizeof(int32_t));
	memset(xp->bus, 0, sizeof(xp->bus));
	memset(xp->iram, 0, sizeof(xp->iram));
	memset(xp->iram_ramping, 0, sizeof(xp->iram_ramping));
	memset(xp->voices, 0, sizeof(xp->voices));
	memset(&xp->dsp, 0, sizeof(xp->dsp));
	xp->program_dirty = true;
	xp->dsp_enabled = false;
	xp->run_mask = 0;
	xp->read_latch = 0;
	xp->frame_counter = 0;
	xp->noise = 0x2545f491;
	xp->irq_head = 0;
	xp->irq_count = 0;
	update_int(xp);
}

static void raise_irq(xp_t *xp, int voice, int reason)
{
	if (xp->irq_count == XP_VOICES)
		return;
	xp->irq_queue[(xp->irq_head + xp->irq_count) % XP_VOICES] = (uint16_t)((voice << 8) | reason);
	xp->irq_count++;
	update_int(xp);
}

/* ---------------------------------------------------------------- host interface */

static uint32_t page(const xp_t *xp, int voice, int index)
{
	const int word = ((index & 0xff) << 7) | ((voice & 63) << 1);
	return ((uint32_t)xp->regs[word] << 16) | xp->regs[word | 1];
}

static uint16_t send(const xp_t *xp, int voice, int bank)
{
	return xp->regs[(XP_SEND_BASE >> 1) + (bank & 3) * 64 + (voice & 63)];
}

static inline uint8_t wave_byte(const xp_t *xp, uint32_t address)
{
	const uint32_t chip = address >> 24;
	const uint32_t offset = address & 0xffffff;
	if (offset >= xp->wave_chip_size)
		return 0;
	const size_t n = (size_t)chip * xp->wave_chip_size + offset;
	return n < xp->wave_size ? xp->wave[n] : 0;
}

static inline uint8_t rom_byte(const xp_t *xp, const xp_voice_t *v, uint32_t offset)
{
	return wave_byte(xp, (v->region << 20) | (offset & 0xfffff));
}

static bool ramp_current(const xp_voice_t *v, int index, uint32_t *value)
{
	switch (index)
	{
	case XP_PAGE_PITCH_SEED: *value = (uint32_t)v->pitch.current; return true;
	case XP_PAGE_TVF_SEED:   *value = (uint32_t)v->tvf.current; return true;
	case XP_PAGE_TVA2_SEED:  *value = (uint32_t)v->tva2.current; return true;
	case XP_PAGE_TVA1_SEED:  *value = (uint32_t)v->tva1.current; return true;
	case XP_PAGE_RESO_SEED:  *value = (uint32_t)v->reso.current << 2; return true;
	default:                 return false;
	}
}

static int iram_word(uint32_t address)
{
	return (address < XP_IRAM3_BASE ? 0 : 0x40) + (int)((address - XP_IRAM_BASE) >> 2);
}

static void load_latch(xp_t *xp, uint32_t address)
{
	const xp_voice_t *v = &xp->voices[(address >> 2) & 63];
	if (address >= XP_IRAM3_BASE + 8 && address < XP_IRAM3_BASE + 12)
		xp->read_latch = xp->noise;
	else if (address < XP_CRAM_BASE && ramp_current(v, address >> 8, &xp->read_latch))
		;
	else if (address >= XP_CRAM_BASE && address < XP_IRAM_BASE)
		xp->read_latch = xp->regs[address >> 1];
	else if (address >= XP_IRAM_BASE && address < XP_IRAM3_TARGET_BASE)
		xp->read_latch = (uint32_t)xp->iram[iram_word(address)];
	else
		xp->read_latch = ((uint32_t)xp->regs[(address >> 1) & ~1] << 16) | xp->regs[(address >> 1) | 1];
}

uint16_t xp_read(xp_t *xp, uint32_t offset)
{
	const uint32_t address = (offset << 1) & 0x3ffe;
	uint16_t data = xp->regs[address >> 1];
	if (address < XP_RUN_MASK)
		load_latch(xp, address);
	else if (address < XP_SEND_BASE)
	{
		switch (address)
		{
		case XP_READBACK_LOW:
			data = xp->read_latch & 0xffff;
			break;
		case XP_READBACK_HIGH:
			data = (uint16_t)(xp->read_latch >> 16);
			break;
		case XP_IRQ_STATUS:
			data = xp->irq_count ? xp->irq_queue[xp->irq_head] : 0;
			break;
		case XP_IRQ_ACK:
			if (xp->irq_count)
			{
				xp->irq_head = (xp->irq_head + 1) % XP_VOICES;
				xp->irq_count--;
			}
			update_int(xp);
			data = 0;
			break;
		}
	}
	else if (address >= XP_ROM_WINDOW)
	{
		const uint32_t word = ((uint32_t)(xp->regs[XP_ROM_BANK >> 1] & 0x7f) << 20)
			| ((uint32_t)(xp->regs[XP_ROM_PAGE >> 1] & 0x7ff) << 9) | ((address - XP_ROM_WINDOW) >> 1);
		data = (uint16_t)(wave_byte(xp, word * 2) | (wave_byte(xp, word * 2 + 1) << 8));
	}
	return data;
}

static void write_page(xp_t *xp, int n, int index, uint32_t value)
{
	xp_voice_t *v = &xp->voices[n];
	switch (index)
	{
	case XP_PAGE_CONTROL:
		if (bit(value, 15))
			v->start_pending = 1;
		break;
	case XP_PAGE_PITCH_SEED:    ramp_seed(&v->pitch, (int32_t)value, XP_LAW_LINEAR); break;
	case XP_PAGE_PITCH_TARGET:  ramp_retarget(&v->pitch, (int32_t)value, XP_LAW_LINEAR); break;
	case XP_PAGE_PITCH_CONTROL: ramp_configure(&v->pitch, (uint16_t)value, XP_LAW_LINEAR); break;
	case XP_PAGE_TVF_SEED:      ramp_seed(&v->tvf, exp_decode(xp, (int32_t)value), XP_LAW_LINEAR); break;
	case XP_PAGE_TVF_TARGET:    ramp_retarget(&v->tvf, exp_decode(xp, (int32_t)value), XP_LAW_LINEAR); break;
	case XP_PAGE_TVF_CONTROL:   ramp_configure(&v->tvf, (uint16_t)value, XP_LAW_LINEAR); break;
	case XP_PAGE_RESO_SEED:     ramp_seed(&v->reso, (int32_t)value >> 2, XP_LAW_EXPONENTIAL); break;
	case XP_PAGE_RESO_TARGET:   ramp_retarget(&v->reso, (int32_t)value, XP_LAW_EXPONENTIAL); break;
	case XP_PAGE_RESO_CONTROL:  ramp_configure(&v->reso, (uint16_t)value, XP_LAW_EXPONENTIAL); break;
	case XP_PAGE_TVA2_SEED:     ramp_seed(&v->tva2, (int32_t)value, XP_LAW_EXPONENTIAL); break;
	case XP_PAGE_TVA2_TARGET:   ramp_retarget(&v->tva2, (int32_t)value, XP_LAW_EXPONENTIAL); break;
	case XP_PAGE_TVA2_CONTROL:  ramp_configure(&v->tva2, (uint16_t)value, XP_LAW_EXPONENTIAL); break;
	case XP_PAGE_TVA1_SEED:     ramp_seed(&v->tva1, (int32_t)value, ramp_control_law(&v->tva1)); break;
	case XP_PAGE_TVA1_TARGET:   ramp_retarget(&v->tva1, (int32_t)value, ramp_control_law(&v->tva1)); break;
	case XP_PAGE_TVA1_CONTROL:
		v->tva1.control = (uint16_t)value;
		ramp_arm(&v->tva1, ramp_control_law(&v->tva1));
		if (ramp_control_law(&v->tva1) == XP_LAW_S_CURVE && !v->tva1.active && !v->done_reported)
		{
			v->done_reported = 1;
			raise_irq(xp, n, XP_IRQ_VOICE_DONE);
		}
		break;
	default:
		break;
	}
}

static void write_run_mask(xp_t *xp, int word, uint16_t data)
{
	const uint64_t before = xp->run_mask;
	xp->run_mask = (xp->run_mask & ~((uint64_t)0xffff << (word * 16))) | ((uint64_t)data << (word * 16));
	for (int n = word * 16; n < word * 16 + 16; n++)
	{
		if (((before >> n) & 1) && !((xp->run_mask >> n) & 1))
		{
			xp_voice_t *v = &xp->voices[n];
			v->reading = 0;
			v->predictor = 0;
			v->filter_low = 0;
			v->filter_band = 0;
		}
	}
}

static void write_iram(xp_t *xp, int word, uint32_t value)
{
	xp->iram[word & 0xff] = (int32_t)(value << 8) >> 8;
	xp->iram_ramping[word & 0xff] = 0;
}

void xp_write(xp_t *xp, uint32_t offset, uint16_t data, uint16_t mask)
{
	const uint32_t address = (offset << 1) & 0x3ffe;
	uint16_t *reg = &xp->regs[address >> 1];
	*reg = (uint16_t)((*reg & ~mask) | (data & mask));

	if (address < XP_CRAM_BASE)
	{
		if (bit(address, 1))
		{
			const int voice = (address >> 2) & 63;
			const int index = address >> 8;
			write_page(xp, voice, index, page(xp, voice, index));
		}
	}
	else if (address < XP_IRAM_BASE)
		xp->program_dirty = true;
	else if (address < XP_IRAM3_TARGET_BASE)
	{
		if (bit(address, 1))
			write_iram(xp, iram_word(address), ((uint32_t)xp->regs[(address >> 1) & ~1] << 16) | xp->regs[address >> 1]);
	}
	else if (address < XP_PRAM_BASE)
		xp->iram_ramping[0xc0 + ((address & 0xff) >> 1)] = 1;
	else if (address < XP_RUN_MASK)
		xp->program_dirty = true;
	else if (address < 0x3908)
		write_run_mask(xp, (int)((address - XP_RUN_MASK) >> 1), xp->regs[address >> 1]);
}

void xp_set_serial_words(xp_t *xp, int left, int right)
{
	xp->serial_out_word[0] = (uint8_t)left;
	xp->serial_out_word[1] = (uint8_t)right;
}

/* ---------------------------------------------------------------- address generator and DPCM */

typedef struct address_step
{
	uint32_t address;
	bool backward;
	bool stopped;
} address_step_t;

static int32_t delta_at(const xp_t *xp, const xp_voice_t *v, uint32_t address)
{
	const int8_t delta = (int8_t)rom_byte(xp, v, address);
	const uint8_t shifts = rom_byte(xp, v, address >> 5);
	const int shift = bit(address, 4) ? (shifts >> 4) : (shifts & 0x0f);
	return (int32_t)((uint32_t)(int32_t)delta << (shift + 10));
}

static void start_reader(xp_t *xp, int n)
{
	xp_voice_t *v = &xp->voices[n];
	const uint32_t control = page(xp, n, XP_PAGE_CONTROL);
	v->start_pending = 0;
	v->region = control & 0x7f;
	v->alternate = bit(control, 12);
	v->reverse = bit(control, 11);
	v->start = page(xp, n, XP_PAGE_ADDRESS) & 0xfffff;
	v->loop = page(xp, n, XP_PAGE_LOOP) & 0xfffff;
	v->end = page(xp, n, XP_PAGE_END) & 0xfffff;
	v->backward = v->reverse;
	v->address = v->reverse ? v->end : v->start;
	v->sub_phase = 0;
	v->reading = 1;
	v->loop_reported = 0;
	v->done_reported = 0;
	v->filter_low = 0;
	v->filter_band = 0;
	v->predictor = 0;
	for (uint32_t a = v->address & ~0x1fu; a < v->address; a++)
		v->predictor = wrap_add(v->predictor, delta_at(xp, v, a));
}

static address_step_t advance(const xp_voice_t *v, address_step_t s)
{
	const bool looping = v->loop < v->end;
	if (!s.backward)
	{
		if (!looping)
		{
			if (s.address + 1 >= v->end)
				return (address_step_t){ s.address, false, true };
			return (address_step_t){ s.address + 1, false, false };
		}
		if (s.address >= v->end)
		{
			if (v->alternate)
				return (address_step_t){ s.address, true, false };
			return (address_step_t){ v->loop, false, false };
		}
		return (address_step_t){ s.address + 1, false, false };
	}
	const uint32_t bound = (v->alternate && looping) ? v->loop : (v->reverse ? v->start : v->loop);
	if (s.address <= bound)
	{
		if (v->alternate && looping)
			return (address_step_t){ s.address, false, false };
		return (address_step_t){ s.address, true, true };
	}
	return (address_step_t){ s.address - 1, true, false };
}

/* ---------------------------------------------------------------- the voice */

static void run_voice(xp_t *xp, int n)
{
	xp_voice_t *v = &xp->voices[n];
	if (!((xp->run_mask >> n) & 1))
		return;
	if (v->start_pending)
		start_reader(xp, n);

	if ((xp->frame_counter & 7) == 0)
	{
		if (ramp_update(&v->pitch, XP_LAW_LINEAR) && v->pitch.current == 0)
		{
			v->reading = 0;
			v->predictor = 0;
			v->done_reported = 1;
		}
		ramp_update(&v->tvf, XP_LAW_LINEAR);
		ramp_update(&v->reso, XP_LAW_EXPONENTIAL);
		ramp_update(&v->tva2, XP_LAW_EXPONENTIAL);
	}
	if ((xp->frame_counter & 1) == 0)
	{
		const bool arrived = ramp_update(&v->tva1, ramp_control_law(&v->tva1));
		if (arrived && v->tva1.current == 0 && !v->done_reported)
		{
			v->done_reported = 1;
			raise_irq(xp, n, XP_IRQ_VOICE_DONE);
		}
	}

	int32_t sample = 0;
	if (v->reading)
	{
		const int phase = v->sub_phase >> 9;
		address_step_t s = { v->address, v->backward != 0, false };
		int64_t weighted = 0;
		for (int i = 0; i < 3 && !s.stopped; i++)
		{
			weighted += (int64_t)interp_weights[i][phase] * delta_at(xp, v, s.address);
			s = advance(v, s);
		}
		sample = wrap_add(v->predictor, (int32_t)(weighted >> 12)) >> 4;

		const uint32_t phase_sum = (uint32_t)v->sub_phase + (uint32_t)exp_decode(xp, ramp_value_at(&v->pitch, xp->frame_counter & 7, 8));
		v->sub_phase = phase_sum & 0xffff;
		for (uint32_t carry = phase_sum >> 16; carry && v->reading; carry--)
		{
			v->predictor = wrap_add(v->predictor, delta_at(xp, v, v->address));
			const address_step_t next = advance(v, (address_step_t){ v->address, v->backward != 0, false });
			if (next.stopped)
			{
				v->reading = 0;
				v->predictor = 0;
				if (!v->done_reported)
				{
					v->done_reported = 1;
					raise_irq(xp, n, XP_IRQ_VOICE_DONE);
				}
				break;
			}
			v->address = next.address;
			v->backward = next.backward;
			if (!v->loop_reported && (v->backward ? v->address <= v->loop : v->address >= v->loop))
			{
				v->loop_reported = 1;
				raise_irq(xp, n, XP_IRQ_LOOP_REACHED);
			}
		}
	}

	const uint32_t filter = page(xp, n, XP_PAGE_FILTER);
	if (!(filter & 0xf000))
	{
		const int32_t f = ramp_coefficient_at(&v->tvf, xp->frame_counter & 7, 8);
		const int32_t q = ramp_coefficient_at(&v->reso, xp->frame_counter & 7, 8);
		v->filter_low += (int32_t)(((int64_t)f * v->filter_band) >> 14);
		const int32_t high = sample - ((int32_t)(((int64_t)q * v->filter_band) >> 14) + v->filter_low);
		v->filter_band += (int32_t)(((int64_t)f * high) >> 14);
		switch ((filter >> 10) & 3)
		{
		case 0: sample = v->filter_low; break;
		case 1: sample = v->filter_band; break;
		case 2: sample = high; break;
		case 3: sample = v->filter_low - high; break;
		}
	}
	sample = (int32_t)(((int64_t)sample * ramp_coefficient_at(&v->tva1, xp->frame_counter & 1, 2)) >> 14);
	sample = (int32_t)(((int64_t)sample * ramp_coefficient_at(&v->tva2, xp->frame_counter & 7, 8)) >> 14);

	for (int bank = 0; bank < 4; bank++)
	{
		const uint16_t s = send(xp, n, bank);
		xp->bus[s & 63] += (int32_t)(((int64_t)sample * (s >> 6)) >> 10);
	}
}

/* ---------------------------------------------------------------- the DSP */

static void update_iram_ramps(xp_t *xp)
{
	for (int word = 0xc0; word < XP_IRAM_SIZE; word++)
	{
		if (!xp->iram_ramping[word])
			continue;
		const int32_t target = (int32_t)xp->regs[(XP_IRAM3_TARGET_BASE >> 1) + (word - 0xc0)] << 13;
		const int32_t rate = xp->regs[(XP_IRAM3_RATE >> 1) + ((word - 0xc0) >> 4)];
		int32_t *current = &xp->iram[word];
		if (*current < target)
			*current = min32(*current + rate, target);
		else
			*current = max32(*current - rate, target);
		if (*current == target)
			xp->iram_ramping[word] = 0;
	}
}

void xp_decode_program(xp_t *xp)
{
	static const uint8_t shifts[4] = { 0, 1, 2, 4 };
	const uint16_t *pram = &xp->regs[XP_PRAM_BASE >> 1];
	const uint16_t *cram = &xp->regs[XP_CRAM_BASE >> 1];
	for (int i = 0; i < XP_DSP_SLOTS; i++)
	{
		const uint32_t w = ((uint32_t)pram[i * 2] << 16) | pram[i * 2 + 1];
		const uint16_t c = cram[i];
		const int32_t mantissa = (int32_t)(int16_t)(c << 2) >> 2;
		xp_slot_t *s = &xp->slots[i];
		s->st = (w >> 14) & 3;
		s->word = (w >> 6) & 0xff;
		s->col = w & 0x3f;
		s->ext = (w >> 25) & 7;
		s->eram_op = 0;
		s->eram_offset = 0;
		s->read_bypass = s->col == 0x30 && (c & 0x3e3f) == 0x0221;
		s->cram = c;
		s->coefficient = mantissa << shifts[c >> 14];
		s->raw = bit(c, 15) ? (int32_t)((c & 0x3fff) << 13) : mantissa;
	}
	for (int i = 0; i < XP_DSP_SLOTS - 1; )
	{
		const uint32_t w = ((uint32_t)pram[i * 2] << 16) | pram[i * 2 + 1];
		const uint32_t next = ((uint32_t)pram[i * 2 + 2] << 16) | pram[i * 2 + 3];
		const int op = (w >> 23) & 3;
		if (op)
		{
			xp->slots[i].eram_op = (uint8_t)op;
			xp->slots[i].eram_offset = (uint16_t)((((w >> 16) & 0x7f) << 9) | ((next >> 16) & 0x1ff));
			i += 2;
		}
		else
			i++;
	}
}

static int32_t output_word(const xp_t *xp, int word)
{
	return clamp24((int64_t)xp->iram[word] * 2);
}

static void exchange_serial(xp_t *xp)
{
	if (xp->link.serial_out)
		for (int channel = 0; channel < 2; channel++)
			xp->link.serial_out(xp->link.user, channel, output_word(xp, xp->serial_out_word[channel]));
	if (xp->link.serial_in)
		for (int channel = 0; channel < 2; channel++)
			xp->dsp.serial_frame[channel] = wrap24(xp->link.serial_in(xp->link.user, channel));
}

static void dsp_frame_start(xp_t *xp)
{
	update_iram_ramps(xp);
	for (int n = 0; n < XP_BUS_COUNT; n++)
		xp->iram[0x40 + n] = clamp24(xp->bus[n] >> XP_DSP_INPUT_SHIFT);
	exchange_serial(xp);
}

void xp_schedule(xp_t *xp)
{
	const xp_slot_t *s = xp->slots;
	xp_sched_t *o = xp->sched;
	uint8_t reads[XP_DSP_SLOTS], loads[XP_DSP_SLOTS];
	int strobes = 0;

	for (int i = 0; i < XP_DSP_SLOTS; i++)
	{
		reads[i] = s[i].eram_op == 1 || s[i].col == 0x20;
		loads[i] = s[i].st == 1 && s[i].word < 0xf0;
	}
	for (int i = 0; i < XP_DSP_SLOTS; i++)
	{
		o[i].lands = reads[(i - 2) & 0xff];
		o[i].latch_fresh = 0;
		for (int k = 0; k < 4; k++)
			o[i].latch_fresh |= reads[(i - 2 - k) & 0xff];
		o[i].now_valid = loads[i];
		o[i].gain_load = s[i].st == 1 && s[i].word >= 0xf0;
		o[i].r_use = loads[i];
		for (int k = 1; k <= 3 && !o[i].r_use; k++)
			o[i].r_use = loads[(i - k) & 0xff];
		o[i].strobe = 0xff;
		if (s[i].ext == 1)
			o[i].strobe = (uint8_t)(strobes++ & 1);
	}
}

#if (defined SLJIT_64BIT_ARCHITECTURE && SLJIT_64BIT_ARCHITECTURE)

#define XP_REG_ACC SLJIT_S1
#define XP_REG_PPREV SLJIT_S2
#define XP_REG_ABEF SLJIT_S3
#define XP_REG_ABP SLJIT_S4
#define XP_REG_ERAM SLJIT_S5

#define CELL(field) SLJIT_MEM1(SLJIT_S0), (sljit_sw)offsetof(xp_t, field)
#define IRAM_WORD(word) SLJIT_MEM1(SLJIT_S0), (sljit_sw)(offsetof(xp_t, iram) + (size_t)(word) * 4)

static void e_sext(jit_builder_t *b, sljit_s32 reg)
{
	sljit_emit_op1(b->c, SLJIT_MOV_S32, reg, 0, reg, 0);
}

static void e_mov(jit_builder_t *b, sljit_s32 dst, sljit_s32 src, sljit_sw srcw)
{
	sljit_emit_op1(b->c, SLJIT_MOV, dst, 0, src, srcw);
}

static void e_load(jit_builder_t *b, sljit_s32 dst, sljit_s32 mem, sljit_sw memw)
{
	sljit_emit_op1(b->c, SLJIT_MOV_S32, dst, 0, mem, memw);
}

static void e_store(jit_builder_t *b, sljit_s32 mem, sljit_sw memw, sljit_s32 src)
{
	sljit_emit_op1(b->c, SLJIT_MOV32, mem, memw, src, 0);
}

static void e_add(jit_builder_t *b, sljit_s32 dst, sljit_s32 a, sljit_s32 bb, sljit_sw bw)
{
	sljit_emit_op2(b->c, SLJIT_ADD, dst, 0, a, 0, bb, bw);
	e_sext(b, dst);
}

/* dst = s32((a * c + 0x1000) >> 13) */
static void e_mul13(jit_builder_t *b, sljit_s32 dst, sljit_s32 a, sljit_s32 c, sljit_sw cw)
{
	sljit_emit_op2(b->c, SLJIT_MUL, dst, 0, a, 0, c, cw);
	sljit_emit_op2(b->c, SLJIT_ADD, dst, 0, dst, 0, SLJIT_IMM, 0x1000);
	sljit_emit_op2(b->c, SLJIT_ASHR, dst, 0, dst, 0, SLJIT_IMM, 13);
	e_sext(b, dst);
}

static void e_wrap24(jit_builder_t *b, sljit_s32 reg)
{
	sljit_emit_op2(b->c, SLJIT_SHL, reg, 0, reg, 0, SLJIT_IMM, 8);
	e_sext(b, reg);
	sljit_emit_op2(b->c, SLJIT_ASHR, reg, 0, reg, 0, SLJIT_IMM, 8);
}

/* R3 = ((offset + cursor) & 0xffff) * 4, an ERAM byte offset */
static void e_eram_index(jit_builder_t *b, uint16_t offset)
{
	sljit_emit_op1(b->c, SLJIT_MOV_U16, SLJIT_R3, 0, CELL(dsp.cursor));
	sljit_emit_op2(b->c, SLJIT_ADD, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, offset);
	sljit_emit_op2(b->c, SLJIT_AND, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 0xffff);
	sljit_emit_op2(b->c, SLJIT_SHL, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 2);
}

/* R0 = the slot's operand; R1 holds the word read this slot when now_valid */
static void e_operand(jit_builder_t *b, const xp_slot_t *s, const xp_sched_t *o)
{
	switch (s->col >> 4)
	{
	case 1:
		e_mov(b, SLJIT_R0, XP_REG_ACC, 0);
		return;
	case 3:
		if (o->latch_fresh)
		{
			e_load(b, SLJIT_R0, CELL(dsp.latch));
			return;
		}
		if (s->st == 1)
		{
			if (o->now_valid)
				e_mov(b, SLJIT_R0, SLJIT_R1, 0);
			else
				e_mov(b, SLJIT_R0, SLJIT_IMM, 0);
			return;
		}
		if (s->st == 2)
		{
			e_load(b, SLJIT_R0, CELL(dsp.latch));
			return;
		}
		break;
	default:
		break;
	}
	if (o->r_use)
		e_load(b, SLJIT_R0, CELL(dsp.r));
	else
		e_mov(b, SLJIT_R0, SLJIT_IMM, 0);
}

/* R2 = "now_valid ? now : 0" */
static void e_now(jit_builder_t *b, const xp_sched_t *o, sljit_s32 dst)
{
	if (o->now_valid)
		e_mov(b, dst, SLJIT_R1, 0);
	else
		e_mov(b, dst, SLJIT_IMM, 0);
}

/* the ALU: acc in XP_REG_ACC, the previous product in XP_REG_PPREV, this slot's product left in R2 */
static void e_execute(jit_builder_t *b, const xp_slot_t *s, const xp_sched_t *o)
{
	const int nibble = s->col & 0xf;
	e_operand(b, s, o);

	if (s->col == 0x30 && s->cram <= 0x000f)
	{
		e_now(b, o, SLJIT_R2);
		switch (s->cram & 0xf)
		{
		case 0:
		case 2:
			e_load(b, SLJIT_R3, CELL(dsp.mem));
			e_add(b, XP_REG_ACC, XP_REG_ACC, SLJIT_R3, 0);
			break;
		case 4:
			e_load(b, XP_REG_ACC, CELL(dsp.mem));
			break;
		case 5:
			e_mov(b, XP_REG_ACC, XP_REG_PPREV, 0);
			break;
		case 9:
			e_load(b, SLJIT_R3, CELL(dsp.mem));
			e_add(b, XP_REG_ACC, SLJIT_R3, XP_REG_PPREV, 0);
			break;
		default:
			e_add(b, XP_REG_ACC, XP_REG_ACC, XP_REG_PPREV, 0);
			break;
		}
		return;
	}

	if (s->col == 0x30 && (s->cram & 0x3e80) == 0x0280)
	{
		const int code = s->cram & 0xf;
		const int select = (s->cram >> 4) & 3;
		e_load(b, SLJIT_R4, CELL(dsp.gain));

		if (code == 5)
		{
			if (select == 1)
				e_mov(b, SLJIT_R3, (s->st == 3) ? XP_REG_ACC : XP_REG_ABP, 0);
			else if (select == 2)
				e_load(b, SLJIT_R3, CELL(dsp.mem));
			else if (o->latch_fresh || o->now_valid)
				e_mov(b, SLJIT_R3, SLJIT_R0, 0);
			else
				e_load(b, SLJIT_R3, CELL(dsp.mem));
			e_mul13(b, SLJIT_R2, SLJIT_R3, SLJIT_R4, 0);
			e_mov(b, XP_REG_ACC, XP_REG_PPREV, 0);
			return;
		}

		if (code == 3 && select == 2)
		{
			e_load(b, SLJIT_R3, CELL(dsp.mem));
			e_mul13(b, SLJIT_R2, SLJIT_R3, SLJIT_R4, 0);
		}
		else if (code == 1 && select == 2)
		{
			if (o->now_valid)
				e_mov(b, SLJIT_R3, SLJIT_R1, 0);
			else if (o->r_use)
				e_load(b, SLJIT_R3, CELL(dsp.r));
			else
				e_mov(b, SLJIT_R3, SLJIT_IMM, 0);
			e_mul13(b, SLJIT_R2, SLJIT_R3, SLJIT_R4, 0);
		}
		else if ((code == 1 || code == 3) && select == 3)
			e_mul13(b, SLJIT_R2, SLJIT_R0, SLJIT_R4, 0);
		else if (code == 1 || code == 2 || code == 3)
			e_mul13(b, SLJIT_R2, XP_REG_ACC, SLJIT_R4, 0);
		else
			e_mov(b, SLJIT_R2, SLJIT_R0, 0);

		if (code == 2 || code == 3)
			e_add(b, XP_REG_ACC, XP_REG_ACC, XP_REG_PPREV, 0);
		else
		{
			e_mul13(b, SLJIT_R3, XP_REG_ACC, SLJIT_R4, 0);
			e_add(b, XP_REG_ACC, SLJIT_R3, XP_REG_PPREV, 0);
		}
		return;
	}

	if (s->col == 0x30 && (s->cram == 0x1001 || s->cram == 0x2801 || s->cram == 0x0231 || s->cram == 0x0325))
	{
		e_mov(b, SLJIT_R2, SLJIT_IMM, 0);
		switch (s->cram)
		{
		case 0x1001:
			e_wrap24(b, XP_REG_ACC);
			break;
		case 0x2801:
		{
			e_wrap24(b, XP_REG_ACC);
			sljit_emit_op2(b->c, SLJIT_SUB, SLJIT_R3, 0, SLJIT_IMM, 0, XP_REG_ACC, 0);
			struct sljit_jump *positive = sljit_emit_cmp(b->c, SLJIT_SIG_GREATER_EQUAL, XP_REG_ACC, 0, SLJIT_IMM, 0);
			e_mov(b, XP_REG_ACC, SLJIT_R3, 0);
			sljit_set_label(positive, sljit_emit_label(b->c));
			break;
		}
		case 0x0231:
			e_now(b, o, SLJIT_R3);
			e_load(b, SLJIT_R4, CELL(dsp.latch));
			sljit_emit_op2(b->c, SLJIT_SUB, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_R3, 0);
			e_sext(b, SLJIT_R4);
			sljit_emit_op1(b->c, SLJIT_MOV_U16, SLJIT_R0, 0, CELL(dsp.fraction));
			sljit_emit_op2(b->c, SLJIT_MUL, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_R0, 0);
			sljit_emit_op2(b->c, SLJIT_ASHR, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_IMM, 12);
			e_sext(b, SLJIT_R4);
			e_add(b, XP_REG_ACC, SLJIT_R3, SLJIT_R4, 0);
			break;
		default:
			break;
		}
		return;
	}

	if (s->col == 0x0c)
	{
		e_load(b, SLJIT_R3, CELL(dsp.serial_in));
		e_mul13(b, SLJIT_R2, SLJIT_R3, SLJIT_IMM, s->coefficient);
		return;
	}

	if (nibble == 0 && s->col != 0x30)
		e_mov(b, SLJIT_R2, SLJIT_R0, 0);
	else
		e_mul13(b, SLJIT_R2, SLJIT_R0, SLJIT_IMM, s->coefficient);

	switch (s->col)
	{
	case 0x11:
		if (o->now_valid)
			e_mov(b, XP_REG_ACC, SLJIT_R1, 0);
		return;
	case 0x14:
		e_load(b, XP_REG_ACC, CELL(dsp.mem));
		return;
	case 0x0f:
		e_add(b, XP_REG_ACC, XP_REG_ACC, SLJIT_IMM, s->raw);
		return;
	case 0x1f:
		e_load(b, SLJIT_R3, CELL(dsp.mem));
		e_add(b, XP_REG_ACC, SLJIT_R3, SLJIT_IMM, s->raw);
		return;
	case 0x2f:
		e_add(b, XP_REG_ACC, XP_REG_PPREV, SLJIT_IMM, s->raw);
		return;
	case 0x20:
	case 0x21:
		return;
	case 0x32:
		e_add(b, XP_REG_ACC, XP_REG_ACC, XP_REG_PPREV, 0);
		return;
	default:
		break;
	}

	switch (nibble)
	{
	case 0:
	case 3:
		e_add(b, XP_REG_ACC, XP_REG_ACC, XP_REG_PPREV, 0);
		break;
	case 5:
		e_mov(b, XP_REG_ACC, XP_REG_PPREV, 0);
		break;
	case 9:
		e_load(b, SLJIT_R3, CELL(dsp.mem));
		e_add(b, XP_REG_ACC, SLJIT_R3, XP_REG_PPREV, 0);
		break;
	default:
		break;
	}
}

static void e_slot(jit_builder_t *b, int i, const xp_slot_t *s, const xp_sched_t *o)
{
	e_mov(b, XP_REG_ABP, XP_REG_ABEF, 0);
	e_mov(b, XP_REG_ABEF, XP_REG_ACC, 0);

	if (o->lands)
	{
		e_load(b, SLJIT_R3, CELL(dsp.pend[i & 1]));
		e_store(b, CELL(dsp.latch), SLJIT_R3);
	}
	if (s->eram_op == 1)
	{
		e_eram_index(b, s->eram_offset);
		e_load(b, SLJIT_R3, SLJIT_MEM2(XP_REG_ERAM, SLJIT_R3), 0);
		e_store(b, CELL(dsp.pend[i & 1]), SLJIT_R3);
	}
	if (s->col == 0x20)
	{
		sljit_emit_op1(b->c, SLJIT_MOV_U16, SLJIT_R3, 0, CELL(dsp.cursor));
		sljit_emit_op2(b->c, SLJIT_ASHR, SLJIT_R4, 0, XP_REG_ACC, 0, SLJIT_IMM, 12);
		sljit_emit_op2(b->c, SLJIT_ADD, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_R4, 0);
		sljit_emit_op2(b->c, SLJIT_AND, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 0xffff);
		sljit_emit_op2(b->c, SLJIT_SHL, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 2);
		e_load(b, SLJIT_R3, SLJIT_MEM2(XP_REG_ERAM, SLJIT_R3), 0);
		e_store(b, CELL(dsp.pend[i & 1]), SLJIT_R3);
		sljit_emit_op2(b->c, SLJIT_AND, SLJIT_R4, 0, XP_REG_ACC, 0, SLJIT_IMM, 0xfff);
		sljit_emit_op1(b->c, SLJIT_MOV_U16, CELL(dsp.fraction), SLJIT_R4, 0);
	}
	if (o->strobe != 0xff)
	{
		e_load(b, SLJIT_R3, CELL(dsp.serial_frame[o->strobe]));
		e_store(b, CELL(dsp.serial_in), SLJIT_R3);
	}
	if (s->st == 1)
		e_load(b, SLJIT_R1, IRAM_WORD(s->word));

	e_execute(b, s, o);

	if (o->gain_load)
	{
		sljit_emit_op2(b->c, SLJIT_ASHR, SLJIT_R3, 0, SLJIT_R1, 0, SLJIT_IMM, 8);
		e_store(b, CELL(dsp.gain), SLJIT_R3);
	}
	if (s->st == 2)
	{
		e_load(b, SLJIT_R3, CELL(dsp.latch));
		e_store(b, IRAM_WORD(s->word), SLJIT_R3);
	}
	else if (s->st == 3)
	{
		e_mov(b, SLJIT_R3, XP_REG_ABEF, 0);
		jit_clamp24(b, SLJIT_R3, SLJIT_R4);
		e_store(b, IRAM_WORD(s->word), SLJIT_R3);
	}
	if (s->eram_op == 3)
	{
		e_mov(b, SLJIT_R4, XP_REG_ABEF, 0);
		jit_clamp24(b, SLJIT_R4, SLJIT_R0);
		e_eram_index(b, s->eram_offset);
		e_store(b, SLJIT_MEM2(XP_REG_ERAM, SLJIT_R3), 0, SLJIT_R4);
	}
	else if (s->eram_op == 2)
	{
		e_load(b, SLJIT_R4, CELL(dsp.r));
		e_eram_index(b, s->eram_offset);
		e_store(b, SLJIT_MEM2(XP_REG_ERAM, SLJIT_R3), 0, SLJIT_R4);
	}
	if (o->now_valid)
	{
		e_store(b, CELL(dsp.r), SLJIT_R1);
		if (!s->read_bypass)
			e_store(b, CELL(dsp.mem), SLJIT_R1);
	}
	e_mov(b, XP_REG_PPREV, SLJIT_R2, 0);
}

static bool compile_program(xp_t *xp)
{
	jit_builder_t b;
	jit_code_t *code = &xp->code[xp->live ^ 1];
	jit_code_free(xp->jit, code);
	xp_schedule(xp);
	if (!jit_begin(&b, xp->jit, 5, 6))
		return false;

	e_load(&b, XP_REG_ACC, CELL(dsp.acc));
	e_mov(&b, XP_REG_PPREV, SLJIT_IMM, 0);
	e_mov(&b, XP_REG_ABEF, XP_REG_ACC, 0);
	sljit_emit_op1(b.c, SLJIT_MOV_P, XP_REG_ERAM, 0, CELL(eram));
	for (int i = 0; i < XP_DSP_SLOTS; i++)
		e_slot(&b, i, &xp->slots[i], &xp->sched[i]);
	e_store(&b, CELL(dsp.acc), XP_REG_ACC);

	if (!jit_end(&b, xp->jit, code))
		return false;
	xp->live ^= 1;
	xp->frame = (xp_frame_fn)code->entry;
	return true;
}

#else

static bool compile_program(xp_t *xp)
{
	xp_schedule(xp);
	xp->frame = NULL;
	return false;
}

#endif

void xp_run_dsp(xp_t *xp)
{
	xp->dsp_enabled = bit(xp->regs[XP_DSP_MODE >> 1], 1);
	if (!xp->dsp_enabled)
		return;
	if (xp->program_dirty)
	{
		xp_decode_program(xp);
		xp->program_dirty = false;
		compile_program(xp);
	}
	dsp_frame_start(xp);
	if (xp->frame)
		xp->frame(xp);
	xp->dsp.cursor--;
}

void xp_run_frame(xp_t *xp)
{
	memset(xp->bus, 0, sizeof(xp->bus));
	for (int n = 0; n < XP_VOICES; n++)
		run_voice(xp, n);
	xp->frame_counter++;
	xp->noise ^= xp->noise << 13;
	xp->noise ^= xp->noise >> 17;
	xp->noise ^= xp->noise << 5;

	xp_run_dsp(xp);
}

int32_t xp_output(const xp_t *xp, int word)
{
	return output_word(xp, word & (XP_OUTPUT_WORDS - 1));
}
