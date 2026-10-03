/* scemu GUI: a reader for the baked artwork's PNG files.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCEMU_PNG_H
#define SCEMU_PNG_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct png_image
{
	int width, height;
	uint32_t *pixels;   /* 0xAARRGGBB, straight alpha, rows top to bottom; free() it */
} png_image_t;

/* 8-bit RGB and RGBA, not interlaced: what the bake writes. */
bool png_decode(const uint8_t *data, size_t size, png_image_t *out);

#endif
