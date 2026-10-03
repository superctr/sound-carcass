/* scplay: Shift-JIS text to UTF-8, and the display width of UTF-8.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCPLAY_SJIS_H
#define SCPLAY_SJIS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool sjis_is_utf8(const uint8_t *in, size_t length);
size_t sjis_decode(const uint8_t *in, size_t length, char *out, size_t size);
size_t sjis_text(const uint8_t *in, size_t length, char *out, size_t size);
int sjis_columns(const char *text);
size_t sjis_fit(const char *text, int columns);

#endif
