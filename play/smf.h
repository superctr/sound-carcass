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
	uint16_t track;           /* the track it came from, from 0 */
	uint32_t offset;          /* the byte offset in that track just past it: what a reader has consumed */
	const uint8_t *bytes;
	uint8_t status[3];
	uint32_t tempo;
} smf_event_t;

/* a time signature: from this tick on, bars are `beats` of a `1 << beat_log2`th note */
typedef struct smf_meter
{
	uint64_t tick;
	uint8_t beats, beat_log2;
} smf_meter_t;

/* the tempo map resolved: from this tick, at this many seconds, `tempo` microseconds a quarter note */
typedef struct smf_tempo
{
	uint64_t tick;
	double seconds;
	uint32_t tempo;
} smf_tempo_t;

typedef struct smf
{
	uint8_t *data;
	smf_event_t *events;
	size_t count, capacity;
	uint16_t division;
	uint32_t last_frame;
	uint64_t last_tick;       /* the end of the track, or of the last one in a type 1 file */
	char name[128];
	/* the sequence name as the file spells it, for a display that takes bytes: the first
	 * track's name at its first tick, padded with spaces; empty when there is none */
	char raw_name[32];
	uint8_t ports;
	uint16_t tracks;          /* the tracks the file holds */
	smf_meter_t *meters;
	size_t meter_count;
	smf_tempo_t *tempos;
	size_t tempo_count;
} smf_t;

/* the SC-8850 takes four port groups over USB; every other machine two */
#define SMF_PORTS 4
#define SMF_PORT_UNSET 0xff

int smf_load(smf_t *s, const char *path, uint32_t rate);
void smf_free(smf_t *s);

/* Bars are counted from 1 at the start, from the time signatures (4/4 before the first, and
 * when there are none); a tick is in the bar it falls in, so the end of a song of four whole
 * bars is in bar 5.  smf_tick_of_bar gives the first tick of a bar. */
uint32_t smf_bar_at_tick(const smf_t *s, uint64_t tick);
uint64_t smf_tick_of_bar(const smf_t *s, uint32_t bar);
/* the sample clock (at the rate the file was loaded for) against the tempo map */
uint64_t smf_frame_at_tick(const smf_t *s, uint64_t tick, uint32_t rate);
uint64_t smf_tick_at_frame(const smf_t *s, uint64_t frame, uint32_t rate);
/* microseconds a quarter note in force at a tick (500000 before any tempo event) */
uint32_t smf_tempo_at_tick(const smf_t *s, uint64_t tick);

#endif
