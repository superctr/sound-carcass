/* scemu GUI: a reader for the baked artwork's PNG files.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "png.h"

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static uint8_t paeth(int a, int b, int c)
{
	int p = a + b - c;
	int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
	if (pa <= pb && pa <= pc)
		return (uint8_t)a;
	return pb <= pc ? (uint8_t)b : (uint8_t)c;
}

bool png_decode(const uint8_t *data, size_t size, png_image_t *out)
{
	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
	if (size < 8 || memcmp(data, sig, 8) != 0)
		return false;
	uint32_t width = 0, height = 0;
	int channels = 0;
	uint8_t *idat = NULL;
	size_t idat_size = 0;
	size_t pos = 8;
	while (pos + 12 <= size)
	{
		uint32_t length = be32(data + pos);
		const uint8_t *type = data + pos + 4;
		const uint8_t *body = data + pos + 8;
		if (length > size - pos - 12)
			break;
		if (memcmp(type, "IHDR", 4) == 0 && length == 13)
		{
			width = be32(body);
			height = be32(body + 4);
			if (body[8] != 8 || body[12] != 0 || (body[9] != 2 && body[9] != 6))
				break;
			channels = body[9] == 6 ? 4 : 3;
		}
		else if (memcmp(type, "IDAT", 4) == 0)
		{
			uint8_t *grown = realloc(idat, idat_size + length);
			if (!grown)
				break;
			idat = grown;
			memcpy(idat + idat_size, body, length);
			idat_size += length;
		}
		else if (memcmp(type, "IEND", 4) == 0)
			break;
		pos += 12 + length;
	}
	if (!channels || !width || !height || !idat || width > 16384 || height > 16384)
	{
		free(idat);
		return false;
	}

	size_t stride = (size_t)width * channels;
	size_t raw_size = (stride + 1) * height;
	uint8_t *raw = malloc(raw_size);
	uint32_t *pixels = malloc((size_t)width * height * sizeof(uint32_t));
	uLongf got = raw_size;
	bool ok = raw && pixels && uncompress(raw, &got, idat, idat_size) == Z_OK && got == raw_size;
	free(idat);
	if (!ok)
	{
		free(raw);
		free(pixels);
		return false;
	}

	uint8_t *prev = NULL;
	for (uint32_t y = 0; y < height; y++)
	{
		uint8_t *row = raw + y * (stride + 1);
		uint8_t filter = row[0];
		uint8_t *line = row + 1;
		for (size_t i = 0; i < stride; i++)
		{
			int a = i >= (size_t)channels ? line[i - channels] : 0;
			int b = prev ? prev[i] : 0;
			int c = prev && i >= (size_t)channels ? prev[i - channels] : 0;
			switch (filter)
			{
			case 1: line[i] = (uint8_t)(line[i] + a); break;
			case 2: line[i] = (uint8_t)(line[i] + b); break;
			case 3: line[i] = (uint8_t)(line[i] + ((a + b) >> 1)); break;
			case 4: line[i] = (uint8_t)(line[i] + paeth(a, b, c)); break;
			default: break;
			}
		}
		for (uint32_t x = 0; x < width; x++)
		{
			const uint8_t *p = line + (size_t)x * channels;
			uint32_t alpha = channels == 4 ? p[3] : 0xff;
			pixels[(size_t)y * width + x] = (alpha << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
		}
		prev = line;
	}
	free(raw);
	out->width = (int)width;
	out->height = (int)height;
	out->pixels = pixels;
	return true;
}
