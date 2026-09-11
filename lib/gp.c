#include <string.h>
#include "gp.h"

static const int16_t interp_weights[3][128] = {
	{
		3385, 3401, 3417, 3432, 3448, 3463, 3478, 3492, 3506, 3521, 3535, 3548, 3562, 3575, 3588, 3601,
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
		2048, 2071, 2095, 2119, 2143, 2166, 2190, 2214, 2237, 2261, 2284, 2308, 2331, 2355, 2378, 2401,
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

enum { GP_REGS = 0x38, MASK20 = 0xfffff };

enum
{
	W_ADDRESS = 0x04, W_LOOP = 0x08, W_END = 0x0c,
	N_PITCH = 0x10, N_PAN = 0x12, N_SEND = 0x14, N_TVA1 = 0x16, N_TVA2 = 0x18, N_TVF = 0x1a,
	N_FLAGS = 0x1c, N_CONTROL = 0x1e,
	W_DPCM_REF = 0x24, W_FILTER_LP = 0x28, W_FILTER_BP = 0x2c,
	N_PHASE = 0x30, N_TVA1_LEVEL = 0x32, N_TVA2_LEVEL = 0x34, N_TVF_LEVEL = 0x36
};

enum { LINE_A = 28, LINE_B = 29, REVERB = 30, CHORUS = 31 };

enum
{
	A_COMB2_STATE = 0x04, A_REVERB_OUT_B = 0x08, A_COMB4_OUT = 0x0c,
	A_REVERB_OUT_A = 0x24, A_TAP_B3 = 0x28, A_CHORUS_B = 0x2c
};

enum
{
	B_COMB4_STATE = 0x04, B_DAMP_A = 0x08, B_REVERB_IN = 0x0c,
	B_CHORUS_A = 0x24, B_DAMP_B = 0x28, B_CHORUS_IN = 0x2c,
	B_CHORUS_TAP_A = 0x34, B_CHORUS_TAP_B = 0x36
};

enum
{
	R_SAMPLE_RIGHT_1 = 0x04, R_SAMPLE_LEFT_1 = 0x08, R_RESIDUAL_LEFT = 0x0c,
	R_LEVEL_SETTING = 0x10, R_IN_COEF = 0x12, R_OUT_COEF_A = 0x14, R_OUT_COEF_B = 0x16,
	R_ALLPASS_COEF_A = 0x18, R_ALLPASS_COEF_B = 0x1a, R_COMB_COEF = 0x1c, R_DAMP_COEF_A = 0x1e,
	R_SAMPLE_RIGHT_2 = 0x24, R_SAMPLE_LEFT_2 = 0x28, R_RESIDUAL_RIGHT = 0x2c,
	R_DAMP_COEF_B = 0x30, R_LEVEL = 0x32, R_NOISE = 0x34
};

enum
{
	C_IN_COEF = 0x12, C_OUT_COEF_A = 0x14, C_OUT_COEF_B = 0x16, C_OUT_COEF_C = 0x18,
	C_OUT_COEF_D = 0x1a, C_MIX_RIGHT = 0x28, C_MIX_LEFT = 0x2c, C_FADE_A = 0x32, C_FADE_B = 0x34
};

enum { ENV_TVA1, ENV_TVA2, ENV_TVF };

/* one position of a voice's address generator */
typedef struct gp_step
{
	uint32_t address;
	bool backward;
} gp_step_t;

static inline bool bit(uint32_t value, int n) { return (value >> n) & 1; }

static inline uint32_t with_byte(uint32_t word, int n, uint8_t data)
{
	return (word & ~(0xffu << (n * 8))) | ((uint32_t)data << (n * 8));
}

static inline bool is_register(uint8_t address)
{
	return address >= 0x04 && address < GP_REGS && (address < 0x20 || address >= 0x24);
}

static inline bool is_wide(uint8_t address) { return (address & 0x1f) < 0x10; }
static inline uint8_t group(uint8_t address) { return is_wide(address) ? (address & 0xfc) : (address & 0xfe); }
static inline int wide_index(int address) { return (address >> 2) - (bit(address, 5) ? 6 : 1); }
static inline int narrow_index(int address) { return ((address >> 1) & 7) + (bit(address, 5) ? 8 : 0); }
static inline int nth_narrow(int index) { return index < 8 ? 0x10 + index * 2 : 0x30 + (index - 8) * 2; }

static inline int32_t wide(const gp_t *gp, int n, int reg)
{
	return (int32_t)(gp->slots[n].wide[wide_index(reg)] << 12) >> 12;
}

static inline uint32_t wide_u(const gp_t *gp, int n, int reg) { return gp->slots[n].wide[wide_index(reg)]; }
static inline void set_wide(gp_t *gp, int n, int reg, int32_t value) { gp->slots[n].wide[wide_index(reg)] = (uint32_t)value & MASK20; }
static inline uint16_t narrow(const gp_t *gp, int n, int reg) { return gp->slots[n].narrow[narrow_index(reg)]; }
static inline void set_narrow(gp_t *gp, int n, int reg, uint16_t value) { gp->slots[n].narrow[narrow_index(reg)] = value; }

static inline int slot_count(const gp_t *gp) { return (gp->slot_config & 31) + 1; }
static inline bool double_rate(const gp_t *gp) { return bit(gp->output_config, 6); }


/* ---------------------------------------------------------------- arithmetic */

static inline int32_t sat20(int64_t value)
{
	return (int32_t)(value > 0x7ffff ? 0x7ffff : value < -0x80000 ? -0x80000 : value);
}

static int32_t mul20x8(int32_t value, int8_t coef)
{
	const int32_t product = (int32_t)((uint32_t)value * (uint32_t)(int32_t)coef);
	return bit(product, 27) ? (product | ~0x1ffffff) : (product & 0x1ffffff);
}

static int32_t mul24x8(int32_t value, int8_t coef)
{
	const int32_t product = (int32_t)((uint32_t)value * (uint32_t)(int32_t)coef);
	return bit(product, 31) ? (product | ~0x1fffffff) : (product & 0x1fffffff);
}

static inline int32_t scaled(int32_t value, int8_t coef) { return mul20x8(value, coef) >> 5; }
static inline int32_t half(int32_t value) { return (value >> 1) + (value & 1); }
static inline int32_t blend(int32_t a, int32_t b) { return sat20((int64_t)(a >> 1) + (b >> 1) + ((a | b) & 1)); }
static inline int8_t coef_hi(uint16_t value) { return (int8_t)(value >> 8); }
static inline int8_t coef_lo(uint16_t value) { return (int8_t)value; }

static int32_t amplify(int32_t sample, uint16_t level)
{
	const int32_t coarse = mul20x8(sample, (int8_t)((level >> 8) & 0x7f));
	const int32_t fine = mul20x8(sample, (int8_t)((level >> 1) & 0x7f));
	return sat20((int64_t)(coarse >> 6) + (fine >> 13) + (((fine >> 12) | (coarse >> 5)) & 1));
}

static int32_t eram_shifted(const gp_t *gp, uint32_t base, int extra_shift)
{
	const uint16_t word = gp->eram[(base + gp->frame_counter) & (GP_ERAM_SIZE - 1)];
	const int32_t mantissa = (int32_t)((uint32_t)word << 18) >> 18;
	return (int32_t)((uint32_t)mantissa << (2 * (word >> 14))) >> extra_shift;
}

static inline int32_t eram_read(const gp_t *gp, uint32_t base) { return eram_shifted(gp, base, 0); }

static void eram_write(gp_t *gp, uint32_t base, int32_t value)
{
	const int32_t clamped = sat20(value);
	int exponent = 0;
	while (exponent < 3 && ((clamped >> (2 * exponent)) < -0x2000 || (clamped >> (2 * exponent)) > 0x1fff))
		exponent++;
	gp->eram[(base + gp->frame_counter) & (GP_ERAM_SIZE - 1)] =
			(uint16_t)((exponent << 14) | ((clamped >> (2 * exponent)) & 0x3fff));
}


/* ---------------------------------------------------------------- wave ROM and the address generator */

static uint8_t read_byte(const gp_t *gp, uint32_t address)
{
	address &= 0xffffff;
	return address < gp->wave_size ? gp->wave[address] : 0;
}

static int8_t byte_at(const gp_t *gp, int slot, uint32_t address)
{
	const uint32_t bank = (uint32_t)(narrow(gp, slot, N_CONTROL) >> 8) & 0x0f;
	return (int8_t)read_byte(gp, (bank << 20) | (address & MASK20));
}

static gp_step_t advance(const gp_t *gp, int slot, gp_step_t step, bool update_direction)
{
	const uint16_t control = narrow(gp, slot, N_CONTROL);
	const bool alternate = bit(control, 6);
	const bool reverse = bit(control, 7);
	const bool at_endpoint = step.address == wide_u(gp, slot, step.backward ? W_LOOP : W_END);

	uint32_t address = step.address;
	int delta = 0;
	if (at_endpoint)
	{
		if (!alternate)
			address = wide_u(gp, slot, W_LOOP);
	}
	else
	{
		delta = (alternate && step.backward) ? -1 : 1;
	}

	const bool backward = alternate && (step.backward != at_endpoint);
	const gp_step_t next = { (uint32_t)(address + (uint32_t)(reverse ? -delta : delta)) & MASK20,
			update_direction ? backward : step.backward };
	return next;
}

static uint8_t fetch_block_shift(const gp_t *gp, int slot, gp_step_t step, bool active, bool keyed)
{
	const uint16_t control = narrow(gp, slot, N_CONTROL);
	const bool alternate = bit(control, 6);
	const bool reverse = bit(control, 7);
	const uint32_t endpoint = wide_u(gp, slot, step.backward ? W_LOOP : W_END);
	const bool at_block = ((endpoint ^ step.address) & 0xffff0) == 0;
	const uint32_t block_address = (!alternate && at_block) ? wide_u(gp, slot, W_LOOP) : step.address;
	const bool odd_block = bit(block_address, 4);

	const bool leaving = (odd_block != reverse) && active;
	const bool towards_end = step.backward == at_block;
	const bool step_forward = alternate ? (leaving && towards_end) : (!at_block && leaving);
	const bool step_back = alternate && !towards_end && active && (odd_block == reverse);
	const int move = (int)step_forward - (int)step_back;

	const uint32_t bank = (uint32_t)(control >> 8) & 0x0f;
	const uint32_t table = (uint32_t)((block_address >> 5) + (uint32_t)(reverse ? -move : move)) & 0x7fff;
	const uint8_t pair = read_byte(gp, (bank << 20) | table);
	return (odd_block != ((alternate || !at_block) && keyed)) ? (pair >> 4) : (pair & 0x0f);
}


/* ---------------------------------------------------------------- envelopes and filter */

static uint16_t envelope_step(gp_t *gp, int slot, int setting_position, int level_position, int kind, bool active)
{
	static const uint16_t period[4] = { 3, 15, 63, 127 };

	const uint16_t setting = narrow(gp, slot, setting_position);
	const uint16_t level = narrow(gp, slot, level_position);
	const uint8_t rate = (uint8_t)setting;
	const uint32_t target = setting >> 8;

	const bool fine_rate = (rate & 0xf0) == 0;
	const bool half_rate = fine_rate || bit(rate, 4);
	const bool linear = !gp->first_frame && (!bit(rate, 7) || (!bit(rate, 6) && (!half_rate || !bit(rate, 5))));
	const bool every_frame = !bit(rate, 7) || !bit(rate, 6);

	const int divider = (half_rate ? 1 : 0) | (bit(rate, 5) ? 2 : 0);
	const bool update = !active || every_frame || (gp->frame_counter & period[divider]) == 0;
	const int window = every_frame ? 0 : divider * 2 + 2;

	int32_t dither = 0;
	for (int i = 0; i < 4; i++)
		dither |= (int32_t)bit(gp->frame_counter, window + 3 - i) << i;

	const bool use_level = kind != ENV_TVF || active;
	const int32_t target_scaled = (int32_t)target << 11;
	const int32_t base = use_level ? ((int32_t)level << 4) : 0;
	const bool negative = bit((uint32_t)(target_scaled - base), 19);

	uint16_t stepped, result;
	if (!linear)
	{
		const int32_t sum = base + dither + ((target_scaled - base) >> ((10 - (rate & 15)) & 15));
		stepped = (uint16_t)((sum >> 4) & 0x7fff);
		result = (uint16_t)((sum >> 4) & 0x7ffe);
	}
	else
	{
		int32_t step = (int32_t)(((uint32_t)(rate & 15) << 9) | (fine_rate ? 0u : 0x2000u));
		if (negative)
			step = -(step + 0x40);
		step >>= (10 - (((rate >> 4) & 14) | (half_rate ? 1 : 0))) & 15;

		const int32_t next = (base + (use_level ? dither : 0) + step) >> 4;
		const bool overshoot = bit((uint32_t)target_scaled - ((uint32_t)next << 4), 19) != negative;
		stepped = overshoot ? (uint16_t)(target << 7) : (uint16_t)(next & 0x7fff);
		result = (kind == ENV_TVA2 && overshoot) ? (uint16_t)(target << 7) : (uint16_t)(next & 0x7ffe);
	}

	if (update && !gp->first_frame)
		set_narrow(gp, slot, level_position, stepped);
	return result;
}

static inline int32_t filter_rail(int64_t value, int rail)
{
	const int64_t limit = (int64_t)1 << (rail - 1);
	return (int32_t)(value > limit - 1 ? limit - 1 : value < -limit ? -limit : value);
}

static inline int32_t filter_product(const gp_t *gp, int32_t value, int8_t coef)
{
	return (gp->gp4 ? mul24x8(value, coef) : mul20x8(value, coef)) >> 5;
}

static int32_t run_filter(gp_t *gp, int slot, int32_t input)
{
	const uint16_t flags = narrow(gp, slot, N_FLAGS);
	const uint16_t cutoff = narrow(gp, slot, N_TVF_LEVEL);
	const int8_t coarse = (int8_t)((cutoff >> 8) & 0x7f);
	const int8_t fine = (int8_t)((cutoff >> 1) & 0x7f);
	const int8_t resonance = (int8_t)((flags >> 8) & 0x7f);
	const int rail = gp->gp4 ? 24 : 20;

	int32_t low = gp->slots[slot].filter_low;
	int32_t band = gp->slots[slot].filter_band;

	low = filter_rail((int64_t)low + half(filter_product(gp, band, coarse)), rail);
	low = filter_rail((int64_t)low + half(filter_product(gp, band, fine) >> 7), rail);
	const int32_t inner = filter_rail((int64_t)low + half(filter_product(gp, band, resonance)), rail);
	const int32_t high = filter_rail((int64_t)input - inner, rail);
	band = filter_rail((int64_t)band + half(filter_product(gp, high, coarse)), rail);
	band = filter_rail((int64_t)band + half(filter_product(gp, high, fine) >> 7), rail);

	gp->slots[slot].filter_low = low;
	gp->slots[slot].filter_band = band;
	set_wide(gp, slot, W_FILTER_LP, low);
	set_wide(gp, slot, W_FILTER_BP, band);
	return bit(flags, 1) ? high : low;
}


/* ---------------------------------------------------------------- voices and the mixer */

static void run_voice(gp_t *gp, int n)
{
	const uint16_t control = narrow(gp, n, N_CONTROL);
	const uint16_t flags = narrow(gp, n, N_FLAGS);
	const uint16_t phase = narrow(gp, n, N_PHASE);
	const bool key = bit(gp->key_mask & gp->key_mask_pending, n);
	const bool active = key && bit(control, 5);

	const uint32_t address = wide_u(gp, n, W_ADDRESS);
	const uint32_t sum = (phase & 0x3fff) + narrow(gp, control & 0x1f, N_PITCH);
	const int carry = sum >> 14;

	gp_step_t steps[5];
	steps[0].address = address;
	steps[0].backward = bit(phase, 15);
	for (int i = 0; i < 4; i++)
		steps[i + 1] = advance(gp, n, steps[i], i < 3);

	const uint8_t fetched = fetch_block_shift(gp, n, steps[0], active, bit(control, 5));
	const bool keying_on = key && !bit(control, 5);
	const uint16_t nibble = keying_on ? (uint16_t)fetched
			: (gp->slots[n].crossed ? (uint16_t)gp->slots[n].prefetch : (uint16_t)(control >> 12));
	gp->slots[n].prefetch = fetched;

	const uint32_t block = address >> 4;
	int8_t delta[4];
	int scale[4];
	for (int i = 0; i < 4; i++)
	{
		const int shift = ((steps[i].address >> 4) == block) ? nibble : fetched;
		scale[i] = (10 - shift) & 15;
		delta[i] = byte_at(gp, n, steps[i].address);
	}

	const int weight = (phase >> 7) & 0x7f;
	int32_t sample = wide(gp, n, W_DPCM_REF);
	for (int i = 0; i < 3; i++)
	{
		const int32_t weighted = mul20x8((int32_t)interp_weights[i][weight] << 6, delta[i]) >> 8;
		sample = sat20((int64_t)sample + half((weighted << 1) >> scale[i]));
	}

	int32_t reference = wide(gp, n, W_DPCM_REF);
	for (int i = 0; i < carry; i++)
		reference = sat20((int64_t)reference + half(((int32_t)delta[i] << 11) >> scale[i]));
	set_wide(gp, n, W_DPCM_REF, reference);

	sample = run_filter(gp, n, sample);

	const uint16_t gain1 = envelope_step(gp, n, N_TVA1, N_TVA1_LEVEL, ENV_TVA1, active);
	const uint16_t gain2 = envelope_step(gp, n, N_TVA2, N_TVA2_LEVEL, ENV_TVA2, active);
	envelope_step(gp, n, N_TVF, N_TVF_LEVEL, ENV_TVF, active);

	if (active)
	{
		sample = amplify(sample, gain1);
		sample = amplify(sample, gain2);

		const uint16_t pan = narrow(gp, n, N_PAN);
		const uint16_t send = narrow(gp, n, N_SEND);
		gp->mix[0] = sat20((int64_t)gp->mix[0] + half(scaled(sample, coef_hi(pan))));
		gp->mix[1] = sat20((int64_t)gp->mix[1] + half(scaled(sample, coef_lo(pan))));
		gp->send[0] = sat20((int64_t)gp->send[0] + half(scaled(sample, coef_hi(send))));
		gp->send[1] = sat20((int64_t)gp->send[1] + half(scaled(sample, coef_lo(send))));
	}

	const gp_step_t next = steps[carry];
	uint16_t new_phase = (uint16_t)((sum & 0x3fff) | (phase & 0x4000) | (next.backward ? 0x8000 : 0));

	if (key && bit(flags, 0) && !bit(phase, 14) && !gp->irq_pending)
	{
		const uint32_t loop = wide_u(gp, n, W_LOOP);
		if (bit(control, 7) ? (address <= loop) : (address >= loop))
		{
			new_phase |= 0x4000;
			gp->irq_slot = (uint8_t)n;
			gp->irq_pending = true;
			if (gp->link.irq)
				gp->link.irq(gp->link.user, true);
		}
	}

	if (active)
		set_wide(gp, n, W_ADDRESS, next.address);
	set_narrow(gp, n, N_PHASE, new_phase);

	set_narrow(gp, n, N_CONTROL, (uint16_t)((control & 0x0fdf) | (nibble << 12) | (key ? 0x20 : 0)));
	gp->slots[n].crossed = (next.address >> 4) != block;

	if (!active)
	{
		set_narrow(gp, n, N_PHASE, 0);
		set_narrow(gp, n, N_TVA1_LEVEL, 0);
		set_narrow(gp, n, N_TVA2_LEVEL, 0);
		if (!gp->first_frame)
		{
			gp->slots[n].filter_low = 0;
			gp->slots[n].filter_band = 0;
			set_wide(gp, n, W_FILTER_LP, 0);
			set_wide(gp, n, W_FILTER_BP, 0);
			set_wide(gp, n, W_DPCM_REF, 0);
		}
	}
}

static void inject(gp_t *gp, int index)
{
	static const struct { uint8_t slot; uint8_t position; uint8_t channel; uint8_t send; } route[6] =
	{
		{ REVERB, R_OUT_COEF_A, 0, 1 },
		{ REVERB, R_OUT_COEF_B, 1, 1 },
		{ CHORUS, C_OUT_COEF_A, 0, 0 },
		{ CHORUS, C_OUT_COEF_B, 1, 1 },
		{ CHORUS, C_OUT_COEF_C, 0, 0 },
		{ CHORUS, C_OUT_COEF_D, 1, 1 },
	};

	const uint16_t coef = narrow(gp, route[index].slot, route[index].position);
	const int32_t value = gp->returns[index];
	const int channel = route[index].channel;
	const int send = route[index].send;

	gp->mix[channel] = sat20((int64_t)gp->mix[channel] + half(scaled(value, coef_hi(coef))));
	gp->send[send] = sat20((int64_t)gp->send[send] + half(scaled(value, coef_lo(coef))));
}

static void run_voices(gp_t *gp)
{
	gp->mix[0] = 0;
	gp->mix[1] = 0;
	gp->send[0] = 0;
	gp->send[1] = 0;

	const int count = slot_count(gp);
	for (int n = 0; n < count; n++)
	{
		switch (n)
		{
		case 16: inject(gp, 0); break;
		case 17: inject(gp, 1); break;
		case 20: inject(gp, 2); break;
		case 21: inject(gp, 3); break;
		case 22: inject(gp, 4); break;
		default: break;
		}
		if (n == count - 1)
		{
			inject(gp, 5);
			set_wide(gp, CHORUS, C_MIX_LEFT, gp->mix[0]);
			set_wide(gp, CHORUS, C_MIX_RIGHT, gp->mix[1]);
		}
		run_voice(gp, n);
	}
}


/* ---------------------------------------------------------------- effect processor */

static int32_t allpass(gp_t *gp, uint32_t read_base, uint32_t write_base, int32_t input, uint16_t coef)
{
	const int32_t tap = eram_read(gp, read_base);
	const int32_t inner = (coef & 0x0030) ? sat20((int64_t)input - eram_shifted(gp, read_base, 1)) : input;
	eram_write(gp, write_base, inner);
	return sat20((int64_t)tap + half(scaled(inner, coef_lo(coef))));
}

static void run_effects(gp_t *gp)
{
	uint32_t a[12], b[10];
	for (int i = 0; i < 12; i++)
		a[i] = narrow(gp, LINE_A, nth_narrow(i));
	for (int i = 0; i < 10; i++)
		b[i] = narrow(gp, LINE_B, nth_narrow(i));

	const uint16_t phase = narrow(gp, CHORUS, N_PHASE);
	const int32_t complement = 0x4000 - (int32_t)phase;
	uint16_t fade_a = 0, fade_b = 0;
	if (bit(phase, 15))
		fade_a = phase & 0x7fff;
	else
		fade_b = phase & 0x7fff;
	if (bit((uint32_t)complement, 15))
		fade_b = (uint16_t)complement & 0x7fff;
	else
		fade_a = (uint16_t)complement & 0x7fff;
	set_narrow(gp, CHORUS, C_FADE_A, fade_a);
	set_narrow(gp, CHORUS, C_FADE_B, fade_b);

	const uint16_t chorus_coef = narrow(gp, CHORUS, C_IN_COEF);
	const int32_t chorus_in = blend(scaled(wide(gp, LINE_B, B_CHORUS_IN), coef_hi(chorus_coef)),
			scaled(gp->send[1], coef_lo(chorus_coef)));

	envelope_step(gp, REVERB, R_LEVEL_SETTING, R_LEVEL, ENV_TVA2, bit(narrow(gp, CHORUS, N_CONTROL), 5));
	const int8_t level = coef_hi(narrow(gp, REVERB, R_LEVEL));

	const uint16_t in_coef = narrow(gp, REVERB, R_IN_COEF);
	const int32_t reverb_in = blend(scaled(wide(gp, LINE_B, B_REVERB_IN), coef_hi(in_coef)),
			scaled(gp->send[0], coef_lo(in_coef)));

	const uint16_t allpass_a = narrow(gp, REVERB, R_ALLPASS_COEF_A);
	const uint16_t allpass_b = narrow(gp, REVERB, R_ALLPASS_COEF_B);
	int32_t chain = mul20x8(reverb_in, coef_hi(allpass_a)) >> 6;
	chain = allpass(gp, a[1], a[0], chain, allpass_a);
	chain = allpass(gp, a[2], a[1], chain, allpass_a);
	chain = allpass(gp, a[3], a[2], chain, allpass_a);
	const int32_t comb1_state_in = eram_read(gp, a[5]);
	chain = allpass(gp, a[4], a[3], chain, allpass_b);
	const int32_t comb2_state_in = eram_read(gp, b[1]);

	const uint16_t damp_coef_a = narrow(gp, REVERB, R_DAMP_COEF_A);
	const uint16_t damp_coef_b = narrow(gp, REVERB, R_DAMP_COEF_B);
	const int32_t damp_a = blend(scaled(wide(gp, LINE_B, B_DAMP_A), coef_hi(damp_coef_a)),
			scaled(eram_read(gp, b[0]), coef_lo(damp_coef_a)));
	const int32_t damp_b = blend(scaled(wide(gp, LINE_B, B_DAMP_B), coef_hi(damp_coef_b)),
			scaled(eram_read(gp, b[8]), coef_lo(damp_coef_b)));

	const uint16_t comb = narrow(gp, REVERB, R_COMB_COEF);
	int32_t feed_a = sat20((int64_t)chain + half(scaled(damp_a, level)));
	int32_t feed_b = sat20((int64_t)chain + half(scaled(damp_b, level)));

	feed_a = sat20((int64_t)feed_a + half(scaled(comb1_state_in, coef_hi(comb))));
	const int32_t comb1_state = sat20((int64_t)comb1_state_in + half(scaled(feed_a, coef_lo(comb))));
	const int32_t comb3_state_in = eram_read(gp, a[9]);
	feed_b = sat20((int64_t)feed_b + half(scaled(comb2_state_in, coef_hi(comb))));
	const int32_t comb2_state = sat20((int64_t)comb2_state_in + half(scaled(feed_b, coef_lo(comb))));
	const int32_t comb4_state_in = eram_read(gp, b[5]);

	const int32_t comb3_state = sat20((int64_t)eram_read(gp, a[8]) + half(scaled(comb3_state_in, coef_hi(comb))));
	const int32_t comb3_out = sat20((int64_t)comb3_state_in + half(scaled(comb3_state, coef_lo(comb))));
	const int32_t comb4_state = sat20((int64_t)eram_read(gp, b[4]) + half(scaled(comb4_state_in, coef_hi(comb))));
	const int32_t comb4_out = sat20((int64_t)comb4_state_in + half(scaled(comb4_state, coef_lo(comb))));

	eram_write(gp, a[4], feed_a);
	eram_write(gp, a[5], comb1_state);
	eram_write(gp, b[0], feed_b);

	const int32_t out_a = sat20((int64_t)sat20((int64_t)eram_read(gp, b[6]) + eram_read(gp, b[2]))
			+ sat20((int64_t)eram_read(gp, a[6]) + eram_read(gp, a[10])));
	const int32_t tap_b3 = eram_read(gp, b[3]);
	const int32_t out_b = sat20((int64_t)sat20((int64_t)sat20((int64_t)eram_read(gp, a[7]) + eram_read(gp, a[11]))
			+ eram_read(gp, b[7])) + tap_b3);

	eram_write(gp, b[1], comb2_state);
	eram_write(gp, a[8], comb3_state);
	eram_write(gp, a[9], comb3_out);

	const uint32_t tap_a = narrow(gp, LINE_B, B_CHORUS_TAP_A);
	const uint32_t tap_b = narrow(gp, LINE_B, B_CHORUS_TAP_B);
	const int32_t next_a = eram_read(gp, tap_a + 1);
	const int32_t next_b = eram_read(gp, tap_b + 1);
	const int32_t current_a = eram_read(gp, tap_a);
	const int32_t current_b = eram_read(gp, tap_b);
	const int32_t chorus_a = sat20((int64_t)sat20((int64_t)current_a - (scaled(current_a, coef_hi(fade_a)) >> 1))
			+ half(scaled(next_a, coef_hi(fade_a))));
	const int32_t chorus_b = sat20((int64_t)sat20((int64_t)current_b - (scaled(current_b, coef_hi(fade_b)) >> 1))
			+ half(scaled(next_b, coef_hi(fade_b))));

	eram_write(gp, b[4], comb4_state);
	eram_write(gp, b[5], comb4_out);
	eram_write(gp, b[9], chorus_in);

	set_wide(gp, LINE_B, B_COMB4_STATE, comb4_state);
	set_wide(gp, LINE_B, B_DAMP_A, damp_a);
	set_wide(gp, LINE_B, B_REVERB_IN, reverb_in);
	set_wide(gp, LINE_B, B_CHORUS_A, chorus_a);
	set_wide(gp, LINE_B, B_DAMP_B, damp_b);
	set_wide(gp, LINE_B, B_CHORUS_IN, chorus_in);
	set_wide(gp, LINE_A, A_COMB2_STATE, comb2_state);
	set_wide(gp, LINE_A, A_REVERB_OUT_B, out_b);
	set_wide(gp, LINE_A, A_COMB4_OUT, comb4_out);
	set_wide(gp, LINE_A, A_REVERB_OUT_A, out_a);
	set_wide(gp, LINE_A, A_TAP_B3, tap_b3);
	set_wide(gp, LINE_A, A_CHORUS_B, chorus_b);

	gp->returns[0] = out_a;
	gp->returns[1] = out_b;
	gp->returns[2] = chorus_a;
	gp->returns[3] = chorus_a;
	gp->returns[4] = chorus_b;
	gp->returns[5] = chorus_b;
}

static void run_modulator(gp_t *gp)
{
	const uint16_t control = narrow(gp, CHORUS, N_CONTROL);
	const uint16_t phase = narrow(gp, CHORUS, N_PHASE);
	const uint32_t sum = (phase & 0x3fff) + narrow(gp, control & 0x1f, N_PITCH);

	gp_step_t current;
	current.address = wide_u(gp, CHORUS, W_ADDRESS);
	current.backward = bit(phase, 15);
	const gp_step_t chosen = ((sum >> 14) & 7) ? advance(gp, CHORUS, current, true) : current;

	if (bit(control, 5))
		set_wide(gp, CHORUS, W_ADDRESS, chosen.address);
	set_narrow(gp, CHORUS, N_PHASE, (uint16_t)((sum & 0x3fff) | (phase & 0x4000) | (chosen.backward ? 0x8000 : 0)));

	const uint32_t address = wide_u(gp, CHORUS, W_ADDRESS);
	set_narrow(gp, LINE_B, B_CHORUS_TAP_A,
			(uint16_t)(wide_u(gp, CHORUS, W_END) - (address - wide_u(gp, CHORUS, W_LOOP))));
	set_narrow(gp, LINE_B, B_CHORUS_TAP_B, (uint16_t)address);
}


/* ---------------------------------------------------------------- output stage */

static void run_output(gp_t *gp)
{
	static const int noise_masks[2][4] = { { 0, 0, 1, 3 }, { 0, 3, 7, 15 } };
	static const int32_t offsets[2][4] = { { 0, 1 << 6, 1 << 8, 0 }, { 0, 1 << 8, 1 << 10, 0 } };
	static const int residual[2] = { R_RESIDUAL_LEFT, R_RESIDUAL_RIGHT };
	static const int stored[2][2] = { { R_SAMPLE_LEFT_1, R_SAMPLE_LEFT_2 }, { R_SAMPLE_RIGHT_1, R_SAMPLE_RIGHT_2 } };

	const int sixteen_bit = ((gp->output_config & 0x30) != 0) ? 1 : 0;
	const int noise_mask = noise_masks[sixteen_bit][(gp->output_config >> 2) & 3];
	const int32_t truncate_mask = bit(gp->output_config, 7) ? (sixteen_bit ? 15 : 3) : 0;
	int32_t offset = offsets[sixteen_bit][gp->output_config & 3];
	if ((gp->output_config & 0x30) == 0x30)
		offset |= 1 << 12;

	uint16_t noise = narrow(gp, REVERB, R_NOISE);
	for (int i = 0; i < 2; i++)
	{
		const bool keep = (i == 0) || double_rate(gp);
		noise = (uint16_t)((noise >> 1) | ((uint16_t)(bit(noise, 0) ^ bit(noise, 1) ^ bit(noise, 7) ^ bit(noise, 12)) << 15));
		if (keep)
			set_narrow(gp, REVERB, R_NOISE, noise);

		const int32_t dither = offset | (noise & noise_mask);
		for (int channel = 0; channel < 2; channel++)
		{
			const int32_t sum = sat20((int64_t)gp->mix[channel] + wide_u(gp, REVERB, residual[channel]));
			gp->mix[channel] = sum;
			if (keep)
				set_wide(gp, REVERB, residual[channel], sum & truncate_mask);

			const int32_t shaped = sat20((int64_t)sum + dither);
			set_wide(gp, REVERB, stored[channel][i], shaped);
			if (keep)
				gp->sample[i][channel] = shaped & ~truncate_mask;
		}
	}
}

void gp_run_frame(gp_t *gp)
{
	run_output(gp);

	if (gp->first_frame)
		gp->frame_counter = narrow(gp, CHORUS, N_PHASE);
	gp->frame_counter = (gp->frame_counter - 1) & 0x3fff;

	run_effects(gp);
	run_modulator(gp);
	run_voices(gp);

	gp->first_frame = false;
}


/* ---------------------------------------------------------------- bus interface */

static void write_register(gp_t *gp, uint8_t address, uint8_t data)
{
	const uint8_t reg = group(address);

	if (!is_wide(address))
	{
		gp->write_latch = with_byte(gp->write_latch, 1 - (address & 1), data);
		if (bit(address, 0))
			set_narrow(gp, gp->selected_slot, reg, (uint16_t)gp->write_latch);
		return;
	}

	const int byte = address & 3;
	if (byte == 0)
		return;
	gp->write_latch = with_byte(gp->write_latch, 3 - byte, data) & MASK20;
	if (byte < 3)
		return;

	set_wide(gp, gp->selected_slot, reg, (int32_t)gp->write_latch);
	if (reg == W_FILTER_LP)
		gp->slots[gp->selected_slot].filter_low = wide(gp, gp->selected_slot, W_FILTER_LP);
	else if (reg == W_FILTER_BP)
		gp->slots[gp->selected_slot].filter_band = wide(gp, gp->selected_slot, W_FILTER_BP);
}

static void load_latch(gp_t *gp, uint8_t address)
{
	if (is_wide(address))
	{
		if ((address & 3) == 1)
			gp->read_latch = wide_u(gp, gp->selected_slot, group(address));
	}
	else if (!bit(address, 0))
	{
		gp->read_latch = narrow(gp, gp->selected_slot, group(address));
	}
}

void gp_write(gp_t *gp, uint8_t offset, uint8_t data)
{
	const uint8_t address = offset & 0x3f;

	if (address < 0x04)
	{
		gp->key_mask_pending = with_byte(gp->key_mask_pending, 3 - address, data) & 0x0fffffff;
		gp->key_mask_dirty = true;
	}
	else if (is_register(address))
	{
		write_register(gp, address, data);
	}
	else if (address < 0x24)
	{
		if (address != 0x20)
			gp->rom_address = with_byte(gp->rom_address, 0x23 - address, data);
		if (address == 0x23)
			gp->rom_byte = read_byte(gp, gp->rom_address & 0xffffff);
	}
	else
	{
		switch (address)
		{
		case 0x3c:
			gp->output_config = data;
			break;

		case 0x3d:
			gp->slot_config = data;
			break;

		case 0x3e:
			gp->selected_slot = data & 0x1f;
			break;

		default:
			break;
		}
	}
}

uint8_t gp_read(gp_t *gp, uint8_t offset)
{
	const uint8_t address = offset & 0x3f;

	if (address < 0x04)
	{
		if (gp->key_mask_dirty)
		{
			gp->key_mask = gp->key_mask_pending;
			gp->key_mask_dirty = false;
		}
	}
	else if (is_register(address))
	{
		load_latch(gp, address);
	}
	else if (address >= 0x39 && address <= 0x3b)
	{
		return (uint8_t)(gp->read_latch >> ((0x3b - address) * 8)) & (address == 0x39 ? 0x0f : 0xff);
	}
	else if (address == 0x3c || address == 0x3e)
	{
		if (address == 0x3e && gp->irq_pending)
		{
			gp->irq_pending = false;
			if (gp->link.irq)
				gp->link.irq(gp->link.user, false);
		}
		return gp->irq_slot | (gp->key_mask_dirty ? 0x20 : 0);
	}
	else if (address == 0x3f)
	{
		return gp->rom_byte;
	}

	return 0;
}


/* ---------------------------------------------------------------- device */

void gp_init(gp_t *gp, const gp_link_t *link, bool gp4, const uint8_t *wave, size_t wave_size)
{
	memset(gp, 0, sizeof(*gp));
	gp->link = *link;
	gp->gp4 = gp4;
	gp->wave = wave;
	gp->wave_size = wave_size;
	gp_reset(gp);
}

void gp_reset(gp_t *gp)
{
	memset(gp->slots, 0, sizeof(gp->slots));
	memset(gp->eram, 0, sizeof(gp->eram));
	gp->mix[0] = 0;
	gp->mix[1] = 0;

	gp->key_mask = 0;
	gp->key_mask_pending = 0;
	gp->key_mask_dirty = false;

	gp->write_latch = 0;
	gp->read_latch = 0;
	gp->rom_address = 0;
	gp->rom_byte = 0;

	gp->output_config = 0;
	gp->slot_config = 0;
	gp->selected_slot = 0;

	gp->irq_slot = 0;
	gp->irq_pending = false;
	if (gp->link.irq)
		gp->link.irq(gp->link.user, false);

	gp->frame_counter = 0;
	gp->first_frame = true;
	gp->send[0] = gp->send[1] = 0;
	memset(gp->returns, 0, sizeof(gp->returns));
	memset(gp->sample, 0, sizeof(gp->sample));
}

uint32_t gp_frame_clocks(const gp_t *gp) { return (uint32_t)(slot_count(gp) + 1) * 25; }
int gp_pairs(const gp_t *gp) { return double_rate(gp) ? 2 : 1; }
int32_t gp_output(const gp_t *gp, int pair, int channel) { return gp->sample[pair][channel]; }

uint32_t gp_output_rate(const gp_t *gp, uint32_t clock)
{
	return (clock / gp_frame_clocks(gp)) * (uint32_t)gp_pairs(gp);
}

uint32_t gp_wide(const gp_t *gp, int slot, int index)
{
	return gp->slots[slot].wide[index];
}

uint16_t gp_narrow(const gp_t *gp, int slot, int index)
{
	return gp->slots[slot].narrow[index];
}
