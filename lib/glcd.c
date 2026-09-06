#include <string.h>
#include "glcd.h"

#define GLCD_LINES 64
#define GLCD_STRIDE 27
#define GLCD_DOTS_PER_BYTE 6
#define GLCD_DOTS (GLCD_STRIDE * GLCD_DOTS_PER_BYTE)
#define GLCD_ROW (GLCD_DOTS + 8)

#define GLCD_VRAM_MASK 0x7fff

#define GLCD_CLOCK_KHZ 10000
#define GLCD_FRAME_KHZ 32

enum
{
	CMD_SYSTEM_SET = 0x40,
	CMD_MWRITE = 0x42,
	CMD_MREAD = 0x43,
	CMD_SCROLL = 0x44,
	CMD_CSRW = 0x46,
	CMD_CSRR = 0x47,
	CMD_CSRDIR_RIGHT = 0x4c,
	CMD_CSRDIR_LEFT = 0x4d,
	CMD_CSRDIR_UP = 0x4e,
	CMD_CSRDIR_DOWN = 0x4f,
	CMD_SLEEP_IN = 0x53,
	CMD_DISP_OFF = 0x58,
	CMD_DISP_ON = 0x59,
	CMD_HDOT_SCR = 0x5a,
	CMD_OVLAY = 0x5b,
	CMD_CGRAM_ADR = 0x5c,
	CMD_CSRFORM = 0x5d
};

static void frame_period(glcd_t *g)
{
	uint32_t p = (uint32_t)9 * g->tcr * g->lf * GLCD_FRAME_KHZ / GLCD_CLOCK_KHZ;
	g->flash_period = p ? p : 1;
	g->flash_count = 0;
}

static bool flash_wanted(const glcd_t *g)
{
	if (g->fc >= 2)
		return true;
	return (g->fp & 0x02) != 0 || (g->fp & 0x08) != 0 || (g->fp & 0x20) != 0;
}

static bool cursor_visible(const glcd_t *g)
{
	switch (g->fc)
	{
	case 1:
		return true;
	case 2:
		return ((g->flash_phase >> 4) & 1) == 0;
	case 3:
		return ((g->flash_phase >> 5) & 1) == 0;
	default:
		return false;
	}
}

static bool page_visible(const glcd_t *g, unsigned attr)
{
	switch (attr)
	{
	case 1:
		return true;
	case 2:
		return ((g->flash_phase >> 4) & 1) == 0;
	case 3:
		return ((g->flash_phase >> 1) & 1) == 0;
	default:
		return false;
	}
}

static uint8_t glyph_row(const glcd_t *g, uint8_t code, unsigned row)
{
	unsigned height = g->m2 ? 16u : 8u;
	unsigned index;

	if (g->m0 || row >= height)
		return 0;
	if (code < 0x20)
		index = code;
	else if (code >= 0x80 && code < 0xa0)
		index = 0x20 + (unsigned)(code - 0x80);
	else
		return 0;
	return g->vram[(g->sag + index * height + row) & GLCD_VRAM_MASK];
}

static bool cursor_row(const glcd_t *g, unsigned row)
{
	return g->cm ? row <= g->cry : row == g->cry;
}

static void row_text(const glcd_t *g, uint8_t *dots, uint16_t base, unsigned row, bool cursor)
{
	for (unsigned cell = 0; cell < g->cr; cell++)
	{
		unsigned x0 = cell * g->fx;
		uint8_t bits;

		if (x0 >= GLCD_ROW)
			break;
		bits = glyph_row(g, g->vram[(base + cell) & GLCD_VRAM_MASK], row);
		for (unsigned x = 0; x < g->fx && x0 + x < GLCD_ROW; x++)
			if (bits & (0x80u >> x))
				dots[x0 + x] = 1;
		if (cursor && (uint16_t)(base + cell) == g->csr && cursor_row(g, row))
			for (unsigned x = 0; x < g->crx && x0 + x < GLCD_ROW; x++)
				dots[x0 + x] = 1;
	}
}

static void row_graphics(const glcd_t *g, uint8_t *dots, uint16_t base)
{
	for (unsigned cell = 0; cell < g->cr; cell++)
	{
		unsigned x0 = cell * g->fx;
		uint8_t bits;

		if (x0 >= GLCD_ROW)
			break;
		bits = g->vram[(base + cell) & GLCD_VRAM_MASK];
		for (unsigned x = 0; x < g->fx && x0 + x < GLCD_ROW; x++)
			if (bits & (0x80u >> x))
				dots[x0 + x] = 1;
	}
}

static void row_page(const glcd_t *g, uint8_t *dots, uint16_t sad, unsigned line, bool graphics, bool cursor)
{
	unsigned fy = g->fy ? g->fy : 1u;

	if (graphics)
		row_graphics(g, dots, (uint16_t)(sad + line * g->ap));
	else
		row_text(g, dots, (uint16_t)(sad + (line / fy) * g->ap), line % fy, cursor);
}

static void merge(uint8_t *dst, const uint8_t *src, unsigned mx)
{
	for (unsigned i = 0; i < GLCD_ROW; i++)
		switch (mx)
		{
		case 1:
			dst[i] ^= src[i];
			break;
		case 2:
			dst[i] &= src[i];
			break;
		default:
			dst[i] |= src[i];
			break;
		}
}

static void compose(glcd_t *g)
{
	uint8_t bitmap[GLCD_LINES * GLCD_STRIDE];
	unsigned lines = g->lf < GLCD_LINES ? g->lf : GLCD_LINES;
	unsigned attr1 = g->fp & 3, attr2 = (g->fp >> 2) & 3, attr3 = (g->fp >> 4) & 3;
	bool on = g->display && !g->sleep;
	bool changed = on != g->out.display_on;

	g->dirty = false;
	g->out.display_on = on;
	if (!on)
	{
		if (changed)
			g->out.changed = true;
		return;
	}

	memset(bitmap, 0, sizeof bitmap);
	for (unsigned y = 0; y < lines; y++)
	{
		uint8_t row[GLCD_ROW], layer[GLCD_ROW];
		bool have = false;

		memset(row, 0, sizeof row);
		if (g->ov)
		{
			if (page_visible(g, attr1))
			{
				row_page(g, row, g->sad1, y, g->dm1, cursor_visible(g));
				have = true;
			}
			if (page_visible(g, attr3))
			{
				memset(layer, 0, sizeof layer);
				row_page(g, layer, g->sad3, y, g->dm3, cursor_visible(g));
				if (have)
					merge(row, layer, g->mx);
				else
					memcpy(row, layer, sizeof row);
				have = true;
			}
		}
		else if (y < g->sl1)
		{
			if (page_visible(g, attr1))
			{
				row_page(g, row, g->sad1, y, g->dm1, cursor_visible(g));
				have = true;
			}
		}
		else
		{
			if (page_visible(g, attr3))
			{
				row_page(g, row, g->sad3, y - g->sl1, g->dm3, cursor_visible(g));
				have = true;
			}
		}

		if (page_visible(g, attr2))
		{
			memset(layer, 0, sizeof layer);
			if (g->ws && y >= g->sl2)
				row_graphics(g, layer, (uint16_t)(g->sad4 + (y - g->sl2) * g->ap));
			else
				row_graphics(g, layer, (uint16_t)(g->sad2 + y * g->ap));
			if (have)
				merge(row, layer, g->mx);
			else
				memcpy(row, layer, sizeof row);
		}

		for (unsigned col = 0; col < GLCD_DOTS; col++)
			if (row[col + g->hdotscr])
				bitmap[y * GLCD_STRIDE + col / GLCD_DOTS_PER_BYTE] |= 0x80u >> (col % GLCD_DOTS_PER_BYTE);
	}

	if (memcmp(bitmap, g->out.bitmap, sizeof bitmap) != 0)
	{
		memcpy(g->out.bitmap, bitmap, sizeof bitmap);
		changed = true;
	}
	if (changed)
		g->out.changed = true;
}

static void csr_step(glcd_t *g)
{
	switch (g->csrdir)
	{
	case 1:
		g->csr--;
		break;
	case 2:
		g->csr -= g->ap;
		break;
	case 3:
		g->csr += g->ap;
		break;
	default:
		g->csr++;
		break;
	}
}

void glcd_init(glcd_t *g)
{
	memset(g, 0, sizeof *g);
	glcd_reset(g);
}

void glcd_reset(glcd_t *g)
{
	memset(&g->out, 0, sizeof g->out);
	g->command = 0;
	g->param = 0;
	g->display = false;
	g->sleep = false;
	g->m0 = g->m1 = g->m2 = g->ws = g->iv = g->wf = 0;
	g->fx = 1;
	g->fy = 1;
	g->cr = 0;
	g->tcr = 1;
	g->lf = 0;
	g->ap = 0;
	g->sad1 = g->sad2 = g->sad3 = g->sad4 = 0;
	g->sl1 = g->sl2 = 0;
	g->sag = 0;
	g->hdotscr = 0;
	g->mx = g->dm1 = g->dm3 = g->ov = 0;
	g->fc = g->fp = 0;
	g->crx = 1;
	g->cry = 0;
	g->cm = 0;
	g->csr = 0;
	g->csrdir = 0;
	g->flash_phase = 0;
	frame_period(g);
	g->dirty = true;
	g->out.changed = true;
}

uint8_t glcd_read(glcd_t *g, uint32_t offset)
{
	uint8_t data = 0;

	if (offset & 1)
		return 0;
	switch (g->command)
	{
	case CMD_MREAD:
		data = g->vram[g->csr & GLCD_VRAM_MASK];
		csr_step(g);
		break;

	case CMD_CSRR:
		if (g->param == 0)
			data = g->csr & 0xff;
		else if (g->param == 1)
			data = g->csr >> 8;
		if (g->param < 2)
			g->param++;
		break;

	default:
		break;
	}
	return data;
}

static void command(glcd_t *g, uint8_t data)
{
	g->command = data;
	g->param = 0;

	switch (data)
	{
	case CMD_SYSTEM_SET:
		g->sleep = false;
		break;

	case CMD_CSRDIR_RIGHT:
	case CMD_CSRDIR_LEFT:
	case CMD_CSRDIR_UP:
	case CMD_CSRDIR_DOWN:
		g->csrdir = data & 3;
		break;

	case CMD_SLEEP_IN:
		g->sleep = true;
		break;

	case CMD_DISP_OFF:
		g->display = false;
		break;

	case CMD_DISP_ON:
		g->display = true;
		g->sleep = false;
		break;

	default:
		break;
	}
	g->dirty = true;
}

static void parameter(glcd_t *g, uint8_t data)
{
	switch (g->command)
	{
	case CMD_SYSTEM_SET:
		switch (g->param)
		{
		case 0:
			g->m0 = (data >> 0) & 1;
			g->m1 = (data >> 1) & 1;
			g->m2 = (data >> 2) & 1;
			g->ws = (data >> 3) & 1;
			g->iv = (data >> 5) & 1;
			break;
		case 1:
			g->fx = (data & 0x07) + 1;
			g->wf = (data >> 7) & 1;
			break;
		case 2:
			g->fy = (data & 0x0f) + 1;
			break;
		case 3:
			g->cr = (uint16_t)data + 1;
			break;
		case 4:
			g->tcr = (uint16_t)data + 1;
			frame_period(g);
			break;
		case 5:
			g->lf = (uint16_t)data + 1;
			frame_period(g);
			break;
		case 6:
			g->ap = (g->ap & 0xff00) | data;
			break;
		case 7:
			g->ap = (uint16_t)(data << 8) | (g->ap & 0xff);
			break;
		default:
			break;
		}
		break;

	case CMD_SCROLL:
		switch (g->param)
		{
		case 0:
			g->sad1 = (g->sad1 & 0xff00) | data;
			break;
		case 1:
			g->sad1 = (uint16_t)(data << 8) | (g->sad1 & 0xff);
			break;
		case 2:
			g->sl1 = (uint16_t)data + 1;
			break;
		case 3:
			g->sad2 = (g->sad2 & 0xff00) | data;
			break;
		case 4:
			g->sad2 = (uint16_t)(data << 8) | (g->sad2 & 0xff);
			break;
		case 5:
			g->sl2 = (uint16_t)data + 1;
			break;
		case 6:
			g->sad3 = (g->sad3 & 0xff00) | data;
			break;
		case 7:
			g->sad3 = (uint16_t)(data << 8) | (g->sad3 & 0xff);
			break;
		case 8:
			g->sad4 = (g->sad4 & 0xff00) | data;
			break;
		case 9:
			g->sad4 = (uint16_t)(data << 8) | (g->sad4 & 0xff);
			break;
		default:
			break;
		}
		break;

	case CMD_CSRFORM:
		if (g->param == 0)
		{
			g->crx = (data & 0x0f) + 1;
		}
		else if (g->param == 1)
		{
			g->cry = data & 0x0f;
			g->cm = (data >> 7) & 1;
		}
		break;

	case CMD_CGRAM_ADR:
		if (g->param == 0)
			g->sag = (g->sag & 0xff00) | data;
		else if (g->param == 1)
			g->sag = (uint16_t)(data << 8) | (g->sag & 0xff);
		break;

	case CMD_HDOT_SCR:
		if (g->param == 0)
			g->hdotscr = data & 0x07;
		break;

	case CMD_OVLAY:
		if (g->param == 0)
		{
			g->mx = data & 0x03;
			g->dm1 = (data >> 2) & 1;
			g->dm3 = (data >> 3) & 1;
			g->ov = (data >> 4) & 1;
		}
		break;

	case CMD_DISP_ON:
	case CMD_DISP_OFF:
		if (g->param == 0)
		{
			g->fc = data & 0x03;
			g->fp = data >> 2;
			if (!flash_wanted(g))
			{
				g->flash_phase = 0;
				g->flash_count = 0;
			}
		}
		break;

	case CMD_CSRW:
		if (g->param == 0)
			g->csr = (g->csr & 0xff00) | data;
		else if (g->param == 1)
			g->csr = (uint16_t)(data << 8) | (g->csr & 0xff);
		break;

	case CMD_MWRITE:
		g->vram[g->csr & GLCD_VRAM_MASK] = data;
		csr_step(g);
		break;

	default:
		break;
	}
	if (g->param < 0xff)
		g->param++;
	g->dirty = true;
}

void glcd_write(glcd_t *g, uint32_t offset, uint8_t data)
{
	if (offset & 1)
		command(g, data);
	else
		parameter(g, data);
}

void glcd_frame(glcd_t *g)
{
	if (flash_wanted(g) && ++g->flash_count >= g->flash_period)
	{
		g->flash_count = 0;
		g->flash_phase++;
		g->dirty = true;
	}
	if (g->dirty)
		compose(g);
}

const scemu_glcd_t *glcd_out(glcd_t *g)
{
	if (g->dirty)
		compose(g);
	return &g->out;
}
