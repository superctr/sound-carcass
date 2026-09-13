/* scemu GUI: the Sound Brush.
 *
 * The SB-55's front panel as a state machine: what its keys do, what its
 * three digits and its lamps show, and what it asks of the player behind it.
 * Here the disk in its slot is the playlist, a song number a row of it, and
 * the player is the emulated module's own.  The behaviour is the SB-55
 * firmware's as measured in MAME (docs/panel/README.md in the project),
 * without the recorder.  No toolkit and no clock of its own: the host feeds
 * it keys, the time in milliseconds and what the player reports, and reads
 * the display back.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef SCEMU_BRUSH_H
#define SCEMU_BRUSH_H

#include <stdbool.h>
#include <stdint.h>

typedef enum brush_key
{
	BRUSH_KEY_POWER, BRUSH_KEY_EJECT,
	BRUSH_KEY_SONG_LEFT, BRUSH_KEY_SONG_RIGHT, BRUSH_KEY_PROG, BRUSH_KEY_SET,
	BRUSH_KEY_TEMPO_LEFT, BRUSH_KEY_TEMPO_RIGHT, BRUSH_KEY_RND, BRUSH_KEY_CLEAR,
	BRUSH_KEY_PAUSE, BRUSH_KEY_REC, BRUSH_KEY_SINGLE, BRUSH_KEY_REPT,
	BRUSH_KEY_STOP, BRUSH_KEY_PLAY, BRUSH_KEY_REW, BRUSH_KEY_FF,
	BRUSH_KEY_COUNT
} brush_key_t;

typedef enum brush_lamp
{
	BRUSH_LAMP_PAUSE, BRUSH_LAMP_REC, BRUSH_LAMP_PLAY, BRUSH_LAMP_PROG, BRUSH_LAMP_RND, BRUSH_LAMP_SINGLE,
	BRUSH_LAMP_REPT, BRUSH_LAMP_STANDBY, BRUSH_LAMP_DISK, BRUSH_LAMP_COUNT
} brush_lamp_t;

/* what the display shows when nothing else is going on: SONG ◀ + ▶, TEMPO ◀ + ▶ and REW + FF choose */
typedef enum brush_show { BRUSH_SHOW_SONG, BRUSH_SHOW_TEMPO, BRUSH_SHOW_MEASURE } brush_show_t;

/* the system functions reached with SET held: the interval between songs (SET + PAUSE), auto
 * play on a disk going in (SET + PLAY), the return to the start on STOP (SET + STOP) */
typedef enum brush_function
{
	BRUSH_FUNCTION_NONE, BRUSH_FUNCTION_INTERVAL, BRUSH_FUNCTION_AUTO_PLAY, BRUSH_FUNCTION_AUTO_REWIND
} brush_function_t;

#define BRUSH_PROGRAM_MAX 99
#define BRUSH_SONGS_MAX 999

/* what the player reports, each tick */
typedef struct brush_player
{
	bool loaded;              /* a song is read in */
	bool ended;               /* it has gone past its last event */
	uint32_t bar, bars;       /* the bar the position is in and the bar the song ends in, from 1 */
	double tempo;             /* the song's own tempo at the position, beats a minute */
	uint64_t frame;           /* the position on the song's clock */
} brush_player_t;

typedef struct brush_actions
{
	void (*load)(void *user, int song);                   /* read a song in (from 0) without starting it */
	void (*start)(void *user, int song, uint64_t frame);  /* play it from a frame of its clock */
	void (*stop)(void *user);                             /* silence; the song stays where it is */
	void (*pause)(void *user, bool paused);
	void (*seek)(void *user, uint32_t bar);               /* to the start of a bar, past the last is the end */
	void (*tempo)(void *user, double factor);             /* the song's tempo scaled */
	void (*eject)(void *user);                            /* the disk is out: the list goes */
	void (*settings)(void *user);                         /* a system function was stored */
} brush_actions_t;

typedef struct brush
{
	const brush_actions_t *act;
	void *user;
	uint64_t now;
	brush_player_t player;
	/* the disk */
	int songs;                 /* on it; 0 is no disk */
	int song;                  /* the one selected, from 0 */
	/* the transport */
	bool standby, playing, paused;
	bool single, rept, rnd, prog;
	bool prog_entry, prog_shown;   /* entering a program; a song shows in place of "--" */
	uint64_t prog_entry_at;
	int program[BRUSH_PROGRAM_MAX];
	int program_len, prog_pos;
	uint8_t played[(BRUSH_SONGS_MAX + 7) / 8];   /* random play: what this round has had */
	uint32_t seed;
	bool started_seen;         /* the player has taken the last start: its end is the song's end */
	/* the settings */
	int interval;              /* seconds between songs, 0..99 */
	bool auto_play, auto_rewind;
	/* keys: a key acts a moment after it goes down, so that the other half of a pair going down
	 * with it makes a chord instead */
	uint32_t down, pending;
	uint64_t down_at[BRUSH_KEY_COUNT];
	/* REW and FF */
	int scan;                  /* -1 REW, +1 FF, 0 neither */
	bool scan_fast, scan_resume;
	uint64_t scan_next;
	uint32_t scan_bar;
	/* the display */
	brush_show_t show;
	int transient;             /* 0 none, 1 the tempo, 2 the bar */
	uint64_t transient_until;
	brush_function_t function;
	int countdown;             /* seconds left before the next song; 0 when not waiting */
	uint64_t countdown_at;
	int next;                  /* the song the countdown leads to */
	int tempo;                 /* the tempo set by hand, or 0 for the song's own */
	double tempo_factor;
	uint64_t disk_until;       /* the DISK lamp is lit while a song is being read */
	uint8_t digits[3];
	uint32_t lamps;
	bool dirty;
} brush_t;

void brush_init(brush_t *b, const brush_actions_t *act, void *user);

/* The list: its length (0 is no disk; a disk going in selects its first song and plays it when
 * auto play is on), a row removed, a row moved, a row put in before another; the selection
 * follows the row it was on. */
void brush_set_disk(brush_t *b, int songs, uint64_t now);
void brush_remove(brush_t *b, int index, uint64_t now);
void brush_move(brush_t *b, int from, int to);
void brush_insert(brush_t *b, int index);

/* a key of the panel, down or up, at this time */
void brush_key(brush_t *b, brush_key_t key, bool down, uint64_t now);
/* the host's own choice of a song: a row of the list, played at once or only selected */
void brush_select(brush_t *b, int song, bool play, uint64_t now);
/* the host stopped the player itself (the module switched off): the transport follows */
void brush_stop(brush_t *b, uint64_t now);
/* the time passing and the player's state; every so often, and after every key */
void brush_tick(brush_t *b, uint64_t now, const brush_player_t *player);

/* three segment masks, leftmost digit first: bit 0 is a (the top) through bit 6 g (the middle),
 * bit 7 the point, which sits at the top left of its digit */
const uint8_t *brush_digits(const brush_t *b);
uint32_t brush_lamps(const brush_t *b);   /* a bit per brush_lamp_t */
int brush_song(const brush_t *b);         /* the selected song, from 0; -1 with no disk */
bool brush_playing(const brush_t *b);
bool brush_dirty(brush_t *b);             /* the display or the lamps changed since last asked */

/* the segment mask of a character the display can show ('0'-'9', '-', 'o', 'F', 'n', ' ') */
uint8_t brush_glyph(char c);

#endif
