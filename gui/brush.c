/* scemu GUI: the Sound Brush.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <string.h>
#include "brush.h"

#define SCAN_FIRST_MS 500      /* a held REW or FF steps once at once, again after this */
#define SCAN_STEP_MS 120       /* and then this often */
#define SCAN_FAST_BARS 4       /* with the other key pressed as well */
#define SCAN_SHOW_MS 800       /* the bar stays on the display after the key comes up */
#define TEMPO_SHOW_MS 1000     /* the tempo after a TEMPO key */
#define PROG_BLINK_MS 300      /* the PROG lamp's half period while a program is entered */
#define DISK_MS 500            /* the DISK lamp while a song is read in */
#define DISK_READ_MS 150       /* and for each read the player reports as it plays */
#define CHORD_MS 50            /* a key acts this long after going down, unless its pair partner joins it */
#define REPEAT_FIRST_MS 500    /* a held SONG or TEMPO key steps again after this */
#define REPEAT_MS 100          /* and then this often; with the other half of its pair held, every tick */
#define TEMPO_MIN 5
#define TEMPO_MAX 260

enum { SHOW_NONE, SHOW_TEMPO, SHOW_BAR };

static const struct { char c; uint8_t mask; } font[] = {
	{ '0', 0x3f }, { '1', 0x06 }, { '2', 0x5b }, { '3', 0x4f }, { '4', 0x66 }, { '5', 0x6d }, { '6', 0x7d },
	{ '7', 0x07 }, { '8', 0x7f }, { '9', 0x6f }, { '-', 0x40 }, { 'o', 0x5c }, { 'F', 0x71 }, { 'n', 0x54 },
	{ ' ', 0x00 },
};

uint8_t brush_glyph(char c)
{
	for (size_t n = 0; n < sizeof(font) / sizeof(font[0]); n++)
		if (font[n].c == c)
			return font[n].mask;
	return 0;
}

void brush_init(brush_t *b, const brush_actions_t *act, void *user)
{
	memset(b, 0, sizeof(*b));
	b->act = act;
	b->user = user;
	b->seed = 1;
	b->interval = 4;
	b->auto_play = b->auto_rewind = true;
	b->tempo_factor = 1;
	b->record_tempo = 120;
	b->repeat_key = -1;
	b->dirty = true;
}

/* ---------------------------------------------------------------- helpers */

static bool held(const brush_t *b, brush_key_t k) { return (b->down >> k) & 1; }

static uint32_t random_below(brush_t *b, uint32_t n)
{
	b->seed = b->seed * 1103515245u + 12345u;
	return (b->seed >> 8) % n;
}

static bool was_played(const brush_t *b, int n) { return (b->played[n / 8] >> (n % 8)) & 1; }
static void mark_played(brush_t *b, int n) { if (n >= 0 && n < BRUSH_SONGS_MAX) b->played[n / 8] |= (uint8_t)(1 << (n % 8)); }
static void clear_played(brush_t *b) { memset(b->played, 0, sizeof(b->played)); }

static void disk_read(brush_t *b) { b->disk_until = b->now + DISK_MS; }

/* a read as the song plays: a shorter flash, unless a longer one is on */
static void disk_chunk(brush_t *b)
{
	if (b->now + DISK_READ_MS > b->disk_until)
		b->disk_until = b->now + DISK_READ_MS;
}

static void load(brush_t *b, int song)
{
	b->song = song;
	disk_read(b);
	b->act->load(b->user, song);
}

/* the song's position in the program, looked for from the position last played, or -1 */
static int program_position(const brush_t *b, int song)
{
	for (int n = b->prog_pos; n < b->program_len; n++)
		if (b->program[n] == song)
			return n;
	for (int n = 0; n < b->prog_pos && n < b->program_len; n++)
		if (b->program[n] == song)
			return n;
	return -1;
}

/* a random entry not yet played this round: of the program, or of the disk */
static int random_unplayed(brush_t *b)
{
	int count = b->prog ? b->program_len : b->songs, left = 0;
	for (int n = 0; n < count; n++)
		if (!was_played(b, n))
			left++;
	if (!left)
		return -1;
	int pick = (int)random_below(b, (uint32_t)left);
	for (int n = 0; n < count; n++)
		if (!was_played(b, n) && pick-- == 0)
			return n;
	return -1;
}

/* play a song from the start: the lamp, the round's bookkeeping, the tempo back to the song's own */
static void begin(brush_t *b, int song, uint64_t frame)
{
	b->song = song;
	b->playing = true;
	b->paused = false;
	b->started_seen = false;
	b->countdown = 0;
	if (b->prog)
	{
		int at = program_position(b, song);
		if (at >= 0)
		{
			b->prog_pos = at;
			mark_played(b, at);
		}
	}
	else
		mark_played(b, song);
	if (b->tempo)
	{
		b->tempo = 0;
		b->tempo_factor = 1;
		b->act->tempo(b->user, 1);
	}
	disk_read(b);
	b->act->start(b->user, song, frame);
}

/* the song that follows in the order of the mode, or -1 at the end of the order */
static int following(brush_t *b)
{
	if (b->prog)
	{
		if (!b->program_len)
			return -1;
		if (b->rnd)
		{
			int at = random_unplayed(b);
			return at < 0 ? -1 : b->program[at];
		}
		int at = program_position(b, b->song);
		return at >= 0 && at + 1 < b->program_len ? b->program[at + 1] : -1;
	}
	if (b->rnd)
		return random_unplayed(b);
	return b->song + 1 < b->songs ? b->song + 1 : -1;
}

/* the order started again: the program's first song, a fresh random round, the first song */
static int wrapped(brush_t *b)
{
	clear_played(b);
	if (b->prog)
	{
		b->prog_pos = 0;
		return b->program_len ? (b->rnd ? b->program[random_unplayed(b)] : b->program[0]) : -1;
	}
	if (b->rnd)
		return random_unplayed(b);
	return 0;
}

static void stop(brush_t *b)
{
	if (b->playing)
		b->act->stop(b->user);
	b->playing = b->paused = false;
	b->countdown = 0;
	if (b->scan)
	{
		b->scan = 0;
		b->scan_resume = false;
	}
}

static void record_end(brush_t *b)
{
	b->recording = false;
	b->act->record(b->user, false, b->record_tempo);
}

/* the song has run out: the next one after the interval, selected from the start of the count so
 * that a STOP and a PLAY during it go on to it, or a stop with the next in line selected */
static void song_over(brush_t *b)
{
	b->playing = b->paused = false;
	int next;
	if (b->single && b->rept)
		next = b->song;
	else if (b->single)
	{
		next = -1;
		int line = following(b);
		if (line >= 0)
			load(b, line);
	}
	else
	{
		next = following(b);
		if (next < 0 && b->rept)
			next = wrapped(b);
		if (next < 0)
		{
			int first = b->prog ? (b->program_len ? b->program[0] : b->song) : b->rnd ? b->song : 0;
			b->prog_pos = 0;
			if (first != b->song)
				load(b, first);
			else
				b->act->seek(b->user, 1);   /* the player ends its run there, its tail ringing on */
		}
	}
	if (next < 0)
		return;
	if (b->interval > 0)
	{
		b->countdown = b->interval;
		b->countdown_at = b->now;
		b->song = next;
	}
	else
		begin(b, next, 0);
}

static void compose(brush_t *b);

static void show_transient(brush_t *b, int what, unsigned ms)
{
	b->transient = what;
	b->transient_until = b->now + ms;
}

/* the tempo the display would show: the recorder's while no song plays, else the one set by
 * hand, or the song's own */
static int shown_tempo(const brush_t *b)
{
	if (!b->playing)
		return b->record_tempo;
	if (b->tempo)
		return b->tempo;
	double t = b->player.tempo > 0 ? b->player.tempo : 120;
	return (int)(t + 0.5);
}

/* the song's tempo while one plays; the recorder's otherwise */
static void set_tempo(brush_t *b, int tempo)
{
	tempo = tempo < TEMPO_MIN ? TEMPO_MIN : tempo > TEMPO_MAX ? TEMPO_MAX : tempo;
	if (!b->playing)
	{
		b->record_tempo = tempo;
		show_transient(b, SHOW_TEMPO, TEMPO_SHOW_MS);
		return;
	}
	b->tempo = tempo;
	double own = b->player.tempo > 0 ? b->player.tempo : 120;
	b->tempo_factor = tempo / own;
	b->act->tempo(b->user, b->tempo_factor);
	show_transient(b, SHOW_TEMPO, TEMPO_SHOW_MS);
}

static void scan_step(brush_t *b)
{
	int step = b->scan * (b->scan_fast ? SCAN_FAST_BARS : 1);
	int bar = (int)b->scan_bar + step;
	uint32_t last = b->player.bars ? b->player.bars : 1;
	b->scan_bar = bar < 1 ? 1 : (uint32_t)bar > last ? last : (uint32_t)bar;
	b->act->seek(b->user, b->scan_bar);
	b->transient = SHOW_BAR;
}

static void scan_begin(brush_t *b, int dir)
{
	if (!b->player.loaded)
		return;
	b->scan = dir;
	b->scan_fast = false;
	b->scan_bar = b->player.bar ? b->player.bar : 1;
	if (b->playing && !b->paused)
	{
		b->scan_resume = true;
		b->act->pause(b->user, true);
	}
	scan_step(b);
	b->scan_next = b->now + SCAN_FIRST_MS;
}

static void scan_end(brush_t *b)
{
	if (!b->scan)
		return;
	b->scan = 0;
	show_transient(b, SHOW_BAR, SCAN_SHOW_MS);
	if (b->scan_resume)
	{
		b->scan_resume = false;
		b->act->pause(b->user, false);
	}
}

/* a SONG or TEMPO key's step, one its own way -- or, with the other half of its pair held, the way
 * of whichever went down first, since holding one and pressing the other is how a value is run
 * through, at speed */
static void value_key(brush_t *b, brush_key_t key);

static void step_song(brush_t *b, int by)
{
	if (!b->songs)
		return;
	int song = b->song + by;
	song = song < 0 ? 0 : song >= b->songs ? b->songs - 1 : song;
	if (song == b->song)
		return;
	if (b->playing)
		begin(b, song, 0);
	else
		load(b, song);
}

static void program_cancel(brush_t *b)
{
	b->program_len = 0;
	b->prog_pos = 0;
	b->prog = b->prog_entry = false;
}

/* the end of program entry: the program is on when it holds anything, its first song selected */
static void program_done(brush_t *b)
{
	b->prog_entry = false;
	b->prog = b->program_len > 0;
	b->prog_pos = 0;
	if (b->prog && !b->playing)
		load(b, b->program[0]);
}

static void play(brush_t *b)
{
	if (!b->songs)
		return;
	if (b->paused)
	{
		b->paused = false;
		b->act->pause(b->user, false);
		return;
	}
	if (b->playing)
		return;
	if (b->countdown)
	{
		begin(b, b->song, 0);
		return;
	}
	if (b->rnd)
		clear_played(b);
	/* on from where it stopped, when that is the same song and it is not at its end */
	uint64_t frame = b->player.loaded && !b->player.ended ? b->player.frame : 0;
	begin(b, b->song, frame);
}

/* ---------------------------------------------------------------- keys */

static void function_key(brush_t *b, brush_key_t key)
{
	int by = key == BRUSH_KEY_FF ? 1 : key == BRUSH_KEY_REW ? -1 : 0;
	switch (b->function)
	{
	case BRUSH_FUNCTION_INTERVAL:
		b->interval += by;
		b->interval = b->interval < 0 ? 0 : b->interval > 99 ? 99 : b->interval;
		break;
	case BRUSH_FUNCTION_AUTO_PLAY:   /* FF is on, REW is off */
		if (by)
			b->auto_play = by > 0;
		break;
	case BRUSH_FUNCTION_AUTO_REWIND:   /* the other way round, as the manual has it */
		if (by)
			b->auto_rewind = by < 0;
		break;
	default:
		break;
	}
	if (key == BRUSH_KEY_SET)
	{
		b->function = BRUSH_FUNCTION_NONE;
		b->act->settings(b->user);
	}
}

static void entry_key(brush_t *b, brush_key_t key)
{
	switch (key)
	{
	case BRUSH_KEY_SONG_LEFT:
	case BRUSH_KEY_SONG_RIGHT:
		/* the first press shows the song selected, the next ones step it */
		if (!b->prog_shown)
			b->prog_shown = true;
		else
			step_song(b, key == BRUSH_KEY_SONG_RIGHT ? 1 : -1);
		break;
	case BRUSH_KEY_SET:
		if (b->prog_shown && b->program_len < BRUSH_PROGRAM_MAX)
			b->program[b->program_len++] = b->song;
		b->prog_shown = false;
		break;
	case BRUSH_KEY_STOP:
		program_done(b);
		break;
	case BRUSH_KEY_PLAY:
		program_done(b);
		if (b->prog)
			begin(b, b->program[0], 0);
		break;
	case BRUSH_KEY_PROG:
		if (held(b, BRUSH_KEY_CLEAR))
			program_cancel(b);
		break;
	default:
		break;
	}
}

int brush_partner(brush_key_t key)
{
	switch (key)
	{
	case BRUSH_KEY_SONG_LEFT: return BRUSH_KEY_SONG_RIGHT;
	case BRUSH_KEY_SONG_RIGHT: return BRUSH_KEY_SONG_LEFT;
	case BRUSH_KEY_TEMPO_LEFT: return BRUSH_KEY_TEMPO_RIGHT;
	case BRUSH_KEY_TEMPO_RIGHT: return BRUSH_KEY_TEMPO_LEFT;
	case BRUSH_KEY_REW: return BRUSH_KEY_FF;
	case BRUSH_KEY_FF: return BRUSH_KEY_REW;
	default: return -1;
	}
}

/* a pair pressed together chooses what the display shows */
static void chord(brush_t *b, brush_key_t key)
{
	if (b->standby || b->function != BRUSH_FUNCTION_NONE || b->prog_entry)
		return;
	if (key == BRUSH_KEY_SONG_LEFT || key == BRUSH_KEY_SONG_RIGHT)
		b->show = BRUSH_SHOW_SONG;
	else if (key == BRUSH_KEY_TEMPO_LEFT || key == BRUSH_KEY_TEMPO_RIGHT)
		b->show = BRUSH_SHOW_TEMPO;
	else
	{
		scan_end(b);
		b->show = BRUSH_SHOW_MEASURE;
	}
	b->transient = SHOW_NONE;
}

static void press(brush_t *b, brush_key_t key)
{
	if (b->standby)
	{
		if (key == BRUSH_KEY_POWER)
			b->standby = false;
		return;
	}
	if (key == BRUSH_KEY_POWER)
	{
		stop(b);
		if (b->recording)
			record_end(b);
		b->function = BRUSH_FUNCTION_NONE;
		b->prog_entry = false;
		b->standby = true;
		return;
	}
	if (key == BRUSH_KEY_EJECT)
	{
		if (!b->songs)
			return;
		stop(b);
		if (b->recording)
			record_end(b);
		program_cancel(b);
		b->songs = 0;
		b->song = 0;
		b->act->eject(b->user);
		return;
	}
	/* taking down: REC again or STOP ends it, and nothing else acts meanwhile */
	if (b->recording)
	{
		if (key == BRUSH_KEY_REC || key == BRUSH_KEY_STOP)
			record_end(b);
		return;
	}
	if (b->function != BRUSH_FUNCTION_NONE)
	{
		function_key(b, key);
		return;
	}
	if (b->prog_entry)
	{
		entry_key(b, key);
		return;
	}
	if (held(b, BRUSH_KEY_SET))
	{
		switch (key)
		{
		case BRUSH_KEY_PROG:
			b->prog_entry = true;
			b->prog_shown = false;
			b->prog_entry_at = b->now;
			break;
		case BRUSH_KEY_PAUSE: b->function = BRUSH_FUNCTION_INTERVAL; break;
		case BRUSH_KEY_PLAY: b->function = BRUSH_FUNCTION_AUTO_PLAY; break;
		case BRUSH_KEY_STOP: b->function = BRUSH_FUNCTION_AUTO_REWIND; break;
		default: break;
		}
		return;
	}
	if (held(b, BRUSH_KEY_CLEAR))
	{
		if (key == BRUSH_KEY_PROG)
			program_cancel(b);
		else if (key == BRUSH_KEY_TEMPO_LEFT || key == BRUSH_KEY_TEMPO_RIGHT)
		{
			if (b->playing)
			{
				b->tempo = 0;
				b->tempo_factor = 1;
				b->act->tempo(b->user, 1);
			}
			else
				b->record_tempo = 120;
			show_transient(b, SHOW_TEMPO, TEMPO_SHOW_MS);
		}
		return;
	}
	if (held(b, BRUSH_KEY_STOP) && (key == BRUSH_KEY_REW || key == BRUSH_KEY_FF) && b->player.loaded)
	{
		b->scan_bar = key == BRUSH_KEY_REW ? 1 : (b->player.bars ? b->player.bars : 1);
		b->act->seek(b->user, b->scan_bar);
		show_transient(b, SHOW_BAR, SCAN_SHOW_MS);
		return;
	}
	switch (key)
	{
	/* the other half of a pair, pressed while this one is held, moves faster the held one's way */
	case BRUSH_KEY_SONG_LEFT:
	case BRUSH_KEY_SONG_RIGHT:
	case BRUSH_KEY_TEMPO_LEFT:
	case BRUSH_KEY_TEMPO_RIGHT:
		value_key(b, key);
		break;
	case BRUSH_KEY_PROG:
		if (!b->program_len)
			break;
		b->prog = !b->prog;
		b->prog_pos = 0;
		if (b->prog && !b->playing)
			load(b, b->program[0]);
		break;
	case BRUSH_KEY_RND:
		b->rnd = !b->rnd;
		clear_played(b);
		if (b->rnd && !b->playing && b->songs)
		{
			int pick = random_unplayed(b);
			if (pick >= 0)
				load(b, b->prog ? b->program[pick] : pick);
		}
		break;
	case BRUSH_KEY_SINGLE:
		b->single = !b->single;
		break;
	case BRUSH_KEY_REPT:
		b->rept = !b->rept;
		break;
	case BRUSH_KEY_PAUSE:
		if (b->playing && !b->scan)
		{
			b->paused = !b->paused;
			b->act->pause(b->user, b->paused);
		}
		break;
	case BRUSH_KEY_STOP:
		if (b->countdown)
			b->countdown = 0;
		else if (b->playing)
		{
			stop(b);
			if (b->auto_rewind)
				b->act->seek(b->user, 1);
		}
		else if (b->player.loaded)   /* a song that ran out may still be sounding */
			b->act->stop(b->user);
		break;
	case BRUSH_KEY_PLAY:
		play(b);
		break;
	case BRUSH_KEY_REC:
		/* not the unit's REC, which waits for PLAY: the take starts now, in place of any song */
		stop(b);
		b->recording = true;
		b->record_at = b->now;
		b->act->record(b->user, true, b->record_tempo);
		break;
	case BRUSH_KEY_REW:
	case BRUSH_KEY_FF:
		if (b->scan)
			b->scan_fast = true;
		else
			scan_begin(b, key == BRUSH_KEY_FF ? 1 : -1);
		break;
	default:
		break;
	}
}

static void value_key(brush_t *b, brush_key_t key)
{
	int other = brush_partner(key);
	bool both = other >= 0 && held(b, (brush_key_t)other);
	brush_key_t way = both && b->repeat_key != (int)key ? (brush_key_t)other : key;
	int by = way == BRUSH_KEY_SONG_RIGHT || way == BRUSH_KEY_TEMPO_RIGHT ? 1 : -1;
	if (key == BRUSH_KEY_SONG_LEFT || key == BRUSH_KEY_SONG_RIGHT)
		step_song(b, by);
	else
		set_tempo(b, shown_tempo(b) + by);
}

static bool repeats(brush_key_t key)
{
	return key == BRUSH_KEY_SONG_LEFT || key == BRUSH_KEY_SONG_RIGHT || key == BRUSH_KEY_TEMPO_LEFT
	       || key == BRUSH_KEY_TEMPO_RIGHT;
}

static void release(brush_t *b, brush_key_t key)
{
	if ((key == BRUSH_KEY_REW || key == BRUSH_KEY_FF) && b->scan)
		scan_end(b);
	if (b->repeat_key == (int)key)
		b->repeat_key = -1;
	else if (b->repeat_key >= 0 && brush_partner((brush_key_t)b->repeat_key) == (int)key)
		b->repeat_next = b->now + REPEAT_MS;   /* the other half up: back to the slow steps */
}

void brush_key(brush_t *b, brush_key_t key, bool down, uint64_t now)
{
	b->now = now;
	uint32_t bit = 1u << key;
	if (down)
	{
		if (b->down & bit)
			return;
		b->down |= bit;
		b->down_at[key] = now;
		int other = brush_partner(key);
		if (other >= 0 && (b->pending >> other) & 1)
		{
			/* the two halves within the window: neither acts on its own */
			b->pending &= ~(1u << other);
			b->repeat_key = -1;
			chord(b, key);
		}
		else
		{
			b->pending |= bit;
			/* the first of a pair held steps again after a while, until it comes up; its other
			 * half going down meanwhile makes it step at every tick from now */
			if (repeats(key) && b->repeat_key < 0)
			{
				b->repeat_key = (int)key;
				b->repeat_next = now + REPEAT_FIRST_MS;
			}
			else if (b->repeat_key >= 0 && other == b->repeat_key)
				b->repeat_next = now;
		}
	}
	else
	{
		if (!(b->down & bit))
			return;
		b->down &= ~bit;
		if (b->pending & bit)   /* a tap shorter than the window still counts */
		{
			b->pending &= ~bit;
			press(b, key);
		}
		release(b, key);
	}
	brush_tick(b, now, &b->player);
}

/* ---------------------------------------------------------------- the list */

void brush_set_disk(brush_t *b, int songs, uint64_t now)
{
	b->now = now;
	if (songs > BRUSH_SONGS_MAX)
		songs = BRUSH_SONGS_MAX;
	int was = b->songs;
	b->songs = songs;
	if (!songs)
	{
		stop(b);
		program_cancel(b);
		b->song = 0;
		b->tempo = 0;
		b->tempo_factor = 1;
	}
	else if (!was)
	{
		clear_played(b);
		if (b->auto_play && !b->standby)
			begin(b, 0, 0);
		else
			load(b, 0);
	}
	else if (b->song >= songs)
		load(b, songs - 1);
	brush_tick(b, now, &b->player);
}

void brush_remove(brush_t *b, int index, uint64_t now)
{
	b->now = now;
	if (index < 0 || index >= b->songs)
		return;
	program_cancel(b);
	if (index == b->song)
	{
		stop(b);
		b->songs--;
		if (b->songs)
			load(b, index < b->songs ? index : b->songs - 1);
		else
			brush_set_disk(b, 0, now);
	}
	else
	{
		if (index < b->song)
			b->song--;
		b->songs--;
	}
	brush_tick(b, now, &b->player);
}

void brush_move(brush_t *b, int from, int to)
{
	if (from == to || from < 0 || to < 0 || from >= b->songs || to >= b->songs)
		return;
	if (b->song == from)
		b->song = to;
	else if (from < b->song && b->song <= to)
		b->song--;
	else if (to <= b->song && b->song < from)
		b->song++;
	for (int n = 0; n < b->program_len; n++)
	{
		if (b->program[n] == from)
			b->program[n] = to;
		else if (from < b->program[n] && b->program[n] <= to)
			b->program[n]--;
		else if (to <= b->program[n] && b->program[n] < from)
			b->program[n]++;
	}
}

void brush_insert(brush_t *b, int index)
{
	if (b->songs >= BRUSH_SONGS_MAX)
		return;
	if (!b->songs)
	{
		brush_set_disk(b, 1, b->now);
		return;
	}
	if (index < 0 || index > b->songs)
		index = b->songs;
	if (index <= b->song)
		b->song++;
	for (int n = 0; n < b->program_len; n++)
		if (b->program[n] >= index)
			b->program[n]++;
	b->songs++;
}

void brush_select(brush_t *b, int song, bool play_it, uint64_t now)
{
	b->now = now;
	if (song < 0 || song >= b->songs || b->standby)
		return;
	if (b->prog_entry || b->function != BRUSH_FUNCTION_NONE)
		return;
	if (play_it)
	{
		if (b->rnd)
			clear_played(b);
		begin(b, song, 0);
	}
	else if (song != b->song)
	{
		if (b->playing)
			begin(b, song, 0);
		else
			load(b, song);
	}
	brush_tick(b, now, &b->player);
}

void brush_stop(brush_t *b, uint64_t now)
{
	b->now = now;
	stop(b);
	brush_tick(b, now, &b->player);
}

void brush_attach(brush_t *b, int songs, int song, bool playing, bool paused, uint64_t now)
{
	b->now = now;
	b->songs = songs < 0 ? 0 : songs > BRUSH_SONGS_MAX ? BRUSH_SONGS_MAX : songs;
	b->song = b->songs && song >= 0 && song < b->songs ? song : 0;
	b->playing = b->songs && playing;
	b->paused = b->playing && paused;
	b->started_seen = b->playing;
	b->standby = false;
	b->countdown = 0;
	b->recording = false;
	b->scan = 0;
	b->scan_resume = false;
	b->pending = 0;
	b->function = BRUSH_FUNCTION_NONE;
	b->prog_entry = false;
	b->transient = SHOW_NONE;
	clear_played(b);
	if (b->playing)
		mark_played(b, b->prog ? program_position(b, b->song) : b->song);
	/* what the player last said is from before the hand-over: the host's next tick says afresh */
	compose(b);
}

/* ---------------------------------------------------------------- time */

/* a number right-aligned with `width` digits shown (zero padded to that), the rest blank */
static void digits_number(uint8_t *d, unsigned v, int width)
{
	d[0] = d[1] = d[2] = 0;
	if (v > 999)
		v = 999;
	if (v >= 100)
		width = 3;
	else if (v >= 10 && width < 2)
		width = 2;
	for (int n = 0; n < width; n++)
	{
		d[2 - n] = brush_glyph((char)('0' + v % 10));
		v /= 10;
	}
}

static void digits_text(uint8_t *d, const char *s)
{
	for (int n = 0; n < 3; n++)
		d[n] = brush_glyph(s[n]);
}

static void compose(brush_t *b)
{
	uint8_t d[3] = { 0, 0, 0 };
	uint32_t lamps = 0;
	if (b->standby)
		lamps = 1u << BRUSH_LAMP_STANDBY;
	else
	{
		switch (b->function)
		{
		case BRUSH_FUNCTION_INTERVAL: digits_number(d, (unsigned)b->interval, 1); break;
		case BRUSH_FUNCTION_AUTO_PLAY: digits_text(d, b->auto_play ? " on" : "oFF"); break;
		case BRUSH_FUNCTION_AUTO_REWIND: digits_text(d, b->auto_rewind ? " on" : "oFF"); break;
		default:
			if (b->recording)   /* the bar the take is in, four beats at its tempo */
				digits_number(d, (unsigned)(1 + (b->now - b->record_at) * (uint64_t)b->record_tempo / 240000), 3);
			else if (b->transient == SHOW_TEMPO)   /* the recorder's, with or without a disk */
			{
				digits_number(d, (unsigned)shown_tempo(b), 1);
				d[0] |= 0x80;
			}
			else if (!b->songs)
				digits_text(d, "---");
			else if (b->prog_entry && !b->prog_shown)
				digits_text(d, " --");
			else if (b->countdown)
			{
				digits_text(d, "-  ");
				d[2] = brush_glyph((char)('0' + b->countdown % 10));
			}
			else if (b->scan || b->transient == SHOW_BAR)
				digits_number(d, b->scan_bar, 3);
			else if (b->show == BRUSH_SHOW_TEMPO && !b->prog_entry)
			{
				digits_number(d, (unsigned)shown_tempo(b), 1);
				d[0] |= 0x80;
			}
			else if (b->show == BRUSH_SHOW_MEASURE && !b->prog_entry)
				digits_number(d, b->player.loaded ? b->player.bar : 1, 3);
			else
			{
				digits_number(d, (unsigned)b->song + 1, 2);
				if (b->song + 1 < 100)
					d[1] |= 0x80;
			}
			break;
		}
		if (b->playing)
			lamps |= 1u << BRUSH_LAMP_PLAY;
		if (b->paused)
			lamps |= 1u << BRUSH_LAMP_PAUSE;
		if (b->recording)
			lamps |= 1u << BRUSH_LAMP_REC;
		if (b->prog || (b->prog_entry && ((b->now - b->prog_entry_at) / PROG_BLINK_MS) % 2 == 0))
			lamps |= 1u << BRUSH_LAMP_PROG;
		if (b->rnd)
			lamps |= 1u << BRUSH_LAMP_RND;
		if (b->single)
			lamps |= 1u << BRUSH_LAMP_SINGLE;
		if (b->rept)
			lamps |= 1u << BRUSH_LAMP_REPT;
		if (b->songs && b->now < b->disk_until)
			lamps |= 1u << BRUSH_LAMP_DISK;
	}
	if (memcmp(d, b->digits, 3) != 0 || lamps != b->lamps)
	{
		memcpy(b->digits, d, 3);
		b->lamps = lamps;
		b->dirty = true;
	}
}

void brush_tick(brush_t *b, uint64_t now, const brush_player_t *player)
{
	b->now = now;
	if (player != &b->player)
		b->player = *player;
	if (b->player.loaded && b->player.reads != b->reads_seen)
		disk_chunk(b);
	b->reads_seen = b->player.reads;
	for (int key = 0; key < BRUSH_KEY_COUNT; key++)
		if (((b->pending >> key) & 1) && now - b->down_at[key] >= CHORD_MS)
		{
			b->pending &= ~(1u << key);
			press(b, (brush_key_t)key);
		}
	if (b->scan && now >= b->scan_next)
	{
		scan_step(b);
		b->scan_next += SCAN_STEP_MS;
	}
	if (b->repeat_key >= 0 && now >= b->repeat_next && !((b->pending >> b->repeat_key) & 1))
	{
		press(b, (brush_key_t)b->repeat_key);
		int other = brush_partner((brush_key_t)b->repeat_key);
		b->repeat_next = other >= 0 && held(b, (brush_key_t)other) ? now : b->repeat_next + REPEAT_MS;
	}
	if (b->transient && !b->scan && now >= b->transient_until)
		b->transient = SHOW_NONE;
	if (b->countdown && now - b->countdown_at >= 1000)
	{
		b->countdown--;
		b->countdown_at += 1000;
		if (!b->countdown)
			begin(b, b->song, 0);
	}
	if (b->playing)
	{
		if (!b->started_seen)
			b->started_seen = b->player.loaded && !b->player.ended;
		else if (b->player.ended && !b->scan)
			song_over(b);
	}
	compose(b);
}

/* ---------------------------------------------------------------- readback */

const uint8_t *brush_digits(const brush_t *b) { return b->digits; }
uint32_t brush_lamps(const brush_t *b) { return b->lamps; }
int brush_song(const brush_t *b) { return b->songs ? b->song : -1; }
bool brush_playing(const brush_t *b) { return b->playing; }

bool brush_dirty(brush_t *b)
{
	bool was = b->dirty;
	b->dirty = false;
	return was;
}
