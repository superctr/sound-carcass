/* scplay: Standard MIDI File reader.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCPLAY_SMF_H
#define SCPLAY_SMF_H

#include <stddef.h>
#include <stdint.h>

typedef struct smf_event
{
	uint64_t tick;
	uint32_t order;
	uint32_t frame;
	uint16_t length;
	uint8_t tempo_change;
	uint8_t port;
	const uint8_t *bytes;
	uint8_t status[3];
	uint32_t tempo;
} smf_event_t;

typedef struct smf
{
	uint8_t *data;
	smf_event_t *events;
	size_t count, capacity;
	uint16_t division;
	uint32_t last_frame;
	char name[128];
	uint8_t ports;
} smf_t;

#define SMF_PORT_UNSET 0xff

int smf_load(smf_t *s, const char *path, uint32_t rate);
void smf_free(smf_t *s);

#endif
