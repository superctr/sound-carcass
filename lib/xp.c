#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stddef.h>
#include "xp.h"

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

static const uint32_t hold_masks[4] = { 0, 7, 31, 127 };
static const uint8_t phase_dither[4] = { 0, 2, 1, 3 };
static const uint8_t shift_select[4] = { 0, 1, 2, 4 };

enum { RAMP_PITCH, RAMP_TVF, RAMP_RESO, RAMP_TVA2, RAMP_TVA1 };

typedef struct ramp_pages
{
	uint8_t current, control, target, step, reason;
} ramp_pages_t;

static const ramp_pages_t RAMPS[5] = {
	{ XP_PAGE_PITCH_SEED, XP_PAGE_PITCH_CONTROL, XP_PAGE_PITCH_TARGET, XP_PAGE_PITCH_STEP, XP_IRQ_PITCH_DONE },
	{ XP_PAGE_TVF_SEED, XP_PAGE_TVF_CONTROL, XP_PAGE_TVF_TARGET, XP_PAGE_TVF_STEP, XP_IRQ_TVF_DONE },
	{ XP_PAGE_RESO_SEED, XP_PAGE_RESO_CONTROL, XP_PAGE_RESO_TARGET, 0, XP_IRQ_RESO_DONE },
	{ XP_PAGE_TVA2_SEED, XP_PAGE_TVA2_CONTROL, XP_PAGE_TVA2_TARGET, 0, XP_IRQ_TVA2_DONE },
	{ XP_PAGE_TVA1_SEED, XP_PAGE_TVA1_CONTROL, XP_PAGE_TVA1_TARGET, XP_PAGE_TVA1_STEP, XP_IRQ_VOICE_DONE },
};

static inline int32_t clamp24(int64_t v) { return (int32_t)(v > 0x7fffff ? 0x7fffff : v < -0x800000 ? -0x800000 : v); }
static inline int32_t clamp29(int64_t v) { return (int32_t)(v > 0x0fffffff ? 0x0fffffff : v < -0x10000000 ? -0x10000000 : v); }
static inline int32_t wrap29(int64_t v) { return (int32_t)((int64_t)((uint64_t)v << 35) >> 35); }
static inline int32_t wrap24(int32_t v) { return (int32_t)((uint32_t)v << 8) >> 8; }
static inline int32_t wrap20(int32_t v) { return (int32_t)((uint32_t)v << 12) >> 12; }
static inline int32_t wrap18(int32_t v) { return (int32_t)((uint32_t)v << 14) >> 14; }
static inline int32_t gain_current(int32_t cell) { return (int16_t)(cell >> 10); }
static inline int voice_count(const xp_t *xp) { return (xp->regs[XP_HIGHEST_VOICE >> 1] & 0x3f) + 1; }
static inline int slot_count(const xp_t *xp) { return voice_count(xp) * 4; }
static inline int ramp_base(const xp_t *xp) { return 256 - ((xp->regs[XP_DSP_CONFIG >> 1] >> 8) & 0x1f); }
static inline int32_t min32(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t max32(int32_t a, int32_t b) { return a > b ? a : b; }
static inline bool bit(uint32_t v, int n) { return (v >> n) & 1; }
static inline int32_t multiply(int32_t operand, int32_t coefficient) { return clamp29(((int64_t)operand * coefficient) / 8192); }
static inline int32_t multiply_q15(int32_t operand, int32_t factor, int shift) { return clamp29((((int64_t)operand * factor) << shift) / 32768); }

static inline int cell_of(int word, int parity)
{
	if (word < 0x80)
		return ((bit(word, 6) ^ parity) << 6) | (word & 0x3f);
	if (word < 0xc0)
		return (bit(word, 5) << 6) | 0x20 | (word & 0x1f);
	return 0x80 | (word & 0x3f);
}

static inline int cell(const xp_t *xp, int word) { return cell_of(word, xp->parity); }

static int32_t gain_goal(int32_t cell)
{
	const int target = cell & 0x3ff;
	int32_t goal = (int16_t)(uint16_t)(target << 6);
	if (target > 0 && target < 0x1ff)
		goal++;
	return goal;
}

static int32_t fold24(int32_t value)
{
	uint32_t v = (uint32_t)value & 0xffffff;
	if (bit(v, 23) != bit(v, 22))
		v ^= 0x7fffff;
	return wrap24((int32_t)v);
}

static uint32_t page_mask(int index)
{
	if (index <= 0x04 || (index >= 0x20 && index <= 0x26))
		return 0xfffff;
	if (index >= 0x08 && index <= 0x0b)
		return 0xfff;
	if (index >= 0x0c && index <= 0x1e)
		return 0x3ffff;
	if (index == 0x27)
		return 0xffff;
	if (index >= 0x28 && index <= 0x2a)
		return 0xffffff;
	return 0xffffffff;
}

static int bit_reverse4(int v)
{
	return ((v & 1) << 3) | ((v & 2) << 1) | ((v & 4) >> 1) | ((v & 8) >> 3);
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
	if (getenv("SCEMU_XP_JIT") && !atoi(getenv("SCEMU_XP_JIT")))
		xp->interpret = true;
	return true;
}

void xp_release(xp_t *xp)
{
	jit_code_free(xp->jit, &xp->code[0]);
	jit_code_free(xp->jit, &xp->code[1]);
	xp->frame[0] = xp->frame[1] = NULL;
	free(xp->eram);
	xp->eram = NULL;
}

static void update_int(xp_t *xp)
{
	if (xp->irq_active != xp->int_state)
	{
		xp->int_state = xp->irq_active;
		xp->link.irq(xp->link.user, xp->int_state);
	}
}

void xp_reset(xp_t *xp)
{
	memset(xp->regs, 0, sizeof(xp->regs));
	memset(xp->eram, 0, XP_ERAM_SIZE * sizeof(int32_t));
	memset(xp->iram, 0, sizeof(xp->iram));
	memset(xp->iram_ramping, 0, sizeof(xp->iram_ramping));
	memset(xp->voices, 0, sizeof(xp->voices));
	memset(xp->still, 0, sizeof(xp->still));
	memset(&xp->dsp, 0, sizeof(xp->dsp));
	xp->dsp.latch = -0x800000;
	memset(xp->port_word, 0, sizeof(xp->port_word));
	memset(xp->port_a_out, 0, sizeof(xp->port_a_out));
	xp->program_dirty = true;
	xp->dsp_enabled = false;
	xp->parity = 0;
	xp->bus_written = 0;
	xp->run_mask = 0;
	xp->run_pending = 0;
	xp->read_latch = 0;
	xp->write_latch = 0;
	xp->frame_counter = 0;
	xp->irq_event = 0;
	xp->irq_active = false;
	xp->irq_frame_used = false;
	update_int(xp);
}

static bool offer_irq(xp_t *xp, int voice, int reason)
{
	if (!bit(xp->regs[XP_IRQ_STATUS >> 1], reason))
		return true;
	if (xp->irq_active || xp->irq_frame_used)
		return false;

	xp->irq_event = (uint16_t)((voice << 8) | reason);
	xp->irq_active = true;
	xp->irq_frame_used = true;
	update_int(xp);
	return true;
}

/* ---------------------------------------------------------------- host interface */

static inline int page_word(int voice, int index)
{
	return ((index & 0xff) << 7) | ((voice & 63) << 1);
}

#if defined __BYTE_ORDER__ && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
static inline uint32_t page(const xp_t *xp, int voice, int index)
{
	uint32_t halves;
	memcpy(&halves, &xp->regs[page_word(voice, index)], sizeof halves);
	return (halves << 16) | (halves >> 16);
}

static inline void set_page(xp_t *xp, int voice, int index, uint32_t value)
{
	const uint32_t halves = (value << 16) | (value >> 16);
	memcpy(&xp->regs[page_word(voice, index)], &halves, sizeof halves);
}
#else
static inline uint32_t page(const xp_t *xp, int voice, int index)
{
	const int word = page_word(voice, index);
	return ((uint32_t)xp->regs[word] << 16) | xp->regs[word | 1];
}

static inline void set_page(xp_t *xp, int voice, int index, uint32_t value)
{
	const int word = page_word(voice, index);
	xp->regs[word] = (uint16_t)(value >> 16);
	xp->regs[word | 1] = (uint16_t)value;
}
#endif

static inline uint16_t send(const xp_t *xp, int voice, int bank)
{
	return xp->regs[(XP_SEND_BASE >> 1) + (bank & 3) * 64 + (voice & 63)];
}

static inline bool running(const xp_t *xp, int voice) { return bit((uint32_t)(xp->run_mask >> (voice & 63)), 0); }

static inline uint8_t wave_byte(const xp_t *xp, uint32_t address)
{
	const uint32_t chip = address >> 24;
	const uint32_t offset = address & 0xffffff;
	if (offset >= xp->wave_chip_size)
		return 0;
	const size_t n = (size_t)chip * xp->wave_chip_size + offset;
	return n < xp->wave_size ? xp->wave[n] : 0;
}

static inline uint8_t rom_byte(const xp_t *xp, int region, uint32_t offset)
{
	return wave_byte(xp, ((uint32_t)(region & 0x7f) << 20) | (offset & 0xfffff));
}

static inline int host_cell(uint32_t address) { return (int)((address - XP_IRAM_BASE) >> 2); }

static void write_iram(xp_t *xp, int c, uint32_t value)
{
	xp->iram[c] = c >= 0x80 ? (int32_t)(value << 6) >> 6 : wrap24((int32_t)value);
	if (c >= 0x80)
		xp->iram_ramping[c & 0x3f] = 0;
}

static void write_iram_target(xp_t *xp, int word, uint16_t value)
{
	const int c = cell(xp, word);
	if (word >= ramp_base(xp))
	{
		xp->iram[c] = (xp->iram[c] & ~0x3ff) | (value & 0x3ff);
		xp->iram_ramping[c & 0x3f] = 1;
	}
	else
	{
		xp->iram[c] = value;
		xp->iram_ramping[c & 0x3f] = 0;
	}
}

static void load_latch(xp_t *xp, uint32_t address)
{
	if (address < XP_CRAM_BASE)
	{
		if (bit(address, 1))
			xp->read_latch = page(xp, (address >> 2) & 63, address >> 8) & page_mask(address >> 8);
	}
	else if (address < XP_IRAM_BASE)
		xp->read_latch = xp->regs[address >> 1];
	else if (address < XP_IRAM3_TARGET_BASE)
	{
		if (bit(address, 1))
		{
			const int c = host_cell(address);
			xp->read_latch = (uint32_t)xp->iram[c] & (c >= 128 ? 0x3ffffff : 0xffffff);
		}
	}
	else if (address < XP_PRAM_BASE)
		xp->read_latch = 0;
	else if (address < XP_RUN_MASK)
	{
		if (bit(address, 1))
			xp->read_latch = (((uint32_t)xp->regs[(address >> 1) & ~1] << 16) | xp->regs[address >> 1]) & 0x0fffffff;
	}
	else if (address >= XP_SEND_BASE && address < XP_ROM_WINDOW)
		xp->read_latch = xp->regs[address >> 1];
	else if (address >= XP_ROM_WINDOW)
	{
		const uint32_t byte = ((uint32_t)(xp->regs[XP_ROM_BANK >> 1] & 0x7f) << 20)
			| ((uint32_t)(xp->regs[XP_ROM_PAGE >> 1] & 0x3ff) << 10) | (address - XP_ROM_WINDOW);
		xp->read_latch = wave_byte(xp, byte) | ((uint32_t)wave_byte(xp, byte + 1) << 8);
	}
}

static uint32_t translate(const xp_t *xp, uint32_t address)
{
	if (address >= XP_VOICE_WINDOW && address < XP_VOICE_WINDOW_END)
		return ((address - XP_VOICE_WINDOW) >> 2) * 0x100 + (xp->regs[XP_VOICE_SELECT >> 1] & 0x3f) * 4 + (address & 2);
	if (address >= XP_SEND_WINDOW && address < XP_SEND_BASE)
		return XP_SEND_BASE + ((address - XP_SEND_WINDOW) >> 1) * 0x80 + (xp->regs[XP_VOICE_SELECT >> 1] & 0x3f) * 2;
	return address;
}

static void write_run_mask(xp_t *xp, int word, uint16_t data);
static void commit_run_mask(xp_t *xp);

uint16_t xp_read(xp_t *xp, uint32_t offset)
{
	const uint32_t address = translate(xp, (offset << 1) & 0x3ffe);
	uint16_t data = 0;

	if (address < XP_RUN_MASK || address >= XP_SEND_BASE)
		load_latch(xp, address);
	else
	{
		if (address < XP_ROM_SELECT)
			commit_run_mask(xp);
		switch (address)
		{
		case XP_RUN_MASK: case XP_RUN_MASK + 2: case XP_RUN_MASK + 4: case XP_RUN_MASK + 6:
		case XP_DSP_MODE:
		case XP_VOICE_SELECT:
			break;
		case XP_READBACK_LOW:
			data = xp->read_latch & 0xffff;
			break;
		case XP_READBACK_HIGH:
			data = (uint16_t)(xp->read_latch >> 16);
			break;
		case XP_IRQ_STATUS:
			data = xp->irq_event;
			break;
		case XP_IRQ_ACK:
			xp->irq_active = false;
			update_int(xp);
			break;
		case XP_STATUS:
			data = (xp->regs[XP_STATUS >> 1] & ~0x40) | (((xp->regs[XP_DIAG_SELECT >> 1] & 0x7ff) >= 0x5a0) ? 0x40 : 0);
			break;
		default:
			data = xp->regs[address >> 1];
			break;
		}
	}
	return data;
}

void xp_write(xp_t *xp, uint32_t offset, uint16_t data, uint16_t mask)
{
	const uint32_t address = translate(xp, (offset << 1) & 0x3ffe);
	data = (uint16_t)((xp->regs[address >> 1] & ~mask) | (data & mask));

	if (address < XP_CRAM_BASE)
	{
		if (!bit(address, 1))
			xp->write_latch = data;
		else
		{
			xp->regs[(address >> 1) & ~1] = xp->write_latch;
			xp->regs[address >> 1] = data;
			xp->still[(address >> 2) & 63] = 0;
		}
	}
	else if (address < XP_IRAM_BASE)
	{
		xp->regs[address >> 1] = data;
		xp->program_dirty = true;
	}
	else if (address < XP_IRAM3_TARGET_BASE)
	{
		if (!bit(address, 1))
			xp->write_latch = data;
		else
			write_iram(xp, host_cell(address), ((uint32_t)xp->write_latch << 16) | data);
	}
	else if (address < XP_PRAM_BASE)
	{
		xp->regs[address >> 1] = data;
		write_iram_target(xp, 0xe0 + ((address >> 1) & 0x1f), data);
	}
	else if (address < XP_RUN_MASK)
	{
		if (!bit(address, 1))
			xp->write_latch = data;
		else
		{
			xp->regs[(address >> 1) & ~1] = xp->write_latch;
			xp->regs[address >> 1] = data;
			xp->program_dirty = true;
		}
	}
	else if (address < XP_SEND_BASE)
	{
		xp->regs[address >> 1] = data;
		if (address < XP_ROM_SELECT)
			write_run_mask(xp, (int)((address - XP_RUN_MASK) >> 1), data);
		else if (address == XP_HIGHEST_VOICE || address == XP_DSP_CONFIG)
			xp->program_dirty = true;
	}
	else if (address < XP_ROM_WINDOW)
		xp->regs[address >> 1] = data;
}

static void write_run_mask(xp_t *xp, int word, uint16_t data)
{
	const uint64_t written = (uint64_t)data << (word * 16);
	const uint64_t field = (uint64_t)0xffff << (word * 16);
	const uint64_t cleared = xp->run_mask & field & ~written;

	xp->run_mask &= ~cleared;
	xp->run_pending = (xp->run_pending & ~field) | (written & ~xp->run_mask);
	for (int n = word * 16; n < word * 16 + 16; n++)
		if ((cleared >> n) & 1)
		{
			xp->voices[n].phase = XP_IDLE;
			xp->still[n] = 0;
		}
}

static void commit_run_mask(xp_t *xp)
{
	const uint64_t launched = xp->run_pending;
	if (!launched)
		return;
	xp->run_mask |= launched;
	xp->run_pending = 0;
	for (int n = 0; n < XP_VOICES; n++)
		if ((launched >> n) & 1)
		{
			xp->voices[n].phase = XP_PRELOAD;
			xp->still[n] = 0;
		}
}

/* ---------------------------------------------------------------- ramps */

static bool linear_law(int index, uint32_t control)
{
	switch (index)
	{
	case RAMP_PITCH: case RAMP_TVF: return true;
	case RAMP_TVA1: return ((control >> 14) & 3) == 1;
	default: return false;
	}
}

static bool s_curve_law(int index, uint32_t control)
{
	return index == RAMP_TVA1 && ((control >> 14) & 3) >= 2;
}

static void service_ramp(xp_t *xp, int n, int k)
{
	const ramp_pages_t *rp = &RAMPS[k];
	uint32_t control = page(xp, n, rp->control);
	if (bit(control, 17))
		return;

	const bool linear = linear_law(k, control);
	const bool s_curve = s_curve_law(k, control);
	const int rate = control & 0xfff;
	const bool reso = k == RAMP_RESO;
	const uint32_t tpage = page(xp, n, rp->target);
	const int32_t target = s_curve ? 0 : reso ? (int32_t)(tpage & 0x3fffe) << 2 : (int32_t)(tpage & 0x3fffe);
	int32_t current = (int32_t)(page(xp, n, rp->current) & (reso ? 0xfffff : 0x3ffff));
	int32_t step = rp->step ? wrap20((int32_t)page(xp, n, rp->step)) : 0;

	if (!bit(tpage, 0))
	{
		set_page(xp, n, rp->target, tpage | 1);
		if (linear)
		{
			const int32_t diff = target - current;
			step = 2 * (((diff >> 3) * rate) >> 10) + (diff > 0 ? 1 : 0);
			set_page(xp, n, rp->step, (uint32_t)step & 0xfffff);
		}
	}

	const uint32_t tick = (k == RAMP_TVA1) ? (xp->frame_counter >> 1) : (xp->frame_counter >> 3);
	if (tick & hold_masks[(control >> 12) & 3])
		return;
	const int32_t parity = tick & 1;

	if (s_curve)
	{
		if (current > 0)
		{
			int32_t speed = -step;
			const int64_t stopping = rate ? ((int64_t)speed * (speed + rate)) / (2 * rate) : 0;
			if (speed < 0 || stopping < current)
				speed += rate;
			else
				speed -= rate;
			speed = max32(speed, 1);
			current = max32(current - speed, 0);
			step = -speed;
			set_page(xp, n, rp->step, (uint32_t)step & 0xfffff);
		}
	}
	else if (linear)
	{
		if (current != target)
		{
			const int32_t moved = current + ((step + parity) >> 1);
			current = (target > current) ? min32(moved, target) : max32(moved, target);
		}
	}
	else if (reso)
	{
		const int32_t quarter = (target - current) >> 2;
		int32_t s = ((quarter >> 3) * rate) >> 10;
		if (!s)
			s = quarter > 0 ? parity : quarter < 0 ? -1 : 0;
		current = (current + (s << 2)) & ~1;
		current = (quarter > 0) ? min32(current, target) : max32(current, target);
	}
	else
	{
		const int32_t diff = target - current;
		int32_t s = ((diff >> 3) * rate) >> 10;
		if (!s)
			s = diff > 0 ? parity : diff < 0 ? -1 : 0;
		current = max32(min32(current + s, 0x3ffff), 0);
		current = (diff > 0) ? min32(current, target) : max32(current, target);
	}
	set_page(xp, n, rp->current, (uint32_t)current);

	if (current == target)
	{
		if (s_curve && ((control >> 14) & 3) == 2)
			control |= 0x4000;
		if (bit(control, 16) && offer_irq(xp, n, rp->reason))
			control |= 0x20000;
		set_page(xp, n, rp->control, control);
	}
}

static void update_amplitude(xp_t *xp, int n)
{
	const int32_t tva1 = page(xp, n, XP_PAGE_TVA1_SEED) & 0x3ffff;
	const int32_t tva2 = page(xp, n, XP_PAGE_TVA2_SEED) & 0x3ffff;
	int32_t amplitude;
	if (bit(page(xp, n, XP_PAGE_TVA2_CONTROL), 14))
	{
		int64_t sum = ((int64_t)tva1 + tva2) << 2;
		amplitude = (int32_t)(sum > 0xffffe ? 0xffffe : sum < 0 ? 0 : sum) & ~1;
	}
	else
		amplitude = (int32_t)(((int64_t)(tva1 >> 3) * (tva2 >> 4)) >> 8) & ~1;
	set_page(xp, n, XP_PAGE_AMPLITUDE, (uint32_t)amplitude);
}

/* ---------------------------------------------------------------- address generator and DPCM */

typedef struct address_step
{
	uint32_t address;
	bool backward;
} address_step_t;

typedef struct reader_bounds
{
	uint32_t loop, end, start;
	bool looping, alternate, reverse;
} reader_bounds_t;

static reader_bounds_t reader_bounds(const xp_t *xp, int n, uint32_t control)
{
	reader_bounds_t b;
	b.loop = page(xp, n, XP_PAGE_LOOP) & 0xfffff;
	b.end = page(xp, n, XP_PAGE_END) & 0xfffff;
	b.start = xp->voices[n].start;
	b.looping = b.loop < b.end;
	b.alternate = bit(control, 12);
	b.reverse = bit(control, 11);
	return b;
}

static const uint8_t *region_base(const xp_t *xp, int region)
{
	const uint32_t address = (uint32_t)(region & 0x7f) << 20;
	const uint32_t chip = address >> 24;
	const uint32_t offset = address & 0xffffff;
	if (offset + 0x100000 > xp->wave_chip_size)
		return NULL;
	const size_t n = (size_t)chip * xp->wave_chip_size + offset;
	return n + 0x100000 <= xp->wave_size ? xp->wave + n : NULL;
}

static inline uint8_t region_byte(const xp_t *xp, int region, const uint8_t *base, uint32_t offset)
{
	return base ? base[offset & 0xfffff] : rom_byte(xp, region, offset);
}

static int32_t delta_at(const xp_t *xp, int region, const uint8_t *base, int format, uint32_t address)
{
	const uint8_t byte = region_byte(xp, region, base, address);
	if (bit(format, 1))
	{
		const int shift = (byte >> 4) & 7;
		const int mantissa = byte & 0x0f;
		const int32_t magnitude = (shift ? (mantissa + 16) << (shift - 1) : mantissa) << 6;
		return bit(byte, 7) ? -magnitude : magnitude;
	}
	if (bit(format, 0))
		return (int8_t)byte;

	const uint8_t shifts = region_byte(xp, region, base, address >> 5);
	return (int32_t)(int8_t)byte << (bit(address, 4) ? (shifts >> 4) : (shifts & 0x0f));
}

static void launch(xp_t *xp, int n)
{
	xp_voice_t *v = &xp->voices[n];
	const uint32_t control = page(xp, n, XP_PAGE_CONTROL);

	v->format = (uint8_t)((bit(control, 9) << 1) | bit(control, 7));
	v->start = page(xp, n, XP_PAGE_ADDRESS) & 0xfffff;
	if (bit(control, 11))
		set_page(xp, n, XP_PAGE_ADDRESS, page(xp, n, XP_PAGE_END) & 0xfffff);
	set_page(xp, n, XP_PAGE_CONTROL, control | 0x80);

	const uint32_t address = page(xp, n, XP_PAGE_ADDRESS) & 0xfffff;
	const uint32_t span = (address >> 5) & ~1u;
	set_page(xp, n, XP_PAGE_EXPONENTS, rom_byte(xp, (int)control, span) | ((uint32_t)rom_byte(xp, (int)control, span + 1) << 8));
}

static address_step_t advance(const reader_bounds_t *b, address_step_t s)
{
	const uint32_t loop = b->loop;
	const uint32_t end = b->end;
	const bool looping = b->looping;
	const bool alternate = b->alternate;
	const bool reverse = b->reverse;

	if (!s.backward)
	{
		if (!looping)
			return (address_step_t){ s.address >= end ? s.address : s.address + 1, false };
		if (s.address >= end)
			return alternate ? (address_step_t){ s.address, true } : (address_step_t){ loop, false };
		return (address_step_t){ s.address + 1, false };
	}

	const uint32_t bound = (alternate && looping) ? loop : (reverse && !looping) ? b->start : loop;
	if (s.address <= bound)
	{
		if (alternate && looping)
			return (address_step_t){ s.address, false };
		return (address_step_t){ s.address, true };
	}
	return (address_step_t){ s.address - 1, true };
}

static bool at_marker(const reader_bounds_t *b, address_step_t s)
{
	return s.backward ? (s.address <= b->loop) : (s.address >= b->loop);
}

static void marker_reached(xp_t *xp, int n)
{
	const uint32_t control = page(xp, n, XP_PAGE_CONTROL);
	if (bit(control, 16))
		return;
	if (bit(control, 15) && !offer_irq(xp, n, bit(control, 14) ? XP_IRQ_LOOP_ALTERNATE : XP_IRQ_LOOP_REACHED))
		return;
	set_page(xp, n, XP_PAGE_CONTROL, control | (bit(control, 17) ? 0x10000 : 0x20000));
}

static void update_mute(xp_t *xp, int n)
{
	const uint32_t control = page(xp, n, XP_PAGE_CONTROL);
	if (bit(control, 19) == bit(control, 18))
		return;
	if (bit(control, 15) && !offer_irq(xp, n, XP_IRQ_MUTE_CHANGED))
		return;
	set_page(xp, n, XP_PAGE_CONTROL, control ^ 0x40000);
}

/* ---------------------------------------------------------------- the voice */

static void deposit(xp_t *xp, int n, int bank, int32_t output)
{
	const uint16_t s = send(xp, n, bank);
	const int word = s & 63;
	const int c = cell_of(0x40 + word, xp->parity ^ 1);

	if (!((xp->bus_written >> word) & 1))
	{
		xp->bus_written |= (uint64_t)1 << word;
		xp->iram[c] = 0;
	}
	if (output)
		xp->iram[c] = clamp24(xp->iram[c] + ((int64_t)output * (s >> 6)) / 512);
}

static void run_voice(xp_t *xp, int n)
{
	xp_voice_t *v = &xp->voices[n];

	if (!running(xp, n))
	{
		const int32_t smooth = page(xp, n, XP_PAGE_SMOOTH) & 0xffff;
		set_page(xp, n, XP_PAGE_SMOOTH, (uint32_t)max32((smooth * 7) >> 3, 1));
		set_page(xp, n, XP_PAGE_OUTPUT, 0);
		return;
	}

	update_mute(xp, n);

	switch (v->phase)
	{
	case XP_PRELOAD:
		launch(xp, n);
		v->phase = XP_INITIALIZE;
		set_page(xp, n, XP_PAGE_OUTPUT, 0);
		return;
	case XP_INITIALIZE:
		set_page(xp, n, XP_PAGE_INCREMENT, (uint32_t)exp_decode(xp, page(xp, n, XP_PAGE_PITCH_SEED) & 0x3ffff) & 0x3ffff);
		set_page(xp, n, XP_PAGE_CUTOFF, (uint32_t)(exp_decode(xp, page(xp, n, XP_PAGE_TVF_SEED) & 0x3ffff) << 2) & 0xfffff);
		service_ramp(xp, n, RAMP_PITCH);
		set_page(xp, n, XP_PAGE_SERVICE, (page(xp, n, XP_PAGE_SERVICE) & ~0x30000u) | 0x10000);
		v->phase = XP_STARTING;
		set_page(xp, n, XP_PAGE_OUTPUT, 0);
		return;
	case XP_STARTING:
		service_ramp(xp, n, RAMP_TVF);
		set_page(xp, n, XP_PAGE_SERVICE, page(xp, n, XP_PAGE_SERVICE) | 0x30000);
		v->phase = XP_RUNNING;
		set_page(xp, n, XP_PAGE_OUTPUT, 0);
		return;
	default:
		break;
	}

	const uint32_t service = page(xp, n, XP_PAGE_SERVICE);
	const int counter = service & 7;
	if (counter & 1)
	{
		service_ramp(xp, n, RAMP_TVA1);
		update_amplitude(xp, n);
	}
	else
	{
		switch (counter)
		{
		case 0:
			service_ramp(xp, n, RAMP_TVA2);
			break;
		case 2:
			service_ramp(xp, n, RAMP_RESO);
			break;
		case 4:
			service_ramp(xp, n, RAMP_TVF);
			set_page(xp, n, XP_PAGE_CUTOFF, (uint32_t)(exp_decode(xp, page(xp, n, XP_PAGE_TVF_SEED) & 0x3ffff) << 2) & 0xfffff);
			break;
		default:
			service_ramp(xp, n, RAMP_PITCH);
			set_page(xp, n, XP_PAGE_INCREMENT, (uint32_t)exp_decode(xp, page(xp, n, XP_PAGE_PITCH_SEED) & 0x3ffff) & 0x3ffff);
			break;
		}
	}
	set_page(xp, n, XP_PAGE_SERVICE, (service & ~7u) | ((counter + 1) & 7));

	int32_t smooth = (7 * (int32_t)(page(xp, n, XP_PAGE_SMOOTH) & 0xffff) + (int32_t)((page(xp, n, XP_PAGE_AMPLITUDE) & 0xfffff) >> 4) + 3) >> 3;
	smooth = max32(min32(smooth, 0xffff), 0);
	set_page(xp, n, XP_PAGE_SMOOTH, (uint32_t)smooth);

	uint32_t control = page(xp, n, XP_PAGE_CONTROL);
	int32_t sample = 0;
	if (!bit(control, 10))
	{
		uint32_t phase = (page(xp, n, XP_PAGE_PHASE) >> 4) & 0x3fff;
		const uint32_t address = page(xp, n, XP_PAGE_ADDRESS) & 0xfffff;
		int32_t predictor = wrap18((int32_t)page(xp, n, XP_PAGE_PREDICTOR));
		const bool backward = bit(control, 11) ^ bit(control, 13);
		const reader_bounds_t bounds = reader_bounds(xp, n, control);
		const int region = (int)control;
		const uint8_t *base = region_base(xp, region);
		const int format = v->format;

		address_step_t s = { address, backward };
		int64_t sum = 4 * (int64_t)predictor;
		for (int i = 0; i < 3; i++)
		{
			sum += ((int64_t)interp_weights[i][phase >> 7] * delta_at(xp, region, base, format, s.address)) / 1024;
			s = advance(&bounds, s);
		}
		sample = wrap20((int32_t)sum) / (1 << (3 - ((service >> 3) & 3)));

		const uint32_t increment = page(xp, n, XP_PAGE_INCREMENT) & 0x3ffff;
		const uint32_t span = address >> 6;
		const uint32_t accumulated = phase + (increment >> 2) + (((increment & 3) > phase_dither[xp->frame_counter & 3]) ? 1 : 0);
		phase = accumulated & 0x3fff;
		address_step_t current = { address, backward };
		for (uint32_t carry = accumulated >> 14; carry; carry--)
		{
			predictor = wrap18(predictor + delta_at(xp, region, base, format, current.address));
			if (at_marker(&bounds, current))
				marker_reached(xp, n);
			const address_step_t next = advance(&bounds, current);
			if (next.backward != current.backward)
			{
				control ^= 0x2000;
				set_page(xp, n, XP_PAGE_CONTROL, control);
			}
			current = next;
		}
		if ((current.address >> 6) != span)
		{
			const uint32_t exponents = (current.address >> 5) & ~1u;
			set_page(xp, n, XP_PAGE_EXPONENTS, rom_byte(xp, (int)control, exponents) | ((uint32_t)rom_byte(xp, (int)control, exponents + 1) << 8));
		}
		set_page(xp, n, XP_PAGE_PHASE, (phase << 4) | (current.address & 7));
		set_page(xp, n, XP_PAGE_ADDRESS, current.address);
		set_page(xp, n, XP_PAGE_PREDICTOR, (uint32_t)predictor & 0x3ffff);
	}

	const int32_t f = page(xp, n, XP_PAGE_CUTOFF) & 0xfffff;
	const int32_t q = page(xp, n, XP_PAGE_RESO_SEED) & 0xfffff;
	int32_t low = wrap24((int32_t)page(xp, n, XP_PAGE_FILTER_LOW));
	int32_t band = wrap24((int32_t)page(xp, n, XP_PAGE_FILTER_BAND));
	low = clamp24(low + ((int64_t)f * band) / (1 << 19));
	const int32_t high = clamp24(sample - ((int32_t)(((int64_t)q * band) / (1 << 19)) + low));
	band = clamp24(band + ((int64_t)f * high) / (1 << 19));
	set_page(xp, n, XP_PAGE_FILTER_LOW, (uint32_t)low & 0xffffff);
	set_page(xp, n, XP_PAGE_FILTER_BAND, (uint32_t)band & 0xffffff);
	switch ((page(xp, n, XP_PAGE_FILTER) >> 10) & 3)
	{
	case 0: sample = low; break;
	case 1: sample = band; break;
	case 2: sample = high; break;
	case 3: sample = clamp24((int64_t)high - low); break;
	}

	set_page(xp, n, XP_PAGE_OUTPUT, (uint32_t)clamp24(((int64_t)sample * (smooth << 4)) / (1 << 19)) & 0xffffff);
}

/* --- a voice whose frame is the identity but for its service counter is stepped by that counter alone */

static bool ramp_settled(const xp_t *xp, int n, int k)
{
	const ramp_pages_t *rp = &RAMPS[k];
	const uint32_t control = page(xp, n, rp->control);
	if (bit(control, 17))
		return true;
	if (bit(control, 16))
		return false;
	const bool s_curve = s_curve_law(k, control);
	if (s_curve && ((control >> 14) & 3) == 2)
		return false;
	const uint32_t tpage = page(xp, n, rp->target);
	if (!bit(tpage, 0))
		return false;
	const bool reso = k == RAMP_RESO;
	const uint32_t mask = reso ? 0xfffff : 0x3ffff;
	const uint32_t cpage = page(xp, n, rp->current);
	if (cpage & ~mask)
		return false;
	const int32_t target = s_curve ? 0 : reso ? (int32_t)(tpage & 0x3fffe) << 2 : (int32_t)(tpage & 0x3fffe);
	return (int32_t)cpage == target && !(reso && (cpage & 1));
}

static bool voice_settled(const xp_t *xp, int n)
{
	if (xp->voices[n].phase != XP_RUNNING)
		return false;
	const uint32_t control = page(xp, n, XP_PAGE_CONTROL);
	if (!bit(control, 10) && (page(xp, n, XP_PAGE_INCREMENT) & 0x3ffff))
		return false;
	if (bit(control, 19) != bit(control, 18))
		return false;
	for (int k = 0; k < 5; k++)
		if (!ramp_settled(xp, n, k))
			return false;
	return true;
}

#define XP_PAGES (XP_CRAM_BASE >> 8)
#define XP_STILL_FRAMES 8

static void run_voice_watched(xp_t *xp, int n)
{
	if (xp->still[n] >= XP_STILL_FRAMES)
	{
		const uint32_t service = page(xp, n, XP_PAGE_SERVICE);
		set_page(xp, n, XP_PAGE_SERVICE, (service & ~7u) | ((service + 1) & 7));
		return;
	}
	if (!voice_settled(xp, n))
	{
		xp->still[n] = 0;
		run_voice(xp, n);
		return;
	}
	uint32_t before[XP_PAGES];
	for (int i = 0; i < XP_PAGES; i++)
		before[i] = page(xp, n, i);
	run_voice(xp, n);
	bool same = true;
	for (int i = 0; i < XP_PAGES && same; i++)
		same = ((before[i] ^ page(xp, n, i)) & (i == XP_PAGE_SERVICE ? ~7u : ~0u)) == 0;
	xp->still[n] = same ? xp->still[n] + 1 : 0;
}

/* ---------------------------------------------------------------- the DSP */

static void update_iram_ramps(xp_t *xp)
{
	if (!(xp->frame_counter & 1))
		return;

	const int threshold = bit_reverse4((xp->frame_counter >> 1) & 15);
	for (int word = ramp_base(xp); word < 256; word++)
	{
		const int c = cell(xp, word);
		if (!xp->iram_ramping[c & 0x3f])
			continue;

		const int32_t code = xp->iram[c] & 0x3ff;
		if (code == 0x200)
		{
			xp->iram_ramping[c & 0x3f] = 0;
			continue;
		}

		const int32_t goal = gain_goal(xp->iram[c]);
		int32_t current = gain_current(xp->iram[c]);
		const int32_t diff = goal - current;
		if (diff)
		{
			const int64_t product = (int64_t)(diff < 0 ? -diff : diff) * xp->regs[(XP_IRAM3_RATE >> 1) + (word & 3)];
			int32_t step = (int32_t)(product >> 17);
			int fraction = (int)(((product & 0x1ffff) + 0x1000) >> 13);
			if (fraction >= 16)
			{
				step++;
				fraction -= 16;
			}
			if (diff > 0)
			{
				if (threshold < fraction)
					step++;
				current = min32(current + step, goal);
			}
			else
			{
				if (threshold >= 16 - fraction)
					step++;
				if (!step)
					step = 2;
				current = max32(current - step, goal);
			}
			xp->iram[c] = ((current << 10) | code) & 0x3ffffff;
		}
		if (current == goal)
			xp->iram_ramping[c & 0x3f] = 0;
	}
}

static bool special(const xp_slot_t *s, int which)
{
	return s->function == 0 && s->input == which;
}

void xp_decode_program(xp_t *xp)
{
	const uint16_t *pram = &xp->regs[XP_PRAM_BASE >> 1];
	const uint16_t *cram = &xp->regs[XP_CRAM_BASE >> 1];
	xp->branching = false;
	for (int i = 0; i < XP_DSP_SLOTS; i++)
	{
		const uint32_t w = ((uint32_t)pram[i * 2] << 16) | pram[i * 2 + 1];
		const uint16_t c = cram[i];
		const int32_t mantissa = (int32_t)(int16_t)(c << 2) >> 2;
		xp_slot_t *s = &xp->slots[i];
		s->st = (w >> 14) & 3;
		s->word = (w >> 6) & 0xff;
		s->input = (w >> 4) & 3;
		s->function = w & 0xf;
		s->ext = (w >> 25) & 7;
		s->eram_op = 0;
		s->eram_second = 0;
		s->eram_offset = 0;
		s->cram = c;
		s->coefficient = mantissa << shift_select[c >> 14];
		s->raw = bit(c, 15) ? (int32_t)((c & 0x3fff) << 13) : mantissa;
		if (special(s, XP_SPECIAL_BRANCH) && i < slot_count(xp))
			xp->branching = true;
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
			xp->slots[i + 1].eram_second = 1;
			i += 2;
		}
		else
			i++;
	}
}

void xp_schedule(xp_t *xp)
{
	const xp_slot_t *s = xp->slots;
	xp_sched_t *o = xp->sched;
	uint8_t reads[XP_DSP_SLOTS];
	int strobes_a = 0, strobes_bcd = 0, position = 0;

	const int base = ramp_base(xp);
	for (int i = 0; i < XP_DSP_SLOTS; i++)
		reads[i] = s[i].eram_op == 1 || (special(&s[i], XP_SPECIAL_INDEXED_READ) && !s[i].eram_second);
	for (int i = 0; i < XP_DSP_SLOTS; i++)
	{
		o[i].lands = i >= 2 && reads[i - 2];
		o[i].now_valid = s[i].st == 1 && s[i].word < base;
		o[i].gain_load = s[i].st == 1 && s[i].word >= base;
		o[i].strobe_a = 0xff;
		o[i].strobe_bcd = 0xff;
		o[i].position = 0xff;
		if (s[i].ext == 1)
		{
			o[i].strobe_a = (uint8_t)(strobes_a++ & (XP_STROBES - 1));
			o[i].position = (uint8_t)position++;
		}
		else if (s[i].ext == 2)
		{
			o[i].strobe_bcd = (uint8_t)strobes_bcd++;
			o[i].position = (uint8_t)position++;
		}
	}
}

static int32_t wire_word(const xp_t *xp, int32_t word)
{
	return clamp24(word) & (bit(xp->regs[XP_SERIAL_FORMAT >> 1], 5) ? ~0x3ff : ~0x3f);
}

/* an `ext=2` strobe clocks the previous frame's word at its position out on its port */
static void emit_port(xp_t *xp, int position, int k)
{
	const int32_t word = clamp24(xp->iram[cell_of(position, xp->parity ^ 1)]);
	const int port = k % 3;
	const int half = (k / 3) & 1;
	const int descriptor = port == XP_PORT_B ? (xp->regs[XP_SERIAL_CONFIG >> 1] >> 8) : (xp->regs[XP_DSP_CONFIG >> 1] & 0xff);
	const int32_t out = (descriptor & 0xc0) ? wire_word(xp, word) : 0;
	xp->port_word[port][half] = out;
	if ((descriptor & 0xc0) && (xp->regs[XP_DSP_MODE >> 1] & 3) == 3 && xp->link.port_out)
		xp->link.port_out(xp->link.user, port * 2 + half, out);
}

static int32_t take_port_a(xp_t *xp, int strobe)
{
	const bool enabled = bit(xp->regs[XP_DSP_MODE >> 1], 1);
	return (enabled && xp->link.port_a_in) ? wrap24(xp->link.port_a_in(xp->link.user, strobe)) : 0;
}

static int32_t take_port_b(xp_t *xp, int group)
{
	const bool enabled = bit(xp->regs[XP_DSP_MODE >> 1], 1);
	return (enabled && xp->link.port_b_in) ? wrap24(xp->link.port_b_in(xp->link.user, group)) : 0;
}

/* --- the interpreter: the slot as the device steps it */

static int32_t operand(const xp_t *xp, const xp_slot_t *s)
{
	switch (s->input)
	{
	case XP_INPUT_PREVIOUS: return xp->dsp.input;
	case XP_INPUT_ACC: return clamp24(xp->dsp.acc);
	case XP_INPUT_R: return xp->dsp.r;
	default: return xp->dsp.latch;
	}
}

static int32_t factor(const xp_t *xp, int select, bool complement)
{
	const int32_t acc = clamp24(xp->dsp.acc);
	int32_t f;
	switch (select)
	{
	case 0: f = (acc & 0xfff) << 3; return complement ? 0x7fff - f : f;
	case 1: f = (acc & 0x7fffff) >> 8; return complement ? 0x7fff - f : f;
	case 2: f = acc >> 8; return complement ? ~f : f;
	default: f = xp->dsp.gain; return complement ? ~f : f;
	}
}

static void alu(xp_t *xp, int function, int mode, int32_t immediate)
{
	xp_dsp_state_t *d = &xp->dsp;
	const int32_t p = d->product;
	const int32_t r = d->r;
	int64_t result;

	switch (function)
	{
	case 0x2: result = (int64_t)d->acc + r; break;
	case 0x3: result = (int64_t)d->acc + p; break;
	case 0x4: result = r; break;
	case 0x5: result = p; break;
	case 0x6: result = -(int64_t)d->acc; break;
	case 0x7: result = (int64_t)r - d->acc; break;
	case 0x8: result = (int64_t)p - d->acc; break;
	case 0x9: result = (int64_t)r + p; break;
	case 0xa: result = min32(d->acc, r); break;
	case 0xb: result = max32(d->acc, r); break;
	case 0xc:
		if (mode == 2)
			result = (int64_t)d->acc + (p >> 13);
		else if (mode == 3)
			result = p >> 13;
		else
			return;
		break;
	case 0xd:
		switch (mode)
		{
		case 0: result = d->acc & immediate; break;
		case 1: result = d->acc | immediate; break;
		case 2: result = d->acc ^ immediate; break;
		default: return;
		}
		break;
	case 0xe:
		if (mode == 0)
			result = min32(d->acc, immediate);
		else if (mode == 1)
			result = max32(d->acc, immediate);
		else
			return;
		break;
	case 0xf:
		switch (mode)
		{
		case 0: result = (int64_t)d->acc + immediate; break;
		case 1: result = (int64_t)r + immediate; break;
		case 2: result = (int64_t)p + immediate; break;
		default: result = (int64_t)immediate - d->acc; break;
		}
		break;
	default:
		return;
	}
	d->acc = wrap29(result);
}

static void parallel_op(xp_t *xp, const xp_slot_t *s)
{
	xp_dsp_state_t *d = &xp->dsp;
	const uint16_t c = s->cram;
	const int function = c & 0xf;
	const int input_select = (c >> 4) & 3;
	const int factor_select = (c >> 6) & 3;
	const bool complement = bit(c, 8);
	const bool multiply_issued = bit(c, 9);
	const int post = (c >> 11) & 7;
	const int shift = shift_select[c >> 14];
	const int32_t p = d->product;
	const int32_t r = d->r;
	int32_t input, product;

	if (!multiply_issued)
	{
		input = d->now_valid ? d->now : 0;
		product = input;
	}
	else
	{
		switch (input_select)
		{
		case 0: input = d->input; break;
		case 1: input = clamp24(d->acc); break;
		case 2: input = r; break;
		default: input = d->latch; break;
		}
		product = multiply_q15(input, factor(xp, factor_select, complement), shift);
	}

	int64_t result;
	switch (function)
	{
	case 0x0: result = (int64_t)d->acc + p + r; break;
	case 0xc: result = (int64_t)r + p - d->acc; break;
	case 0xd: result = (int64_t)d->acc + p - r; break;
	case 0xe: result = (int64_t)p - r - d->acc; break;
	case 0xf: result = (int64_t)p - r; break;
	default:
		alu(xp, function, input_select, s->raw);
		result = d->acc;
		break;
	}
	d->acc = wrap29(result);

	switch (post)
	{
	case 1: d->acc = d->acc < 0 ? -d->acc : d->acc; break;
	case 2: d->acc = wrap24(d->acc); break;
	case 3:
	{
		const uint32_t v = (uint32_t)d->acc & 0xffffff;
		d->acc = wrap24((int32_t)((v << 1) | (((v >> 23) ^ (v >> 6) ^ (v >> 1)) & 1)));
		break;
	}
	case 4: d->acc = fold24(d->acc); break;
	case 5:
		d->acc = wrap24(d->acc);
		if (d->acc < 0)
			d->acc = ~d->acc;
		break;
	default: break;
	}
	if (bit(c, 10))
		d->acc = wrap24(d->acc);

	d->input = input;
	d->product = product;
}

/* returns the next program counter */
static int execute(xp_t *xp, const xp_slot_t *s, int pc)
{
	xp_dsp_state_t *d = &xp->dsp;

	if (special(s, XP_SPECIAL_PARALLEL))
	{
		parallel_op(xp, s);
		return pc + 1;
	}

	const int mode = s->input;
	const int function = s->function;
	const bool multiply_issued = function >= 1 && function <= 0xc;

	int32_t input = 0, product = 0;
	if (multiply_issued)
	{
		if (function == 0xc && mode == 0)
			input = d->port_a_in;
		else if (function == 0xc && mode == 1)
			input = d->port_b_in;
		else
			input = operand(xp, s);
		product = multiply(input, s->coefficient);
	}

	alu(xp, function, mode, s->raw);

	int next = pc + 1;
	if (special(s, XP_SPECIAL_BRANCH))
	{
		bool taken;
		switch ((s->cram >> 10) & 0xf)
		{
		case 0: taken = d->acc == 0; break;
		case 1: taken = d->acc != 0; break;
		case 3: case 5: case 13: case 14: taken = true; break;
		case 6: case 8: taken = d->acc >= 0; break;
		case 7: case 9: taken = d->acc < 0; break;
		case 10: taken = d->acc > 0; break;
		case 11: taken = d->acc <= 0; break;
		default: taken = false; break;
		}
		if (taken)
			next = (bit(s->cram, 9) ? pc + 1 + (int8_t)s->cram : (s->cram & 0xff)) % XP_DSP_SLOTS;
	}

	if (multiply_issued)
	{
		d->input = input;
		d->product = product;
	}
	return next;
}

static void interpret_frame(xp_t *xp)
{
	xp_dsp_state_t *d = &xp->dsp;
	int32_t landing[2] = { 0, 0 };
	int landing_valid = 0;
	int pc = 0, strobe_a = 0, strobe_bcd = 0, position = 0;

	d->product = 0;
	for (int cycle = 0; cycle < slot_count(xp); cycle++)
	{
		const xp_slot_t *s = &xp->slots[pc];

		if (landing_valid & 1)
			d->latch = landing[0];
		landing[0] = landing[1];
		landing_valid >>= 1;

		if (s->eram_op == 1)
		{
			landing[1] = xp->eram[(s->eram_offset + d->cursor) & 0xffff];
			landing_valid |= 2;
		}
		else if (special(s, XP_SPECIAL_INDEXED_READ) && !s->eram_second)
		{
			landing[1] = xp->eram[(d->cursor + (d->acc >> 12)) & 0xffff];
			landing_valid |= 2;
		}

		d->now_valid = 0;
		int32_t gain_pending = 0;
		bool gain_arrives = false;
		if (s->st == 1)
		{
			const int32_t v = xp->iram[cell(xp, s->word)];
			if (s->word >= ramp_base(xp))
			{
				gain_pending = gain_current(v);
				gain_arrives = true;
			}
			else
			{
				d->now = v;
				d->now_valid = 1;
			}
		}

		const int32_t acc_before = d->acc;
		const int32_t r_before = d->r;
		const int next = execute(xp, s, pc);

		if (gain_arrives)
			d->gain = gain_pending;
		if (d->now_valid)
			d->r = d->now;

		if (s->st == 2)
			xp->iram[cell(xp, s->word)] = d->latch;
		else if (s->st == 3)
			xp->iram[cell(xp, s->word)] = clamp24(acc_before);

		if (s->eram_op == 3)
			xp->eram[(s->eram_offset + d->cursor) & 0xffff] = clamp24(acc_before);
		else if (s->eram_op == 2)
			xp->eram[(s->eram_offset + d->cursor) & 0xffff] = clamp24(r_before);

		if (s->ext == 1)
		{
			xp->port_a_out[strobe_a & (XP_STROBES - 1)] = clamp24(xp->iram[cell_of(position, xp->parity ^ 1)]);
			d->port_a_in = take_port_a(xp, strobe_a);
			strobe_a++;
			position++;
		}
		else if (s->ext == 2)
		{
			emit_port(xp, position, strobe_bcd);
			if (strobe_bcd % 3 == 0)
				d->port_b_in = take_port_b(xp, strobe_bcd / 3);
			strobe_bcd++;
			position++;
		}

		pc = next % XP_DSP_SLOTS;
	}
}

/* --- the straight-line frame: what the compiled pass cannot do at slot time, done at the frame start */

static void frame_ports(xp_t *xp)
{
	const int n = slot_count(xp);
	for (int i = 0; i < n; i++)
	{
		const xp_sched_t *o = &xp->sched[i];
		if (o->strobe_a != 0xff)
		{
			xp->port_a_out[o->strobe_a] = clamp24(xp->iram[cell_of(o->position, xp->parity ^ 1)]);
			xp->dsp.port_a_return[o->strobe_a] = take_port_a(xp, o->strobe_a);
		}
	}
	for (int i = 0; i < n; i++)
	{
		const xp_sched_t *o = &xp->sched[i];
		if (o->strobe_bcd != 0xff && o->strobe_bcd % 3 == 0)
			xp->dsp.port_b_pair[(o->strobe_bcd / 3) & 1] = take_port_b(xp, o->strobe_bcd / 3);
	}
	for (int i = 0; i < n; i++)
	{
		const xp_sched_t *o = &xp->sched[i];
		if (o->strobe_bcd != 0xff)
			emit_port(xp, o->position, o->strobe_bcd);
	}
}

#if (defined SLJIT_64BIT_ARCHITECTURE && SLJIT_64BIT_ARCHITECTURE)

#define XP_REG_ACC SLJIT_S1
#define XP_REG_PPREV SLJIT_S2
#define XP_REG_ABEF SLJIT_S3
#define XP_REG_ERAM SLJIT_S5

#define CELL(field) SLJIT_MEM1(SLJIT_S0), (sljit_sw)offsetof(xp_t, field)
#define IRAM_CELL(c) SLJIT_MEM1(SLJIT_S0), (sljit_sw)(offsetof(xp_t, iram) + (size_t)(c) * 4)

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

/* reg = wrap29(reg) */
static void e_wrap29(jit_builder_t *b, sljit_s32 reg)
{
	sljit_emit_op2(b->c, SLJIT_SHL, reg, 0, reg, 0, SLJIT_IMM, 35);
	sljit_emit_op2(b->c, SLJIT_ASHR, reg, 0, reg, 0, SLJIT_IMM, 35);
}

static void e_wrap24(jit_builder_t *b, sljit_s32 reg)
{
	sljit_emit_op2(b->c, SLJIT_SHL, reg, 0, reg, 0, SLJIT_IMM, 40);
	sljit_emit_op2(b->c, SLJIT_ASHR, reg, 0, reg, 0, SLJIT_IMM, 40);
}

/* reg = clamp(reg, lo, hi) */
static void e_clamp(jit_builder_t *b, sljit_s32 reg, sljit_sw lo, sljit_sw hi)
{
	struct sljit_jump *below = sljit_emit_cmp(b->c, SLJIT_SIG_LESS_EQUAL, reg, 0, SLJIT_IMM, hi);
	e_mov(b, reg, SLJIT_IMM, hi);
	sljit_set_label(below, sljit_emit_label(b->c));
	struct sljit_jump *above = sljit_emit_cmp(b->c, SLJIT_SIG_GREATER_EQUAL, reg, 0, SLJIT_IMM, lo);
	e_mov(b, reg, SLJIT_IMM, lo);
	sljit_set_label(above, sljit_emit_label(b->c));
}

static void e_clamp24(jit_builder_t *b, sljit_s32 reg) { e_clamp(b, reg, -0x800000, 0x7fffff); }
static void e_clamp29(jit_builder_t *b, sljit_s32 reg) { e_clamp(b, reg, -0x10000000, 0x0fffffff); }

/* dst = dst / 2^bits towards zero */
static void e_divide(jit_builder_t *b, sljit_s32 dst, int bits)
{
	struct sljit_jump *positive = sljit_emit_cmp(b->c, SLJIT_SIG_GREATER_EQUAL, dst, 0, SLJIT_IMM, 0);
	sljit_emit_op2(b->c, SLJIT_ADD, dst, 0, dst, 0, SLJIT_IMM, (1 << bits) - 1);
	sljit_set_label(positive, sljit_emit_label(b->c));
	sljit_emit_op2(b->c, SLJIT_ASHR, dst, 0, dst, 0, SLJIT_IMM, bits);
}

static void e_add(jit_builder_t *b, sljit_s32 dst, sljit_s32 a, sljit_s32 bb, sljit_sw bw)
{
	sljit_emit_op2(b->c, SLJIT_ADD, dst, 0, a, 0, bb, bw);
	e_wrap29(b, dst);
}

static void e_sub(jit_builder_t *b, sljit_s32 dst, sljit_s32 a, sljit_sw aw, sljit_s32 bb, sljit_sw bw)
{
	sljit_emit_op2(b->c, SLJIT_SUB, dst, 0, a, aw, bb, bw);
	e_wrap29(b, dst);
}

/* dst = clamp29(a * c / 8192), towards zero */
static void e_mul13(jit_builder_t *b, sljit_s32 dst, sljit_s32 a, sljit_s32 c, sljit_sw cw)
{
	sljit_emit_op2(b->c, SLJIT_MUL, dst, 0, a, 0, c, cw);
	e_divide(b, dst, 13);
	e_clamp29(b, dst);
}

/* dst = clamp29((a * f) << shift / 32768), towards zero */
static void e_mulq15(jit_builder_t *b, sljit_s32 dst, sljit_s32 a, sljit_s32 f, int shift)
{
	sljit_emit_op2(b->c, SLJIT_MUL, dst, 0, a, 0, f, 0);
	if (shift)
		sljit_emit_op2(b->c, SLJIT_SHL, dst, 0, dst, 0, SLJIT_IMM, shift);
	e_divide(b, dst, 15);
	e_clamp29(b, dst);
}

/* R3 = ((offset + cursor) & 0xffff) * 4, an ERAM byte offset */
static void e_eram_index(jit_builder_t *b, uint16_t offset)
{
	sljit_emit_op1(b->c, SLJIT_MOV_U16, SLJIT_R3, 0, CELL(dsp.cursor));
	sljit_emit_op2(b->c, SLJIT_ADD, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, offset);
	sljit_emit_op2(b->c, SLJIT_AND, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 0xffff);
	sljit_emit_op2(b->c, SLJIT_SHL, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 2);
}

/* R0 = the multiply input named by mode: the previous multiply's input, the accumulator saturated,
   the IRAM read latch, or the ERAM read latch */
static void e_operand(jit_builder_t *b, int mode)
{
	switch (mode)
	{
	case 0:
		e_load(b, SLJIT_R0, CELL(dsp.input));
		return;
	case 1:
		e_mov(b, SLJIT_R0, XP_REG_ACC, 0);
		e_clamp24(b, SLJIT_R0);
		return;
	case 2:
		e_load(b, SLJIT_R0, CELL(dsp.r));
		return;
	default:
		e_load(b, SLJIT_R0, CELL(dsp.latch));
		return;
	}
}

/* dst = min or max of dst and (src, srcw) */
static void e_minmax(jit_builder_t *b, sljit_s32 dst, sljit_s32 src, sljit_sw srcw, bool max)
{
	struct sljit_jump *keep = sljit_emit_cmp(b->c, max ? SLJIT_SIG_GREATER_EQUAL : SLJIT_SIG_LESS_EQUAL, dst, 0, src, srcw);
	e_mov(b, dst, src, srcw);
	sljit_set_label(keep, sljit_emit_label(b->c));
}

/* acc = |acc| */
static void e_abs(jit_builder_t *b)
{
	sljit_emit_op2(b->c, SLJIT_SUB, SLJIT_R3, 0, SLJIT_IMM, 0, XP_REG_ACC, 0);
	struct sljit_jump *positive = sljit_emit_cmp(b->c, SLJIT_SIG_GREATER_EQUAL, XP_REG_ACC, 0, SLJIT_IMM, 0);
	e_mov(b, XP_REG_ACC, SLJIT_R3, 0);
	sljit_set_label(positive, sljit_emit_label(b->c));
}

/* acc = fold24(acc): reflect the halves beyond half scale */
static void e_fold(jit_builder_t *b)
{
	sljit_emit_op2(b->c, SLJIT_AND, SLJIT_R3, 0, XP_REG_ACC, 0, SLJIT_IMM, 0xffffff);
	sljit_emit_op2(b->c, SLJIT_LSHR, SLJIT_R4, 0, SLJIT_R3, 0, SLJIT_IMM, 22);
	sljit_emit_op2(b->c, SLJIT_LSHR, SLJIT_R0, 0, SLJIT_R3, 0, SLJIT_IMM, 23);
	sljit_emit_op2(b->c, SLJIT_XOR, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_R0, 0);
	sljit_emit_op2(b->c, SLJIT_AND, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_IMM, 1);
	struct sljit_jump *same = sljit_emit_cmp(b->c, SLJIT_EQUAL, SLJIT_R4, 0, SLJIT_IMM, 0);
	sljit_emit_op2(b->c, SLJIT_XOR, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 0x7fffff);
	sljit_set_label(same, sljit_emit_label(b->c));
	e_mov(b, XP_REG_ACC, SLJIT_R3, 0);
	e_wrap24(b, XP_REG_ACC);
}

/* R4 = the parallel op's factor as Q15 */
static void e_factor(jit_builder_t *b, int factor_select, bool complement)
{
	if (factor_select == 3)
	{
		e_load(b, SLJIT_R4, CELL(dsp.gain));
		if (complement)
			sljit_emit_op2(b->c, SLJIT_XOR, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_IMM, -1);
		return;
	}
	e_mov(b, SLJIT_R4, XP_REG_ACC, 0);
	e_clamp24(b, SLJIT_R4);
	switch (factor_select)
	{
	case 0:
		sljit_emit_op2(b->c, SLJIT_AND, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_IMM, 0xfff);
		sljit_emit_op2(b->c, SLJIT_SHL, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_IMM, 3);
		if (complement)
			sljit_emit_op2(b->c, SLJIT_SUB, SLJIT_R4, 0, SLJIT_IMM, 0x7fff, SLJIT_R4, 0);
		break;
	case 1:
		sljit_emit_op2(b->c, SLJIT_AND, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_IMM, 0x7fffff);
		sljit_emit_op2(b->c, SLJIT_LSHR, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_IMM, 8);
		if (complement)
			sljit_emit_op2(b->c, SLJIT_SUB, SLJIT_R4, 0, SLJIT_IMM, 0x7fff, SLJIT_R4, 0);
		break;
	default:
		sljit_emit_op2(b->c, SLJIT_ASHR, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_IMM, 8);
		if (complement)
			sljit_emit_op2(b->c, SLJIT_XOR, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_IMM, -1);
		break;
	}
}

/* the ALU functions both encodings share: p is XP_REG_PPREV, r the read latch */
static void e_alu(jit_builder_t *b, int fn, int mode, int32_t raw)
{
	switch (fn)
	{
	case 0x2:
		e_load(b, SLJIT_R3, CELL(dsp.r));
		e_add(b, XP_REG_ACC, XP_REG_ACC, SLJIT_R3, 0);
		break;
	case 0x3:
		e_add(b, XP_REG_ACC, XP_REG_ACC, XP_REG_PPREV, 0);
		break;
	case 0x4:
		e_load(b, XP_REG_ACC, CELL(dsp.r));
		break;
	case 0x5:
		e_mov(b, XP_REG_ACC, XP_REG_PPREV, 0);
		break;
	case 0x6:
		e_sub(b, XP_REG_ACC, SLJIT_IMM, 0, XP_REG_ACC, 0);
		break;
	case 0x7:
		e_load(b, SLJIT_R3, CELL(dsp.r));
		e_sub(b, XP_REG_ACC, SLJIT_R3, 0, XP_REG_ACC, 0);
		break;
	case 0x8:
		e_sub(b, XP_REG_ACC, XP_REG_PPREV, 0, XP_REG_ACC, 0);
		break;
	case 0x9:
		e_load(b, SLJIT_R3, CELL(dsp.r));
		e_add(b, XP_REG_ACC, SLJIT_R3, XP_REG_PPREV, 0);
		break;
	case 0xa:
	case 0xb:
		e_load(b, SLJIT_R3, CELL(dsp.r));
		e_minmax(b, XP_REG_ACC, SLJIT_R3, 0, fn == 0xb);
		break;
	case 0xc:
		if (mode == 2)
		{
			sljit_emit_op2(b->c, SLJIT_ASHR, SLJIT_R3, 0, XP_REG_PPREV, 0, SLJIT_IMM, 13);
			e_add(b, XP_REG_ACC, XP_REG_ACC, SLJIT_R3, 0);
		}
		else if (mode == 3)
			sljit_emit_op2(b->c, SLJIT_ASHR, XP_REG_ACC, 0, XP_REG_PPREV, 0, SLJIT_IMM, 13);
		break;
	case 0xd:
		if (mode < 3)
		{
			sljit_emit_op2(b->c, mode == 0 ? SLJIT_AND : mode == 1 ? SLJIT_OR : SLJIT_XOR, XP_REG_ACC, 0, XP_REG_ACC, 0, SLJIT_IMM, raw);
			e_wrap29(b, XP_REG_ACC);
		}
		break;
	case 0xe:
		if (mode < 2)
			e_minmax(b, XP_REG_ACC, SLJIT_IMM, raw, mode == 1);
		break;
	case 0xf:
		switch (mode)
		{
		case 0:
			e_add(b, XP_REG_ACC, XP_REG_ACC, SLJIT_IMM, raw);
			break;
		case 1:
			e_load(b, SLJIT_R3, CELL(dsp.r));
			e_add(b, XP_REG_ACC, SLJIT_R3, SLJIT_IMM, raw);
			break;
		case 2:
			e_add(b, XP_REG_ACC, XP_REG_PPREV, SLJIT_IMM, raw);
			break;
		default:
			e_sub(b, XP_REG_ACC, SLJIT_IMM, raw, XP_REG_ACC, 0);
			break;
		}
		break;
	default:
		break;
	}
}

/* col 0x30: the CRAM word is a second instruction; the product is left in R2, the input in R0 */
static void e_parallel(jit_builder_t *b, const xp_slot_t *s, const xp_sched_t *o)
{
	const uint16_t c = s->cram;
	const int fn = c & 0xf;
	const int input = (c >> 4) & 3;
	const int factor_select = (c >> 6) & 3;
	const bool complement = bit(c, 8);
	const bool multiply = bit(c, 9);
	const int post = (c >> 11) & 7;
	const int shift = shift_select[c >> 14];

	if (!multiply)
	{
		if (o->now_valid)
			e_mov(b, SLJIT_R0, SLJIT_R1, 0);
		else
			e_mov(b, SLJIT_R0, SLJIT_IMM, 0);
		e_mov(b, SLJIT_R2, SLJIT_R0, 0);
	}
	else
	{
		e_operand(b, input);
		e_factor(b, factor_select, complement);
		e_mulq15(b, SLJIT_R2, SLJIT_R0, SLJIT_R4, shift);
	}
	e_store(b, CELL(dsp.input), SLJIT_R0);

	switch (fn)
	{
	case 0x0:
		e_load(b, SLJIT_R3, CELL(dsp.r));
		e_add(b, XP_REG_ACC, XP_REG_ACC, XP_REG_PPREV, 0);
		e_add(b, XP_REG_ACC, XP_REG_ACC, SLJIT_R3, 0);
		break;
	case 0x1:
		break;
	case 0xc:
		e_load(b, SLJIT_R3, CELL(dsp.r));
		sljit_emit_op2(b->c, SLJIT_ADD, SLJIT_R3, 0, SLJIT_R3, 0, XP_REG_PPREV, 0);
		e_sub(b, XP_REG_ACC, SLJIT_R3, 0, XP_REG_ACC, 0);
		break;
	case 0xd:
		e_load(b, SLJIT_R3, CELL(dsp.r));
		sljit_emit_op2(b->c, SLJIT_ADD, XP_REG_ACC, 0, XP_REG_ACC, 0, XP_REG_PPREV, 0);
		e_sub(b, XP_REG_ACC, XP_REG_ACC, 0, SLJIT_R3, 0);
		break;
	case 0xe:
		e_load(b, SLJIT_R3, CELL(dsp.r));
		sljit_emit_op2(b->c, SLJIT_ADD, SLJIT_R3, 0, SLJIT_R3, 0, XP_REG_ACC, 0);
		e_sub(b, XP_REG_ACC, XP_REG_PPREV, 0, SLJIT_R3, 0);
		break;
	case 0xf:
		e_load(b, SLJIT_R3, CELL(dsp.r));
		e_sub(b, XP_REG_ACC, XP_REG_PPREV, 0, SLJIT_R3, 0);
		break;
	default:
		e_alu(b, fn, input, s->raw);
		break;
	}

	switch (post)
	{
	case 1:
		e_abs(b);
		break;
	case 2:
		e_wrap24(b, XP_REG_ACC);
		break;
	case 3:
		sljit_emit_op2(b->c, SLJIT_AND, SLJIT_R3, 0, XP_REG_ACC, 0, SLJIT_IMM, 0xffffff);
		sljit_emit_op2(b->c, SLJIT_LSHR, SLJIT_R4, 0, SLJIT_R3, 0, SLJIT_IMM, 23);
		sljit_emit_op2(b->c, SLJIT_LSHR, SLJIT_R0, 0, SLJIT_R3, 0, SLJIT_IMM, 6);
		sljit_emit_op2(b->c, SLJIT_XOR, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_R0, 0);
		sljit_emit_op2(b->c, SLJIT_LSHR, SLJIT_R0, 0, SLJIT_R3, 0, SLJIT_IMM, 1);
		sljit_emit_op2(b->c, SLJIT_XOR, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_R0, 0);
		sljit_emit_op2(b->c, SLJIT_AND, SLJIT_R4, 0, SLJIT_R4, 0, SLJIT_IMM, 1);
		sljit_emit_op2(b->c, SLJIT_SHL, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 1);
		sljit_emit_op2(b->c, SLJIT_OR, XP_REG_ACC, 0, SLJIT_R3, 0, SLJIT_R4, 0);
		e_wrap24(b, XP_REG_ACC);
		break;
	case 4:
		e_fold(b);
		break;
	case 5:
		e_wrap24(b, XP_REG_ACC);
		{
			struct sljit_jump *positive = sljit_emit_cmp(b->c, SLJIT_SIG_GREATER_EQUAL, XP_REG_ACC, 0, SLJIT_IMM, 0);
			sljit_emit_op2(b->c, SLJIT_XOR, XP_REG_ACC, 0, XP_REG_ACC, 0, SLJIT_IMM, -1);
			sljit_set_label(positive, sljit_emit_label(b->c));
		}
		break;
	default:
		break;
	}
	if (bit(c, 10))
		e_wrap24(b, XP_REG_ACC);
}

/* the grid: the multiply input by col[5:4] and the function by col[3:0]; a slot's product is left in R2
   and its input in R0.  Returns 0 when the slot issued no multiply and the latches stand, 1 when both
   commit, 2 when the parallel op has already committed its input. */
static int e_execute(jit_builder_t *b, const xp_slot_t *s, const xp_sched_t *o)
{
	if (special(s, XP_SPECIAL_PARALLEL))
	{
		e_parallel(b, s, o);
		return 2;
	}

	const int mode = s->input;
	const int fn = s->function;
	const bool issued = fn >= 1 && fn <= 0xc;

	if (issued)
	{
		if (fn == 0xc && mode == 0)
			e_load(b, SLJIT_R0, CELL(dsp.port_a_in));
		else if (fn == 0xc && mode == 1)
			e_load(b, SLJIT_R0, CELL(dsp.port_b_in));
		else
			e_operand(b, mode);
		e_mul13(b, SLJIT_R2, SLJIT_R0, SLJIT_IMM, s->coefficient);
	}
	e_alu(b, fn, mode, s->raw);
	return issued ? 1 : 0;
}

static void e_slot(jit_builder_t *b, int i, const xp_slot_t *s, const xp_sched_t *o, int parity)
{
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
	else if (special(s, XP_SPECIAL_INDEXED_READ) && !s->eram_second)
	{
		sljit_emit_op1(b->c, SLJIT_MOV_U16, SLJIT_R3, 0, CELL(dsp.cursor));
		sljit_emit_op2(b->c, SLJIT_ASHR, SLJIT_R4, 0, XP_REG_ACC, 0, SLJIT_IMM, 12);
		sljit_emit_op2(b->c, SLJIT_ADD, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_R4, 0);
		sljit_emit_op2(b->c, SLJIT_AND, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 0xffff);
		sljit_emit_op2(b->c, SLJIT_SHL, SLJIT_R3, 0, SLJIT_R3, 0, SLJIT_IMM, 2);
		e_load(b, SLJIT_R3, SLJIT_MEM2(XP_REG_ERAM, SLJIT_R3), 0);
		e_store(b, CELL(dsp.pend[i & 1]), SLJIT_R3);
	}
	if (s->st == 1)
		e_load(b, SLJIT_R1, IRAM_CELL(cell_of(s->word, parity)));

	const int issued = e_execute(b, s, o);

	if (o->gain_load)
	{
		sljit_emit_op2(b->c, SLJIT_ASHR, SLJIT_R3, 0, SLJIT_R1, 0, SLJIT_IMM, 10);
		sljit_emit_op1(b->c, SLJIT_MOV_S16, SLJIT_R3, 0, SLJIT_R3, 0);
		e_store(b, CELL(dsp.gain), SLJIT_R3);
	}
	if (s->st == 2)
	{
		e_load(b, SLJIT_R3, CELL(dsp.latch));
		e_store(b, IRAM_CELL(cell_of(s->word, parity)), SLJIT_R3);
	}
	else if (s->st == 3)
	{
		e_mov(b, SLJIT_R3, XP_REG_ABEF, 0);
		e_clamp24(b, SLJIT_R3);
		e_store(b, IRAM_CELL(cell_of(s->word, parity)), SLJIT_R3);
	}
	if (s->eram_op == 3)
	{
		e_mov(b, SLJIT_R4, XP_REG_ABEF, 0);
		e_clamp24(b, SLJIT_R4);
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
		e_store(b, CELL(dsp.r), SLJIT_R1);
	if (o->strobe_a != 0xff)
	{
		e_load(b, SLJIT_R3, CELL(dsp.port_a_return[o->strobe_a]));
		e_store(b, CELL(dsp.port_a_in), SLJIT_R3);
	}
	if (o->strobe_bcd != 0xff && o->strobe_bcd % 3 == 0)
	{
		e_load(b, SLJIT_R3, CELL(dsp.port_b_pair[(o->strobe_bcd / 3) & 1]));
		e_store(b, CELL(dsp.port_b_in), SLJIT_R3);
	}
	if (issued == 1)
		e_store(b, CELL(dsp.input), SLJIT_R0);
	if (issued)
		e_mov(b, XP_REG_PPREV, SLJIT_R2, 0);
}

static bool compile_parity(xp_t *xp, int parity)
{
	jit_builder_t b;
	jit_code_t *code = &xp->code[parity];
	jit_code_free(xp->jit, code);
	xp->frame[parity] = NULL;
	if (!jit_begin(&b, xp->jit, 5, 6))
		return false;

	e_load(&b, XP_REG_ACC, CELL(dsp.acc));
	e_mov(&b, XP_REG_PPREV, SLJIT_IMM, 0);
	e_mov(&b, XP_REG_ABEF, XP_REG_ACC, 0);
	sljit_emit_op1(b.c, SLJIT_MOV_P, XP_REG_ERAM, 0, CELL(eram));
	for (int i = 0; i < slot_count(xp); i++)
		e_slot(&b, i, &xp->slots[i], &xp->sched[i], parity);
	e_store(&b, CELL(dsp.acc), XP_REG_ACC);

	if (!jit_end(&b, xp->jit, code))
		return false;
	xp->frame[parity] = (xp_frame_fn)code->entry;
	return true;
}

static bool compile_program(xp_t *xp)
{
	xp_schedule(xp);
	if (xp->branching)
	{
		jit_code_free(xp->jit, &xp->code[0]);
		jit_code_free(xp->jit, &xp->code[1]);
		xp->frame[0] = xp->frame[1] = NULL;
		return false;
	}
	return compile_parity(xp, 0) && compile_parity(xp, 1);
}

#else

static bool compile_program(xp_t *xp)
{
	xp_schedule(xp);
	xp->frame[0] = xp->frame[1] = NULL;
	return false;
}

#endif

static void run_dsp(xp_t *xp)
{
	xp->dsp_enabled = bit(xp->regs[XP_DSP_MODE >> 1], 2);
	if (!xp->dsp_enabled)
	{
		memset(xp->port_word, 0, sizeof(xp->port_word));
		return;
	}
	if (xp->program_dirty)
	{
		xp_decode_program(xp);
		xp->program_dirty = false;
		compile_program(xp);
	}
	update_iram_ramps(xp);

	xp_frame_fn frame = xp->interpret ? NULL : xp->frame[xp->parity];
	if (frame)
	{
		frame_ports(xp);
		xp->dsp.product = 0;
		frame(xp);
	}
	else
		interpret_frame(xp);
	xp->dsp.cursor--;
}

void xp_run_frame(xp_t *xp)
{
	xp->bus_written = 0;
	xp->irq_frame_used = false;
	for (int n = 0; n < voice_count(xp); n++)
	{
		const int32_t output = running(xp, n) ? wrap24((int32_t)page(xp, n, XP_PAGE_OUTPUT)) : 0;
		for (int bank = 0; bank < 4; bank++)
			deposit(xp, n, bank, output);
		if (running(xp, n))
			run_voice_watched(xp, n);
		else
			run_voice(xp, n);
	}

	run_dsp(xp);
	xp->parity ^= 1;
	xp->frame_counter++;
}

int32_t xp_output(const xp_t *xp, int channel)
{
	return xp->port_word[(channel >> 1) % XP_OUTPUT_PORTS][channel & 1];
}

int32_t xp_port_a_out(const xp_t *xp, int strobe)
{
	return xp->port_a_out[strobe & (XP_STROBES - 1)];
}

int32_t xp_iram(const xp_t *xp, int word)
{
	return xp->iram[cell_of(word & 0xff, xp->parity ^ 1)];
}

int32_t xp_bus_word(const xp_t *xp, int n)
{
	return xp->iram[cell_of(0x40 + (n & 63), xp->parity)];
}

void xp_set_bus_word(xp_t *xp, int n, int32_t value)
{
	xp->iram[cell_of(0x40 + (n & 63), xp->parity)] = value;
}
