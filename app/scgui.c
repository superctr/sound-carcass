/* scgui: the Sound Canvas on the desktop -- the front panel in a window,
 * a playlist beside it.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define _POSIX_C_SOURCE 200809L
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gtk/gtk.h>
#include "scemu.h"
#include "panel.h"
#include "machine.h"
#include "session.h"
#include "smf.h"
#include "midi_io.h"
#include "audio.h"
#include "roms.h"
#include "config.h"
#include "combos.h"
#include "controls.h"
#include "git_version.h"

#define MIDI_PORTS_MAX 64
#define AUDIO_DEVICES_MAX 64
#define MIDI_SLOTS 9           /* MIDI IN A-D, MIDI OUT, Song A-D; C and D only on an SC-8850 on USB */
#define COMPUTER_POSITIONS 4   /* the rear switch: MIDI, PC-1, PC-2, Mac (USB on the SC-8850) */

typedef struct app
{
	machine_t *mc;
	panel_t *panel;
	int size, scale;           /* the window size setting (4 or 8) and the screen's scale factor */
	uint32_t *frame;
	GtkWidget *window, *area, *playlist_window, *list;
	GtkWidget *settings_window, *notebook, *audio_label, *audio_drop, *rate_drop, *block_drop;
	GtkWidget *system_label, *model_check[MACHINE_SYSTEMS], *computer_check[COMPUTER_POSITIONS], *cache_label;
	GtkWidget *size_drop, *swap_check;
	GtkWidget *logo_popover, *system_popover;
	GSimpleAction *system_model_action;   /* the system menu's radio state */
	scgui_config_t cfg;
	char config_file[1024];
	guint config_timer;
	GMainLoop *loop;
	GPtrArray *songs;          /* char * paths */
	int current;               /* index in songs, or -1 */
	bool paused;
	controls_t ctl;            /* the panel under the pointer; the power switch's state is its */
	uint64_t seen_generation;
	machine_state_t state;
	double pointer_x, pointer_y;
	midi_port_info_t ports[MIDI_PORTS_MAX];
	int port_count;
	audio_device_info_t devices[AUDIO_DEVICES_MAX];
	int device_count;
	int audio_choice;              /* index into devices, or -1 for the default */
	int rate_choice;               /* index into output_rates */
	int block_choice;
	bool system_updating;          /* the radio group is being set from the machine */
	scemu_model_t shown_model;
	GtkWidget *midi_drop[MIDI_SLOTS], *midi_label[MIDI_SLOTS];
	int midi_choice[MIDI_SLOTS];   /* index into ports, or -1 */
	GtkWidget *combo_popover;
	GtkWidget *more;               /* the revealer with the less-used settings */
	machine_reset_t reset;
	scemu_map_t map;
	int combo_ids[64];
} app_t;

static const char *const midi_slot_names[MIDI_SLOTS] = { "MIDI IN A", "MIDI IN B", "MIDI IN C", "MIDI IN D", "MIDI OUT",
                                                         "Song to A", "Song to B", "Song to C", "Song to D" };
static const bool midi_slot_is_input[MIDI_SLOTS] = { true, true, true, true, false, false, false, false, false };
/* the slots that are only there with four port groups */
static const bool midi_slot_is_extra[MIDI_SLOTS] = { false, false, true, true, false, false, false, true, true };

/* ---------------------------------------------------------------- options */

typedef struct options
{
	const char *model, *rom;
	scemu_map_t map;
	uint32_t midi_rate;
	int size;
	scemu_computer_switch_t computer[MACHINE_SYSTEMS];
	bool keep_settings, no_cache, no_audio;
	double tail;
	unsigned audio_rate;
} options_t;

/* what the output is asked of the device: 0 is the machine's own rate, the
 * words are --rate's and the settings file's, the labels the Settings tab's */
static const int output_rates[] = { 0, 32000, 44100, 48000 };
static const char *const output_rate_words[] = { "native", "32000", "44100", "48000" };
static const char *const output_rate_labels[] = { "the machine's own", "32000 Hz", "44100 Hz", "48000 Hz" };
#define RATE_CHOICES ((int)(sizeof(output_rates) / sizeof(output_rates[0])))

static int output_rate_index(const char *word)
{
	for (int n = 0; n < RATE_CHOICES; n++)
		if (!strcmp(output_rate_words[n], word))
			return n;
	return -1;
}

static void usage(FILE *fp)
{
	fprintf(fp,
	        "usage: scgui [options] [file.mid ...]\n"
	        "  --model NAME        sc88pro (default when its ROMs are found), sc88, sc88vl, sc8850,\n"
	        "                      sc55mk2\n"
	        "  --rom PATH          a zip or directory with the ROM images\n"
	        "  --map sc55|sc88|sc88pro|sc8850\n"
	        "                      play every part from that instrument map\n"
	        "  --midi-rate BAUD    31250 (default), 38400, 0\n"
	        "  --rate native|32000|44100|48000\n"
	        "                      the rate to ask the output device for; native (the default) is\n"
	        "                      the machine's own\n"
	        "  --size 4|8          the window size: 4 the small panel, 8 twice as large\n"
	        "  --keep-settings     keep the machine's settings memory across sessions\n"
	        "  --no-cache          boot the firmware every time\n"
	        "  --no-audio          run without a sound card\n"
	        "  --tail N            seconds after a song's last event (default 4)\n");
}

static int parse_options(int argc, char **argv, options_t *o, GPtrArray *songs)
{
	for (int n = 1; n < argc; n++)
	{
		const char *a = argv[n];
		if (!strcmp(a, "--help") || !strcmp(a, "-h"))
		{
			usage(stdout);
			return 0;
		}
		else if (!strcmp(a, "--model") && n + 1 < argc)
			o->model = argv[++n];
		else if (!strcmp(a, "--rom") && n + 1 < argc)
			o->rom = argv[++n];
		else if (!strcmp(a, "--map") && n + 1 < argc)
		{
			const char *v = argv[++n];
			o->map = !strcmp(v, "sc55") ? SCEMU_MAP_SC55 : !strcmp(v, "sc88") ? SCEMU_MAP_SC88
			         : !strcmp(v, "sc88pro") ? SCEMU_MAP_SC88PRO : !strcmp(v, "sc8850") ? SCEMU_MAP_SC8850
			         : SCEMU_MAP_NATIVE;
		}
		else if (!strcmp(a, "--midi-rate") && n + 1 < argc)
			o->midi_rate = (uint32_t)atoi(argv[++n]);
		else if (!strcmp(a, "--rate") && n + 1 < argc)
		{
			int choice = output_rate_index(argv[++n]);
			if (choice < 0)
			{
				fprintf(stderr, "scgui: --rate takes native, 32000, 44100 or 48000\n");
				usage(stderr);
				return -1;
			}
			o->audio_rate = (unsigned)output_rates[choice];
		}
		else if (!strcmp(a, "--size") && n + 1 < argc)
			o->size = atoi(argv[++n]);
		else if (!strcmp(a, "--tail") && n + 1 < argc)
			o->tail = atof(argv[++n]);
		else if (!strcmp(a, "--keep-settings"))
			o->keep_settings = true;
		else if (!strcmp(a, "--no-cache"))
			o->no_cache = true;
		else if (!strcmp(a, "--no-audio"))
			o->no_audio = true;
		else if (a[0] == '-' && a[1])
		{
			fprintf(stderr, "scgui: unknown option %s\n", a);
			usage(stderr);
			return -1;
		}
		else
			g_ptr_array_add(songs, g_strdup(a));
	}
	return 1;
}

/* ---------------------------------------------------------------- the settings file */

static const char *const reset_words[MACHINE_RESET_COUNT] = { "none", "gm", "gs", "gm2", "sc88-single", "sc88-double" };
static const char *const map_words[] = { "native", "sc55", "sc88", "sc88pro", "sc8850" };
#define MAP_WORDS ((int)(sizeof(map_words) / sizeof(map_words[0])))

static int word_index(const char *const *words, int count, const char *word, int fallback)
{
	for (int n = 0; n < count; n++)
		if (!strcmp(words[n], word))
			return n;
	return fallback;
}

/* the rear COMPUTER switch, as the settings file spells it and as the unit
 * prints it; the SC-8850's fourth position is the one the others call Mac */
static const char *const computer_words[COMPUTER_POSITIONS] = { "midi", "pc1", "pc2", "mac" };
static const char *const computer_labels[COMPUTER_POSITIONS] = { "MIDI", "PC-1", "PC-2", "Mac" };

static scemu_computer_switch_t computer_position(const char *word)
{
	if (!strcmp(word, "usb"))
		return SCEMU_COMPUTER_MAC;
	return (scemu_computer_switch_t)word_index(computer_words, COMPUTER_POSITIONS, word, SCEMU_COMPUTER_MIDI);
}

static const char *computer_word(scemu_model_t model, scemu_computer_switch_t sw)
{
	return sw == SCEMU_COMPUTER_MAC && model == SCEMU_MODEL_SC8850 ? "usb" : computer_words[sw];
}

static const char *computer_label(scemu_model_t model, scemu_computer_switch_t sw)
{
	return sw == SCEMU_COMPUTER_MAC && model == SCEMU_MODEL_SC8850 ? "USB" : computer_labels[sw];
}

/* the entry of a fixed set nearest the file's number */
static int nearest_index(const int *values, int count, int want)
{
	int best = 0;
	for (int n = 1; n < count; n++)
		if (abs(values[n] - want) < abs(values[best] - want))
			best = n;
	return best;
}

static void options_from_config(const scgui_config_t *c, options_t *o)
{
	memset(o, 0, sizeof(*o));
	o->model = c->model[0] ? c->model : NULL;
	o->rom = c->rom[0] ? c->rom : NULL;
	o->size = c->size;
	for (int n = 0; n < MACHINE_SYSTEMS; n++)
		o->computer[n] = computer_position(c->computer[n]);
	o->map = (scemu_map_t)word_index(map_words, MAP_WORDS, c->map, SCEMU_MAP_NATIVE);
	o->midi_rate = (uint32_t)c->midi_rate;
	o->audio_rate = (unsigned)c->audio_rate;
	o->keep_settings = c->keep_settings;
	o->tail = c->tail;
}

static gboolean config_flush(gpointer user)
{
	app_t *app = user;
	app->config_timer = 0;
	if (app->config_file[0])
		config_save(&app->cfg, app->config_file);
	return G_SOURCE_REMOVE;
}

/* a moment after the last change, so the knob does not write a file per step */
static void config_touch(app_t *app)
{
	if (!app->config_timer)
		app->config_timer = g_timeout_add(1000, config_flush, app);
}

/* ---------------------------------------------------------------- playlist */

static void play_index(app_t *app, int index);
static void set_title(app_t *app);
static gboolean macro_done(gpointer user);
static void panel_rebuild(app_t *app, panel_model_t model, int size);

static const char *song_label(const char *path, char *buf, size_t size)
{
	snprintf(buf, size, "%s", session_base_name(path));
	return buf;
}

static void list_select(app_t *app, int index)
{
	if (!app->list)
		return;
	GtkListBoxRow *row = index >= 0 ? gtk_list_box_get_row_at_index(GTK_LIST_BOX(app->list), index) : NULL;
	gtk_list_box_select_row(GTK_LIST_BOX(app->list), row);
}

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user)
{
	app_t *app = user;
	play_index(app, gtk_list_box_row_get_index(row));
}

static void list_append(app_t *app, const char *path)
{
	if (!app->list)
		return;
	char buf[300];
	GtkWidget *label = gtk_label_new(song_label(path, buf, sizeof(buf)));
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
	gtk_widget_set_margin_start(label, 8);
	gtk_widget_set_margin_end(label, 8);
	gtk_widget_set_margin_top(label, 4);
	gtk_widget_set_margin_bottom(label, 4);
	gtk_list_box_append(GTK_LIST_BOX(app->list), label);
}

static void songs_add(app_t *app, const char *path)
{
	g_ptr_array_add(app->songs, g_strdup(path));
	list_append(app, path);
}

static void on_files_chosen(GObject *source, GAsyncResult *result, gpointer user)
{
	app_t *app = user;
	GListModel *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source), result, NULL);
	if (!files)
		return;
	bool was_empty = app->songs->len == 0;
	for (guint n = 0; n < g_list_model_get_n_items(files); n++)
	{
		GFile *f = g_list_model_get_item(files, n);
		char *path = g_file_get_path(f);
		if (path)
			songs_add(app, path);
		g_free(path);
		g_object_unref(f);
	}
	g_object_unref(files);
	if (was_empty && app->songs->len)
		play_index(app, 0);
}

static void on_add_clicked(GtkButton *b, gpointer user)
{
	app_t *app = user;
	GtkFileDialog *dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, "Add Standard MIDI Files");
	GtkFileFilter *filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, "Standard MIDI Files");
	gtk_file_filter_add_pattern(filter, "*.mid");
	gtk_file_filter_add_pattern(filter, "*.MID");
	gtk_file_filter_add_pattern(filter, "*.midi");
	GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	g_list_store_append(filters, filter);
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(app->playlist_window), NULL, on_files_chosen, app);
	g_object_unref(filters);
	g_object_unref(filter);
	g_object_unref(dialog);
}

static void on_prev_clicked(GtkButton *b, gpointer user)
{
	app_t *app = user;
	if (app->current > 0)
		play_index(app, app->current - 1);
}

static void on_next_clicked(GtkButton *b, gpointer user)
{
	app_t *app = user;
	if (app->current + 1 < (int)app->songs->len)
		play_index(app, app->current + 1);
}

static void on_pause_clicked(GtkButton *b, gpointer user)
{
	app_t *app = user;
	app->paused = !app->paused;
	machine_pause(app->mc, app->paused);
}

static void on_stop_clicked(GtkButton *b, gpointer user)
{
	app_t *app = user;
	machine_stop_song(app->mc);
	app->current = -1;
	list_select(app, -1);
}

static void on_clear_clicked(GtkButton *b, gpointer user)
{
	app_t *app = user;
	on_stop_clicked(b, user);
	g_ptr_array_set_size(app->songs, 0);
	if (app->list)
		gtk_list_box_remove_all(GTK_LIST_BOX(app->list));
}

static gboolean on_playlist_close(GtkWindow *w, gpointer user)
{
	app_t *app = user;
	gtk_widget_set_visible(GTK_WIDGET(w), FALSE);
	(void)app;
	return TRUE;
}

/* ---------------------------------------------------------------- MIDI ports */

static void midi_apply(app_t *app, int slot)
{
	int choice = app->midi_choice[slot];
	if (midi_slot_is_input[slot])
		machine_midi_input(app->mc, slot, choice >= 0 ? app->ports[choice].in_id : -1);
	else
		machine_midi_output(app->mc, slot - MIDI_IO_INPUT_COUNT, choice >= 0 ? app->ports[choice].out_id : -1);
	snprintf(app->cfg.midi[slot], sizeof(app->cfg.midi[slot]), "%s", choice >= 0 ? app->ports[choice].name : "");
	config_touch(app);
}

static void on_midi_selected(GObject *drop, GParamSpec *spec, gpointer user)
{
	app_t *app = user;
	int slot = GPOINTER_TO_INT(g_object_get_data(drop, "slot"));
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(drop));
	int choice = -1;
	if (sel != GTK_INVALID_LIST_POSITION && sel > 0)
	{
		/* the list holds "none" then the ports that fit the slot, in order */
		guint seen = 0;
		for (int n = 0; n < app->port_count; n++)
		{
			bool fits = midi_slot_is_input[slot] ? app->ports[n].readable : app->ports[n].writable;
			if (fits && ++seen == sel)
			{
				choice = n;
				break;
			}
		}
	}
	if (choice != app->midi_choice[slot])
	{
		app->midi_choice[slot] = choice;
		midi_apply(app, slot);
	}
}

static void midi_fill(app_t *app)
{
	app->port_count = machine_midi_list(app->mc, app->ports, MIDI_PORTS_MAX);
	for (int slot = 0; slot < MIDI_SLOTS; slot++)
	{
		GtkStringList *list = gtk_string_list_new(NULL);
		gtk_string_list_append(list, "none");
		guint selected = 0, seen = 0;
		for (int n = 0; n < app->port_count; n++)
		{
			bool fits = midi_slot_is_input[slot] ? app->ports[n].readable : app->ports[n].writable;
			if (!fits)
				continue;
			gtk_string_list_append(list, app->ports[n].name);
			seen++;
			if (n == app->midi_choice[slot])
				selected = seen;
		}
		g_signal_handlers_block_by_func(app->midi_drop[slot], on_midi_selected, app);
		gtk_drop_down_set_model(GTK_DROP_DOWN(app->midi_drop[slot]), G_LIST_MODEL(list));
		gtk_drop_down_set_selected(GTK_DROP_DOWN(app->midi_drop[slot]), selected);
		g_signal_handlers_unblock_by_func(app->midi_drop[slot], on_midi_selected, app);
		g_object_unref(list);
	}
}

/* the rows for port groups C and D come and go with the machine that has them */
static void midi_rows_show(app_t *app)
{
	bool four = machine_midi_ports(app->mc) == 4;
	for (int slot = 0; slot < MIDI_SLOTS; slot++)
	{
		if (!app->midi_label[slot])
			continue;
		gtk_widget_set_visible(app->midi_label[slot], four || !midi_slot_is_extra[slot]);
		gtk_widget_set_visible(app->midi_drop[slot], four || !midi_slot_is_extra[slot]);
	}
}

static void on_midi_refresh(GtkButton *b, gpointer user)
{
	app_t *app = user;
	/* a port that is gone drops to none; the rest keep their choice by name */
	midi_port_info_t old[MIDI_PORTS_MAX];
	int old_choice[MIDI_SLOTS];
	memcpy(old, app->ports, sizeof(old));
	memcpy(old_choice, app->midi_choice, sizeof(old_choice));
	machine_midi_rescan(app->mc);
	int count = machine_midi_list(app->mc, app->ports, MIDI_PORTS_MAX);
	for (int slot = 0; slot < MIDI_SLOTS; slot++)
	{
		app->midi_choice[slot] = -1;
		if (old_choice[slot] < 0)
			continue;
		for (int n = 0; n < count; n++)
			if (strcmp(app->ports[n].name, old[old_choice[slot]].name) == 0)
				app->midi_choice[slot] = n;
		if (app->midi_choice[slot] != old_choice[slot])
			midi_apply(app, slot);
	}
	midi_fill(app);
}

static void on_reset_selected(GObject *drop, GParamSpec *spec, gpointer user)
{
	app_t *app = user;
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(drop));
	if (sel < MACHINE_RESET_COUNT)
	{
		app->reset = (machine_reset_t)sel;
		machine_set_reset(app->mc, app->reset);
		snprintf(app->cfg.reset, sizeof(app->cfg.reset), "%s", reset_words[app->reset]);
		config_touch(app);
	}
}

static void on_map_selected(GObject *drop, GParamSpec *spec, gpointer user)
{
	app_t *app = user;
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(drop));
	if (sel <= SCEMU_MAP_SC8850)
	{
		app->map = (scemu_map_t)sel;
		machine_set_map(app->mc, app->map);
		snprintf(app->cfg.map, sizeof(app->cfg.map), "%s", map_words[app->map]);
		config_touch(app);
	}
}

static GtkWidget *settings_grid(void)
{
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
	gtk_widget_set_margin_start(grid, 8);
	gtk_widget_set_margin_end(grid, 8);
	gtk_widget_set_margin_top(grid, 8);
	gtk_widget_set_margin_bottom(grid, 8);
	return grid;
}

static GtkWidget *grid_row(GtkWidget *grid, int row, const char *name, GtkWidget *widget)
{
	GtkWidget *label = gtk_label_new(name);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
	gtk_widget_set_hexpand(widget, TRUE);
	gtk_grid_attach(GTK_GRID(grid), widget, 1, row, 1, 1);
	return label;
}

static GtkWidget *midi_drop(app_t *app, int slot)
{
	app->midi_drop[slot] = gtk_drop_down_new(NULL, NULL);
	g_object_set_data(G_OBJECT(app->midi_drop[slot]), "slot", GINT_TO_POINTER(slot));
	g_signal_connect(app->midi_drop[slot], "notify::selected", G_CALLBACK(on_midi_selected), app);
	return app->midi_drop[slot];
}

/* the ports a user looks for first, always in view */
static GtkWidget *midi_section(app_t *app)
{
	GtkWidget *grid = settings_grid();
	for (int slot = 0; slot <= MIDI_IO_INPUT_COUNT; slot++)
		app->midi_label[slot] = grid_row(grid, slot, midi_slot_names[slot], midi_drop(app, slot));
	GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
	gtk_widget_set_tooltip_text(refresh, "Look for ports again");
	gtk_widget_set_valign(refresh, GTK_ALIGN_START);
	g_signal_connect(refresh, "clicked", G_CALLBACK(on_midi_refresh), app);
	gtk_grid_attach(GTK_GRID(grid), refresh, 2, 0, 1, 1);
	return grid;
}

/* the rest, behind the toolbar's toggle */
static GtkWidget *more_section(app_t *app)
{
	GtkWidget *grid = settings_grid();
	const int first = MIDI_IO_INPUT_COUNT + 1;
	for (int slot = first; slot < MIDI_SLOTS; slot++)
		app->midi_label[slot] = grid_row(grid, slot - first, midi_slot_names[slot], midi_drop(app, slot));

	GtkStringList *resets = gtk_string_list_new(machine_reset_names);
	GtkWidget *reset = gtk_drop_down_new(G_LIST_MODEL(resets), NULL);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(reset), app->reset);
	g_signal_connect(reset, "notify::selected", G_CALLBACK(on_reset_selected), app);
	grid_row(grid, MIDI_IO_OUTPUT_COUNT - 1, "Before each song", reset);

	static const char *const map_names[] = { "as the song selects", "SC-55 map", "SC-88 map", "SC-88Pro map", "SC-8850 map", NULL };
	GtkStringList *maps = gtk_string_list_new(map_names);
	GtkWidget *map = gtk_drop_down_new(G_LIST_MODEL(maps), NULL);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(map), app->map);
	g_signal_connect(map, "notify::selected", G_CALLBACK(on_map_selected), app);
	grid_row(grid, MIDI_IO_OUTPUT_COUNT, "Instrument map", map);

	app->more = gtk_revealer_new();
	gtk_revealer_set_child(GTK_REVEALER(app->more), grid);
	gtk_revealer_set_reveal_child(GTK_REVEALER(app->more), FALSE);
	return app->more;
}

static void on_more_toggled(GtkToggleButton *b, gpointer user)
{
	app_t *app = user;
	gtk_revealer_set_reveal_child(GTK_REVEALER(app->more), gtk_toggle_button_get_active(b));
}

static GtkWidget *toolbar_button(const char *icon, const char *tip, GCallback cb, app_t *app)
{
	GtkWidget *b = gtk_button_new_from_icon_name(icon);
	gtk_widget_set_tooltip_text(b, tip);
	g_signal_connect(b, "clicked", cb, app);
	return b;
}

static void playlist_show(app_t *app)
{
	if (!app->playlist_window)
	{
		GtkWidget *w = gtk_window_new();
		gtk_window_set_title(GTK_WINDOW(w), "Playlist");
		gtk_window_set_default_size(GTK_WINDOW(w), 420, 480);
		gtk_window_set_transient_for(GTK_WINDOW(w), GTK_WINDOW(app->window));
		gtk_window_set_hide_on_close(GTK_WINDOW(w), TRUE);
		g_signal_connect(w, "close-request", G_CALLBACK(on_playlist_close), app);

		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
		GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
		gtk_widget_set_margin_start(bar, 6);
		gtk_widget_set_margin_end(bar, 6);
		gtk_widget_set_margin_top(bar, 6);
		gtk_widget_set_margin_bottom(bar, 6);
		gtk_box_append(GTK_BOX(bar), toolbar_button("list-add-symbolic", "Add files", G_CALLBACK(on_add_clicked), app));
		gtk_box_append(GTK_BOX(bar), toolbar_button("media-skip-backward-symbolic", "Previous", G_CALLBACK(on_prev_clicked), app));
		gtk_box_append(GTK_BOX(bar), toolbar_button("media-playback-pause-symbolic", "Pause / resume", G_CALLBACK(on_pause_clicked), app));
		gtk_box_append(GTK_BOX(bar), toolbar_button("media-playback-stop-symbolic", "Stop", G_CALLBACK(on_stop_clicked), app));
		gtk_box_append(GTK_BOX(bar), toolbar_button("media-skip-forward-symbolic", "Next", G_CALLBACK(on_next_clicked), app));
		GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
		gtk_widget_set_hexpand(spacer, TRUE);
		gtk_box_append(GTK_BOX(bar), spacer);
		gtk_box_append(GTK_BOX(bar), toolbar_button("edit-clear-all-symbolic", "Clear the list", G_CALLBACK(on_clear_clicked), app));
		GtkWidget *more = gtk_toggle_button_new();
		gtk_button_set_icon_name(GTK_BUTTON(more), "emblem-system-symbolic");
		gtk_widget_set_tooltip_text(more, "More settings: the song's own outputs, the reset before each song, the instrument map");
		g_signal_connect(more, "toggled", G_CALLBACK(on_more_toggled), app);
		gtk_box_append(GTK_BOX(bar), more);
		gtk_box_append(GTK_BOX(box), bar);

		app->list = gtk_list_box_new();
		gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->list), GTK_SELECTION_SINGLE);
		g_signal_connect(app->list, "row-activated", G_CALLBACK(on_row_activated), app);
		for (guint n = 0; n < app->songs->len; n++)
			list_append(app, g_ptr_array_index(app->songs, n));
		list_select(app, app->current);
		GtkWidget *scroll = gtk_scrolled_window_new();
		gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), app->list);
		gtk_widget_set_vexpand(scroll, TRUE);
		gtk_box_append(GTK_BOX(box), scroll);
		gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
		gtk_box_append(GTK_BOX(box), midi_section(app));
		gtk_box_append(GTK_BOX(box), more_section(app));
		midi_fill(app);
		midi_rows_show(app);
		gtk_window_set_child(GTK_WINDOW(w), box);
		app->playlist_window = w;
	}
	gtk_window_present(GTK_WINDOW(app->playlist_window));
}

static void play_index(app_t *app, int index)
{
	if (index < 0 || index >= (int)app->songs->len)
		return;
	app->current = index;
	app->paused = false;
	machine_play(app->mc, g_ptr_array_index(app->songs, index));
	list_select(app, index);
}

/* ---------------------------------------------------------------- audio settings */

static const int block_sizes[] = { 64, 128, 256, 512, 1024 };
#define BLOCK_CHOICES ((int)(sizeof(block_sizes) / sizeof(block_sizes[0])))

static void audio_readout(app_t *app)
{
	if (!app->audio_label)
		return;
	char text[384];
	snprintf(text, sizeof(text), "Output: %s\nLatency: %.0f ms from the machine to the jack\nUnderruns so far: %u",
	         app->state.audio, app->state.latency * 1000, app->state.underruns);
	gtk_label_set_text(GTK_LABEL(app->audio_label), text);
}

static void audio_apply(app_t *app)
{
	int device = app->audio_choice >= 0 ? app->devices[app->audio_choice].index : -1;
	machine_set_audio(app->mc, device, (unsigned)block_sizes[app->block_choice],
	                  (unsigned)output_rates[app->rate_choice]);
	snprintf(app->cfg.audio_device, sizeof(app->cfg.audio_device), "%s",
	         app->audio_choice >= 0 ? app->devices[app->audio_choice].name : "");
	app->cfg.audio_rate = output_rates[app->rate_choice];
	app->cfg.audio_block = block_sizes[app->block_choice];
	config_touch(app);
}

static void on_audio_selected(GObject *drop, GParamSpec *spec, gpointer user)
{
	app_t *app = user;
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(drop));
	int choice = sel == GTK_INVALID_LIST_POSITION || sel == 0 ? -1 : (int)sel - 1;
	if (choice != app->audio_choice)
	{
		app->audio_choice = choice;
		audio_apply(app);
	}
}

static void on_rate_selected(GObject *drop, GParamSpec *spec, gpointer user)
{
	app_t *app = user;
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(drop));
	if (sel < (guint)RATE_CHOICES && (int)sel != app->rate_choice)
	{
		app->rate_choice = (int)sel;
		audio_apply(app);
	}
}

static void on_block_selected(GObject *drop, GParamSpec *spec, gpointer user)
{
	app_t *app = user;
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(drop));
	if (sel < (guint)BLOCK_CHOICES && (int)sel != app->block_choice)
	{
		app->block_choice = (int)sel;
		audio_apply(app);
	}
}

/* the list holds "default" then every output device; a choice survives by name */
static void audio_fill(app_t *app)
{
	char chosen[128] = "";
	if (app->audio_choice >= 0)
		snprintf(chosen, sizeof(chosen), "%s", app->devices[app->audio_choice].name);
	app->device_count = audio_list(app->devices, AUDIO_DEVICES_MAX);
	GtkStringList *list = gtk_string_list_new(NULL);
	gtk_string_list_append(list, "default");
	guint selected = 0;
	app->audio_choice = -1;
	for (int n = 0; n < app->device_count; n++)
	{
		gtk_string_list_append(list, app->devices[n].name);
		if (chosen[0] && strcmp(chosen, app->devices[n].name) == 0)
		{
			app->audio_choice = n;
			selected = (guint)n + 1;
		}
	}
	g_signal_handlers_block_by_func(app->audio_drop, on_audio_selected, app);
	gtk_drop_down_set_model(GTK_DROP_DOWN(app->audio_drop), G_LIST_MODEL(list));
	gtk_drop_down_set_selected(GTK_DROP_DOWN(app->audio_drop), selected);
	g_signal_handlers_unblock_by_func(app->audio_drop, on_audio_selected, app);
	g_object_unref(list);
}

static void on_audio_refresh(GtkButton *b, gpointer user)
{
	app_t *app = user;
	bool had = app->audio_choice >= 0;
	audio_fill(app);
	if (had && app->audio_choice < 0)
		audio_apply(app);
}

static void on_notches_changed(GtkSpinButton *spin, gpointer user)
{
	app_t *app = user;
	int notches = gtk_spin_button_get_value_as_int(spin);
	if (notches == app->cfg.knob_notches)
		return;
	app->cfg.knob_notches = notches;
	controls_set_knob_notches(&app->ctl, notches);
	config_touch(app);
}

static GtkWidget *audio_page(app_t *app)
{
	GtkWidget *grid = settings_grid();
	app->audio_drop = gtk_drop_down_new(NULL, NULL);
	g_signal_connect(app->audio_drop, "notify::selected", G_CALLBACK(on_audio_selected), app);
	grid_row(grid, 0, "Output device", app->audio_drop);
	GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
	gtk_widget_set_tooltip_text(refresh, "Look for devices again");
	g_signal_connect(refresh, "clicked", G_CALLBACK(on_audio_refresh), app);
	gtk_grid_attach(GTK_GRID(grid), refresh, 2, 0, 1, 1);

	GtkStringList *rates = gtk_string_list_new(NULL);
	for (int n = 0; n < RATE_CHOICES; n++)
		gtk_string_list_append(rates, output_rate_labels[n]);
	app->rate_drop = gtk_drop_down_new(G_LIST_MODEL(rates), NULL);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(app->rate_drop), app->rate_choice);
	gtk_widget_set_tooltip_text(app->rate_drop, "The rate the device is asked for; the machine's output is"
	                                            " converted to whatever it opens at");
	g_signal_connect(app->rate_drop, "notify::selected", G_CALLBACK(on_rate_selected), app);
	grid_row(grid, 1, "Output rate", app->rate_drop);

	GtkStringList *blocks = gtk_string_list_new(NULL);
	for (int n = 0; n < BLOCK_CHOICES; n++)
	{
		char text[64];
		snprintf(text, sizeof(text), "%d frames, %.0f ms", block_sizes[n], block_sizes[n] * 1000.0 / 32000);
		gtk_string_list_append(blocks, text);
	}
	app->block_drop = gtk_drop_down_new(G_LIST_MODEL(blocks), NULL);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(app->block_drop), app->block_choice);
	g_signal_connect(app->block_drop, "notify::selected", G_CALLBACK(on_block_selected), app);
	grid_row(grid, 2, "Buffer", app->block_drop);

	GtkWidget *notches = gtk_spin_button_new_with_range(5, 200, 5);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(notches), app->cfg.knob_notches);
	gtk_widget_set_tooltip_text(notches, "Notches of the scroll wheel that take the volume knob from silent to full");
	g_signal_connect(notches, "value-changed", G_CALLBACK(on_notches_changed), app);
	grid_row(grid, 3, "Knob travel, in notches", notches);

	app->audio_label = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(app->audio_label), 0);
	gtk_widget_set_margin_top(app->audio_label, 8);
	gtk_grid_attach(GTK_GRID(grid), app->audio_label, 0, 4, 3, 1);
	audio_fill(app);
	audio_readout(app);
	return grid;
}

/* ---------------------------------------------------------------- the interface */

#define SIZE_SMALL 4
#define SIZE_LARGE 8

static void on_size_selected(GObject *drop, GParamSpec *spec, gpointer user)
{
	app_t *app = user;
	(void)spec;
	int size = gtk_drop_down_get_selected(GTK_DROP_DOWN(drop)) ? SIZE_LARGE : SIZE_SMALL;
	if (size == app->size)
		return;
	app->cfg.size = size;
	panel_rebuild(app, panel_model(app->panel), size);
	config_touch(app);
}

static void on_swap_toggled(GtkCheckButton *b, gpointer user)
{
	app_t *app = user;
	bool swap = gtk_check_button_get_active(b);
	if (swap == app->cfg.swap_buttons)
		return;
	app->cfg.swap_buttons = swap;
	controls_set_swap_buttons(&app->ctl, swap);
	config_touch(app);
}

static GtkWidget *interface_page(app_t *app)
{
	GtkWidget *grid = settings_grid();
	GtkStringList *sizes = gtk_string_list_new(NULL);
	gtk_string_list_append(sizes, "Small");
	gtk_string_list_append(sizes, "Large, twice the size");
	app->size_drop = gtk_drop_down_new(G_LIST_MODEL(sizes), NULL);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(app->size_drop), app->size >= SIZE_LARGE ? 1 : 0);
	gtk_widget_set_tooltip_text(app->size_drop, "The panel is drawn from artwork baked at two sizes;"
	                                            " the window follows at once");
	g_signal_connect(app->size_drop, "notify::selected", G_CALLBACK(on_size_selected), app);
	grid_row(grid, 0, "Panel size", app->size_drop);

	GtkWidget *swap_label = gtk_label_new("Swap the right and middle mouse buttons: the combination menu"
	                                      " opens on a right-click, and the right button's queue-and-hold"
	                                      " gesture moves to the middle one");
	gtk_label_set_xalign(GTK_LABEL(swap_label), 0);
	gtk_label_set_wrap(GTK_LABEL(swap_label), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(swap_label), 52);
	app->swap_check = gtk_check_button_new();
	gtk_check_button_set_child(GTK_CHECK_BUTTON(app->swap_check), swap_label);
	gtk_check_button_set_active(GTK_CHECK_BUTTON(app->swap_check), app->cfg.swap_buttons);
	g_signal_connect(app->swap_check, "toggled", G_CALLBACK(on_swap_toggled), app);
	gtk_grid_attach(GTK_GRID(grid), app->swap_check, 0, 1, 3, 1);
	return grid;
}

/* ---------------------------------------------------------------- system settings */

static void system_readout(app_t *app)
{
	if (!app->system_label)
		return;
	machine_rom_info_t info;
	machine_rom_info(app->mc, &info);
	char text[900];
	size_t at = (size_t)snprintf(text, sizeof(text), "Running: %s, control ROM %s\nROMs: %s",
	                             info.label, info.version[0] ? info.version : "unknown", info.origin);
	if (app->state.error[0] && at < sizeof(text))
		snprintf(text + at, sizeof(text) - at, "\n%s", app->state.error);
	gtk_label_set_text(GTK_LABEL(app->system_label), text);
	app->system_updating = true;
	for (int n = 0; n < MACHINE_SYSTEMS; n++)
		if (machine_systems[n] == info.model)
			gtk_check_button_set_active(GTK_CHECK_BUTTON(app->model_check[n]), TRUE);
	int row = machine_system_index(info.model);
	scemu_computer_switch_t sw = row < 0 ? SCEMU_COMPUTER_MIDI : computer_position(app->cfg.computer[row]);
	for (int n = 0; n < COMPUTER_POSITIONS; n++)
	{
		gtk_check_button_set_label(GTK_CHECK_BUTTON(app->computer_check[n]),
		                           computer_label(info.model, (scemu_computer_switch_t)n));
		if (n == (int)sw)
			gtk_check_button_set_active(GTK_CHECK_BUTTON(app->computer_check[n]), TRUE);
	}
	app->system_updating = false;
	midi_rows_show(app);
}

static void on_model_toggled(GtkCheckButton *b, gpointer user)
{
	app_t *app = user;
	if (app->system_updating || !gtk_check_button_get_active(b))
		return;
	scemu_model_t model = (scemu_model_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "model"));
	if (model == machine_model(app->mc))
		return;
	machine_set_model(app->mc, model);
	snprintf(app->cfg.model, sizeof(app->cfg.model), "%s", scplay_model_name(model));
	config_touch(app);
}

static void on_computer_toggled(GtkCheckButton *b, gpointer user)
{
	app_t *app = user;
	if (app->system_updating || !gtk_check_button_get_active(b))
		return;
	scemu_computer_switch_t sw = (scemu_computer_switch_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "position"));
	scemu_model_t model = machine_model(app->mc);
	int row = machine_system_index(model);
	if (row < 0 || sw == computer_position(app->cfg.computer[row]))
		return;
	snprintf(app->cfg.computer[row], sizeof(app->cfg.computer[row]), "%s", computer_word(model, sw));
	config_touch(app);
	machine_set_computer_switch(app->mc, sw);
}

static void on_clear_cache(GtkButton *button, gpointer user)
{
	app_t *app = user;
	(void)button;
	int gone = session_clear_cache();
	char text[80];
	if (gone < 0)
		snprintf(text, sizeof(text), "No cache to clear");
	else
		snprintf(text, sizeof(text), gone == 1 ? "%d file cleared" : "%d files cleared", gone);
	gtk_label_set_text(GTK_LABEL(app->cache_label), text);
}

static GtkWidget *system_page(app_t *app)
{
	GtkWidget *grid = settings_grid();
	unsigned have = machine_models_available(app->mc);
	scemu_model_t model = machine_model(app->mc);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	app->system_updating = true;
	for (int n = 0; n < MACHINE_SYSTEMS; n++)
	{
		GtkWidget *b = gtk_check_button_new_with_label(scplay_model_label(machine_systems[n]));
		app->model_check[n] = b;
		if (n)
			gtk_check_button_set_group(GTK_CHECK_BUTTON(b), GTK_CHECK_BUTTON(app->model_check[0]));
		if (machine_systems[n] == model)
			gtk_check_button_set_active(GTK_CHECK_BUTTON(b), TRUE);
		if (!(have & (1u << machine_systems[n])))
		{
			gtk_widget_set_sensitive(b, FALSE);
			gtk_widget_set_tooltip_text(b, "ROM images not found");
		}
		g_object_set_data(G_OBJECT(b), "model", GINT_TO_POINTER((int)machine_systems[n]));
		g_signal_connect(b, "toggled", G_CALLBACK(on_model_toggled), app);
		gtk_box_append(GTK_BOX(box), b);
	}
	app->system_updating = false;
	grid_row(grid, 0, "System", box);

	GtkWidget *positions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	app->system_updating = true;
	for (int n = 0; n < COMPUTER_POSITIONS; n++)
	{
		GtkWidget *b = gtk_check_button_new_with_label(computer_label(model, (scemu_computer_switch_t)n));
		app->computer_check[n] = b;
		if (n)
			gtk_check_button_set_group(GTK_CHECK_BUTTON(b), GTK_CHECK_BUTTON(app->computer_check[0]));
		g_object_set_data(G_OBJECT(b), "position", GINT_TO_POINTER(n));
		g_signal_connect(b, "toggled", G_CALLBACK(on_computer_toggled), app);
		gtk_box_append(GTK_BOX(positions), b);
	}
	app->system_updating = false;
	gtk_widget_set_tooltip_text(positions, "The switch on the back, read by the firmware when it comes up:"
	                                       " changing it boots the machine again");
	grid_row(grid, 1, "Computer switch", positions);

	app->system_label = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(app->system_label), 0);
	gtk_label_set_wrap(GTK_LABEL(app->system_label), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(app->system_label), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_max_width_chars(GTK_LABEL(app->system_label), 60);
	gtk_widget_set_margin_top(app->system_label, 8);
	gtk_widget_set_margin_bottom(app->system_label, 8);
	gtk_grid_attach(GTK_GRID(grid), app->system_label, 0, 2, 3, 1);

	GtkWidget *cache = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *clear = gtk_button_new_with_label("Clear cache");
	gtk_widget_set_halign(clear, GTK_ALIGN_START);
	g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_cache), app);
	app->cache_label = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(app->cache_label), 0);
	gtk_box_append(GTK_BOX(cache), clear);
	gtk_box_append(GTK_BOX(cache), app->cache_label);
	gtk_widget_set_tooltip_text(cache, "Throws away the boot snapshots and factory settings kept in"
	                                   " ~/.cache/scemu: the firmware boots again the next time a machine"
	                                   " starts.  What a machine remembers is not touched.");
	grid_row(grid, 3, "Boot cache", cache);
	system_readout(app);
	return grid;
}

/* ---------------------------------------------------------------- the settings window */

#define SETTINGS_TAB_AUDIO 0
#define SETTINGS_TAB_INTERFACE 1
#define SETTINGS_TAB_SYSTEM 2

static void settings_show(app_t *app, int tab)
{
	if (!app->settings_window)
	{
		GtkWidget *w = gtk_window_new();
		gtk_window_set_title(GTK_WINDOW(w), "Settings");
		gtk_window_set_transient_for(GTK_WINDOW(w), GTK_WINDOW(app->window));
		gtk_window_set_hide_on_close(GTK_WINDOW(w), TRUE);
		gtk_window_set_default_size(GTK_WINDOW(w), 480, -1);

		app->notebook = gtk_notebook_new();
		gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), audio_page(app), gtk_label_new("Audio"));
		gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), interface_page(app), gtk_label_new("Interface"));
		gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), system_page(app), gtk_label_new("System"));
		gtk_window_set_child(GTK_WINDOW(w), app->notebook);
		app->settings_window = w;
	}
	gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), tab);
	gtk_window_present(GTK_WINDOW(app->settings_window));
}

/* ---------------------------------------------------------------- the logo menu */

static void about_show(app_t *app)
{
	machine_rom_info_t info;
	machine_rom_info(app->mc, &info);
	char text[900];
	snprintf(text, sizeof(text), "%s, control ROM %s\nROMs: %s\nRendering at %u Hz",
	         info.label, info.version[0] ? info.version : "unknown", info.origin, info.rate);
	GtkWidget *d = gtk_about_dialog_new();
	gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(d), "SoundCarcass");
	gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(d), GIT_VERSION);
	gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(d), text);
	gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(d), GTK_LICENSE_BSD_3);
	gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(app->window));
	gtk_window_set_modal(GTK_WINDOW(d), TRUE);
	gtk_window_present(GTK_WINDOW(d));
}

static void on_menu_open(GSimpleAction *a, GVariant *parameter, gpointer user)
{
	app_t *app = user;
	const char *what = g_action_get_name(G_ACTION(a));
	if (!strcmp(what, "playlist"))
		playlist_show(app);
	else if (!strcmp(what, "audio"))
		settings_show(app, SETTINGS_TAB_AUDIO);
	else if (!strcmp(what, "interface"))
		settings_show(app, SETTINGS_TAB_INTERFACE);
	else if (!strcmp(what, "system"))
		settings_show(app, SETTINGS_TAB_SYSTEM);
	else
		about_show(app);
}

static void on_menu_send_reset(GSimpleAction *a, GVariant *parameter, gpointer user)
{
	app_t *app = user;
	size_t size;
	const uint8_t *msg = machine_reset_message((machine_reset_t)g_variant_get_int32(parameter), &size);
	if (msg)
		machine_send(app->mc, msg, size);
}

static void on_menu_send_map(GSimpleAction *a, GVariant *parameter, gpointer user)
{
	app_t *app = user;
	uint8_t bytes[128];
	size_t n = scemu_map_selection((scemu_map_t)g_variant_get_int32(parameter), bytes, sizeof(bytes));
	if (n)
		machine_send(app->mc, bytes, n);
}

static void menu_append(GMenu *menu, const char *label, const char *action, int parameter)
{
	GMenuItem *item = g_menu_item_new(label, NULL);
	g_menu_item_set_action_and_target_value(item, action, parameter < 0 ? NULL : g_variant_new_int32(parameter));
	g_menu_append_item(menu, item);
	g_object_unref(item);
}

static void logo_menu(app_t *app, double x, double y)
{
	if (!app->logo_popover)
	{
		static const GActionEntry entries[] =
		{
			{ "playlist", on_menu_open, NULL, NULL, NULL, { 0 } },
			{ "audio", on_menu_open, NULL, NULL, NULL, { 0 } },
			{ "interface", on_menu_open, NULL, NULL, NULL, { 0 } },
			{ "system", on_menu_open, NULL, NULL, NULL, { 0 } },
			{ "about", on_menu_open, NULL, NULL, NULL, { 0 } },
			{ "send-reset", on_menu_send_reset, "i", NULL, NULL, { 0 } },
			{ "send-map", on_menu_send_map, "i", NULL, NULL, { 0 } },
		};
		GSimpleActionGroup *group = g_simple_action_group_new();
		g_action_map_add_action_entries(G_ACTION_MAP(group), entries, G_N_ELEMENTS(entries), app);
		gtk_widget_insert_action_group(app->area, "logo", G_ACTION_GROUP(group));
		g_object_unref(group);

		GMenu *send = g_menu_new();
		for (int r = MACHINE_RESET_GM; r < MACHINE_RESET_COUNT; r++)
			menu_append(send, machine_reset_names[r], "logo.send-reset", r);
		GMenu *maps = g_menu_new();
		static const char *const map_names[] = { NULL, "SC-55 map", "SC-88 map", "SC-88Pro map", "SC-8850 map" };
		for (int m = SCEMU_MAP_SC55; m <= SCEMU_MAP_SC8850; m++)
		{
			char label[64];
			snprintf(label, sizeof(label), "Set all parts to the %s", map_names[m]);
			menu_append(maps, label, "logo.send-map", m);
		}
		g_menu_append_section(send, NULL, G_MENU_MODEL(maps));
		g_object_unref(maps);

		GMenu *menu = g_menu_new();
		GMenu *windows = g_menu_new();
		menu_append(windows, "Playlist and MIDI", "logo.playlist", -1);
		menu_append(windows, "Audio", "logo.audio", -1);
		menu_append(windows, "Interface", "logo.interface", -1);
		menu_append(windows, "System", "logo.system", -1);
		g_menu_append_submenu(windows, "Send", G_MENU_MODEL(send));
		g_menu_append_section(menu, NULL, G_MENU_MODEL(windows));
		GMenu *about = g_menu_new();
		menu_append(about, "About SoundCarcass", "logo.about", -1);
		g_menu_append_section(menu, NULL, G_MENU_MODEL(about));
		g_object_unref(send);
		g_object_unref(windows);
		g_object_unref(about);

		app->logo_popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
		g_object_unref(menu);
		gtk_widget_set_parent(app->logo_popover, app->area);
		gtk_popover_set_has_arrow(GTK_POPOVER(app->logo_popover), FALSE);
	}
	GdkRectangle at = { (int)x, (int)y, 1, 1 };
	gtk_popover_set_pointing_to(GTK_POPOVER(app->logo_popover), &at);
	gtk_popover_popup(GTK_POPOVER(app->logo_popover));
}

/* the menu on the model name: the systems whose ROMs are there, and a reset */
static void on_menu_system(GSimpleAction *action, GVariant *parameter, gpointer user)
{
	app_t *app = user;
	scemu_model_t model = (scemu_model_t)g_variant_get_int32(parameter);
	g_simple_action_set_state(action, parameter);
	if (model == machine_model(app->mc))
		return;
	machine_set_model(app->mc, model);
	snprintf(app->cfg.model, sizeof(app->cfg.model), "%s", scplay_model_name(model));
	config_touch(app);
}

static void on_menu_reset(GSimpleAction *action, GVariant *parameter, gpointer user)
{
	app_t *app = user;
	controls_power_cycle(&app->ctl);
}

static void system_menu(app_t *app, double x, double y)
{
	if (!app->system_popover)
	{
		GSimpleActionGroup *group = g_simple_action_group_new();
		GSimpleAction *model = g_simple_action_new_stateful("model", G_VARIANT_TYPE_INT32,
		                                                    g_variant_new_int32((int)machine_model(app->mc)));
		g_signal_connect(model, "activate", G_CALLBACK(on_menu_system), app);
		g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(model));
		GSimpleAction *reset = g_simple_action_new("reset", NULL);
		g_signal_connect(reset, "activate", G_CALLBACK(on_menu_reset), app);
		g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(reset));
		gtk_widget_insert_action_group(app->area, "system", G_ACTION_GROUP(group));
		app->system_model_action = model;
		g_object_unref(reset);
		g_object_unref(group);

		GMenu *menu = g_menu_new();
		GMenu *models = g_menu_new();
		unsigned have = machine_models_available(app->mc);
		for (int n = 0; n < MACHINE_SYSTEMS; n++)
			if (have & (1u << machine_systems[n]))
				menu_append(models, scplay_model_label(machine_systems[n]), "system.model", (int)machine_systems[n]);
		g_menu_append_section(menu, NULL, G_MENU_MODEL(models));
		GMenu *actions = g_menu_new();
		menu_append(actions, "Reset", "system.reset", -1);
		g_menu_append_section(menu, NULL, G_MENU_MODEL(actions));
		g_object_unref(models);
		g_object_unref(actions);

		app->system_popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
		g_object_unref(menu);
		gtk_widget_set_parent(app->system_popover, app->area);
		gtk_popover_set_has_arrow(GTK_POPOVER(app->system_popover), FALSE);
	}
	g_simple_action_set_state(app->system_model_action, g_variant_new_int32((int)machine_model(app->mc)));
	GdkRectangle at = { (int)x, (int)y, 1, 1 };
	gtk_popover_set_pointing_to(GTK_POPOVER(app->system_popover), &at);
	gtk_popover_popup(GTK_POPOVER(app->system_popover));
}

/* ---------------------------------------------------------------- the panel */

static void draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user)
{
	app_t *app = user;
	int w = panel_width(app->panel), h = panel_height(app->panel);
	if (panel_dirty(app->panel))
		panel_render(app->panel, app->frame, (size_t)w);
	/* a fresh wrapper each time: the toolkit may keep a snapshot of the last one */
	cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *)app->frame, CAIRO_FORMAT_RGB24,
	                                                               w, h, w * (int)sizeof(uint32_t));
	cairo_surface_set_device_scale(surface, app->scale, app->scale);
	cairo_set_source_surface(cr, surface, 0, 0);
	cairo_paint(cr);
	cairo_surface_destroy(surface);
}

/* the bake for a panel at the size setting: the glass's dot pitch, 4 and 8 on
 * the 88 family, 3 and 6 on the SC-8850 */
static int panel_pitch_for(panel_model_t model, int size)
{
	return panel_sizes[model][size >= 8 ? 1 : 0].pitch;
}

/* what the panel shows of the machine: one of the two kinds of glass, and the lamps */
static void panel_glass(panel_t *p, const machine_state_t *st)
{
	if (st->has_glcd)
		panel_set_glcd(p, &st->glcd);
	else
		panel_set_lcd(p, &st->lcd);
	panel_set_leds(p, st->leds);
}

/* another model's panel, or the same one at the other size: the keys held come up with the old one */
static void panel_rebuild(app_t *app, panel_model_t model, int size)
{
	panel_t *p = panel_create(model, panel_pitch_for(model, size) * app->scale);
	if (!p)
		return;
	app->size = size;
	panel_destroy(app->panel);
	app->panel = p;
	int w = panel_width(p), h = panel_height(p);
	free(app->frame);
	app->frame = calloc((size_t)w * h, sizeof(uint32_t));
	controls_set_panel(&app->ctl, p);
	/* the glass and the lamps come with the snapshot, whose generation has
	 * already passed; give the new panel the last one */
	panel_glass(p, &app->state);
	gtk_widget_set_size_request(app->area, w / app->scale, h / app->scale);
	gtk_widget_queue_draw(app->area);
}

static void panel_switch(app_t *app, panel_model_t model)
{
	if (model != panel_model(app->panel))
		panel_rebuild(app, model, app->size);
}

static void set_title(app_t *app)
{
	char title[512];
	if (app->state.song[0])
		snprintf(title, sizeof(title), "%s%s%s — %s", app->state.title[0] ? app->state.title : app->state.song,
		         app->paused ? " (paused)" : "", app->ctl.power ? "" : " (off)", machine_model_label(app->mc));
	else
		snprintf(title, sizeof(title), "%s%s", machine_model_label(app->mc), app->ctl.power ? "" : " (off)");
	gtk_window_set_title(GTK_WINDOW(app->window), title);
}

static gboolean on_tick(gpointer user)
{
	app_t *app = user;
	machine_state_t st;
	machine_snapshot(app->mc, &st);
	if (st.generation != app->seen_generation)
	{
		bool finished_now = st.finished && !app->state.finished;
		app->seen_generation = st.generation;
		bool song_changed = strcmp(st.song, app->state.song) != 0 || strcmp(st.title, app->state.title) != 0
		                    || st.paused != app->state.paused;
		app->state = st;
		panel_glass(app->panel, &st);
		if (app->settings_window && gtk_widget_get_visible(app->settings_window))
		{
			audio_readout(app);
			system_readout(app);
		}
		if (song_changed)
			set_title(app);
		if (finished_now && app->current >= 0 && app->current + 1 < (int)app->songs->len)
			play_index(app, app->current + 1);
		if (!st.booting && st.power)
		{
			unsigned ms = controls_boot_done(&app->ctl);
			if (ms)
				g_timeout_add(ms, macro_done, app);
		}
	}
	scemu_model_t model = machine_model(app->mc);
	if (model != app->shown_model)
	{
		app->shown_model = model;
		set_title(app);
		system_readout(app);
		panel_switch(app, panel_model_for(model));
		controls_set_soft_power(&app->ctl, model == SCEMU_MODEL_SC55MK2);
	}
	panel_set_standby(app->panel, !app->ctl.power);
	if (panel_dirty(app->panel))
		gtk_widget_queue_draw(app->area);
	return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------- the panel's actions */

static void act_key(void *user, scemu_button_t b, bool down)
{
	app_t *app = user;
	machine_button(app->mc, b, down);
}

static void act_key_after(void *user, scemu_button_t b, bool down, unsigned ms)
{
	app_t *app = user;
	machine_button_after(app->mc, b, down, ms);
}

static void act_dial(void *user, int steps)
{
	app_t *app = user;
	machine_dial(app->mc, steps);
}

static void act_power(void *user, bool on)
{
	app_t *app = user;
	machine_power(app->mc, on);
	if (!on)
	{
		app->current = -1;
		list_select(app, -1);
	}
	set_title(app);
}

static void act_knob(void *user, float turn)
{
	app_t *app = user;
	machine_set_gain(app->mc, turn * turn);
	app->cfg.volume = turn;
	config_touch(app);
}

static void combo_menu(app_t *app, int element, double x, double y);

static void act_combo_menu(void *user, panel_element_t e, double x, double y)
{
	app_t *app = user;
	combo_menu(app, e, x / app->scale, y / app->scale);
}

static void act_element(void *user, panel_element_t e, double x, double y)
{
	app_t *app = user;
	switch (e)
	{
	case PANEL_JACK_MIDI_IN_B:
		playlist_show(app);
		break;
	case PANEL_JACK_PHONES:
		settings_show(app, SETTINGS_TAB_AUDIO);
		break;
	case PANEL_LOGO:
		logo_menu(app, x / app->scale, y / app->scale);
		break;
	case PANEL_LOGO_MODEL:
		system_menu(app, x / app->scale, y / app->scale);
		break;
	default:
		break;
	}
}

static const controls_actions_t actions = {
	act_key, act_key_after, act_dial, act_power, act_knob, act_element, act_combo_menu
};

/* ---------------------------------------------------------------- the combination menu */

static void on_combo_enter(GtkEventControllerMotion *m, double x, double y, gpointer user)
{
	app_t *app = user;
	GtkWidget *item = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(m));
	controls_highlight(&app->ctl, &combos[GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "combo"))], true);
}

static void on_combo_leave(GtkEventControllerMotion *m, gpointer user)
{
	app_t *app = user;
	GtkWidget *item = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(m));
	controls_highlight(&app->ctl, &combos[GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "combo"))], false);
}

static void on_combo_closed(GtkPopover *popover, gpointer user)
{
	app_t *app = user;
	controls_highlight_clear(&app->ctl);
}

static gboolean macro_done(gpointer user)
{
	app_t *app = user;
	controls_macro_done(&app->ctl);
	return G_SOURCE_REMOVE;
}

static void on_combo_chosen(GtkButton *b, gpointer user)
{
	app_t *app = user;
	const combo_t *c = &combos[GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "combo"))];
	gtk_popover_popdown(GTK_POPOVER(app->combo_popover));
	unsigned ms = controls_play_combo(&app->ctl, c);
	if (ms)
		g_timeout_add(ms, macro_done, app);
}

static void combo_menu(app_t *app, int element, double x, double y)
{
	int button = panel_element_button((panel_element_t)element);
	int count = combos_for(panel_model(app->panel), (scemu_button_t)button, app->combo_ids, 64);
	/* holding one half and pressing the other is a mouse gesture here, not a menu entry */
	int kept = 0;
	for (int n = 0; n < count; n++)
		if (strcmp(combos[app->combo_ids[n]].name, "the value will change faster") != 0)
			app->combo_ids[kept++] = app->combo_ids[n];
	count = kept;
	if (count == 0)
		return;
	if (!app->combo_popover)
	{
		app->combo_popover = gtk_popover_new();
		gtk_widget_set_parent(app->combo_popover, app->area);
		g_signal_connect(app->combo_popover, "closed", G_CALLBACK(on_combo_closed), app);
	}
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	for (int n = 0; n < count; n++)
	{
		const combo_t *c = &combos[app->combo_ids[n]];
		char keys[300], *name = g_markup_escape_text(c->name, -1);
		combo_text(c, keys, sizeof(keys));
		char *keys_markup = g_markup_escape_text(keys, -1);
		char *mode = g_markup_escape_text(c->mode ? c->mode : "any mode", -1);
		char *markup = g_strdup_printf("<b>%s</b>  <small><i>%s</i></small>\n<small>%s</small>", name, mode, keys_markup);
		g_free(mode);
		GtkWidget *label = gtk_label_new(NULL);
		gtk_label_set_markup(GTK_LABEL(label), markup);
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		g_free(markup);
		g_free(keys_markup);
		g_free(name);
		GtkWidget *item = gtk_button_new();
		gtk_button_set_child(GTK_BUTTON(item), label);
		gtk_button_set_has_frame(GTK_BUTTON(item), FALSE);
		char tip[400];
		snprintf(tip, sizeof(tip), "%s  (%s)", c->effect, c->page);
		gtk_widget_set_tooltip_text(item, tip);
		g_object_set_data(G_OBJECT(item), "combo", GINT_TO_POINTER(app->combo_ids[n]));
		g_signal_connect(item, "clicked", G_CALLBACK(on_combo_chosen), app);
		GtkEventController *hover = gtk_event_controller_motion_new();
		g_signal_connect(hover, "enter", G_CALLBACK(on_combo_enter), app);
		g_signal_connect(hover, "leave", G_CALLBACK(on_combo_leave), app);
		gtk_widget_add_controller(item, hover);
		gtk_box_append(GTK_BOX(box), item);
	}
	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 420);
	gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scroll), 320);
	gtk_popover_set_child(GTK_POPOVER(app->combo_popover), scroll);
	GdkRectangle at = { (int)x, (int)y, 1, 1 };
	gtk_popover_set_pointing_to(GTK_POPOVER(app->combo_popover), &at);
	gtk_popover_popup(GTK_POPOVER(app->combo_popover));
}

static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer user)
{
	app_t *app = user;
	app->pointer_x = x;
	app->pointer_y = y;
	controls_motion(&app->ctl, x * app->scale, y * app->scale);
}

/* the raw button events: the toolkit's click gestures track one button at
 * a time and end the first press when a second button joins, which is
 * exactly the gesture the pairs want */
static gboolean on_button_event(GtkEventControllerLegacy *c, GdkEvent *event, gpointer user)
{
	app_t *app = user;
	GdkEventType type = gdk_event_get_event_type(event);
	if (type != GDK_BUTTON_PRESS && type != GDK_BUTTON_RELEASE)
		return FALSE;
	guint button = gdk_button_event_get_button(event);
	if (type == GDK_BUTTON_PRESS)
	{
		GdkModifierType gdk = gdk_event_get_modifier_state(event);
		unsigned mods = (gdk & GDK_SHIFT_MASK ? CONTROLS_SHIFT : 0) | (gdk & GDK_CONTROL_MASK ? CONTROLS_CONTROL : 0);
		controls_press(&app->ctl, (int)button, mods, app->pointer_x * app->scale, app->pointer_y * app->scale);
	}
	else
		controls_release(&app->ctl, (int)button);
	return TRUE;
}

static gboolean on_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer user)
{
	app_t *app = user;
	return controls_scroll(&app->ctl, app->pointer_x * app->scale, app->pointer_y * app->scale, dy) ? TRUE : FALSE;
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint keycode, GdkModifierType mods, gpointer user)
{
	app_t *app = user;
	switch (keyval)
	{
	case GDK_KEY_space:
		app->paused = !app->paused;
		machine_pause(app->mc, app->paused);
		return TRUE;
	case GDK_KEY_n:
		on_next_clicked(NULL, app);
		return TRUE;
	case GDK_KEY_p:
		on_prev_clicked(NULL, app);
		return TRUE;
	case GDK_KEY_l:
		playlist_show(app);
		return TRUE;
	case GDK_KEY_q:
	case GDK_KEY_Escape:
		g_main_loop_quit(app->loop);
		return TRUE;
	default:
		return FALSE;
	}
}

static gboolean on_close(GtkWindow *w, gpointer user)
{
	app_t *app = user;
	g_main_loop_quit(app->loop);
	return FALSE;
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv)
{
	options_t opt;
	app_t app;
	memset(&app, 0, sizeof(app));
	app.songs = g_ptr_array_new_with_free_func(g_free);
	app.current = -1;
	for (int n = 0; n < MIDI_SLOTS; n++)
		app.midi_choice[n] = -1;
	app.audio_choice = -1;

	config_defaults(&app.cfg);
	if (config_path(app.config_file, sizeof(app.config_file)))
	{
		char complaint[512] = "";
		if (!config_load(&app.cfg, app.config_file, complaint, sizeof(complaint)))
			fprintf(stderr, "scgui: cannot read %s\n", app.config_file);
		else if (complaint[0])
			fprintf(stderr, "scgui: %s\n", complaint);
	}
	options_from_config(&app.cfg, &opt);

	int rc = parse_options(argc, argv, &opt, app.songs);
	if (rc <= 0)
		return rc == 0 ? 0 : 2;

	app.reset = (machine_reset_t)word_index(reset_words, MACHINE_RESET_COUNT, app.cfg.reset, MACHINE_RESET_GS);
	app.rate_choice = nearest_index(output_rates, RATE_CHOICES, (int)opt.audio_rate);
	app.block_choice = nearest_index(block_sizes, BLOCK_CHOICES, app.cfg.audio_block);
	app.device_count = audio_list(app.devices, AUDIO_DEVICES_MAX);
	for (int n = 0; n < app.device_count; n++)
		if (app.cfg.audio_device[0] && !strcmp(app.devices[n].name, app.cfg.audio_device))
			app.audio_choice = n;

	gtk_init();

	char exe_dir[PATH_MAX];
	session_exe_directory(argv[0], exe_dir, sizeof(exe_dir));
	machine_options_t mo = { opt.model, opt.rom, exe_dir, opt.map, opt.midi_rate,
	                         { 0 },
	                         opt.tail, opt.keep_settings, opt.no_cache, opt.no_audio,
	                         app.audio_choice >= 0 ? app.devices[app.audio_choice].index : -1,
	                         (unsigned)block_sizes[app.block_choice],
	                         (unsigned)output_rates[app.rate_choice] };
	for (int n = 0; n < MACHINE_SYSTEMS; n++)
		mo.computer[n] = opt.computer[n];
	char err[512];
	app.mc = machine_start(&mo, err, sizeof(err));
	if (!app.mc)
	{
		fprintf(stderr, "scgui: %s\n", err);
		if (strncmp(err, "unknown model", 13) != 0)
			fprintf(stderr, "scgui: put sc88pro.zip (or sc88.zip, sc88vl.zip) beside the program"
			                " or in ~/.mame/roms, or give --rom\n");
		return 1;
	}

	app.shown_model = machine_model(app.mc);
	machine_set_reset(app.mc, app.reset);
	/* the ties the file remembers, by the device's name; one whose device is
	 * not here now keeps its place in the file for the next time */
	app.port_count = machine_midi_list(app.mc, app.ports, MIDI_PORTS_MAX);
	for (int slot = 0; slot < MIDI_SLOTS; slot++)
	{
		if (!app.cfg.midi[slot][0])
			continue;
		for (int n = 0; n < app.port_count; n++)
		{
			bool fits = midi_slot_is_input[slot] ? app.ports[n].readable : app.ports[n].writable;
			if (fits && !strcmp(app.ports[n].name, app.cfg.midi[slot]))
				app.midi_choice[slot] = n;
		}
		if (app.midi_choice[slot] >= 0)
			midi_apply(&app, slot);
	}

	app.window = gtk_window_new();
	app.scale = gtk_widget_get_scale_factor(app.window);
	app.size = opt.size;
	panel_model_t pm = panel_model_for(machine_model(app.mc));
	app.panel = panel_create(pm, panel_pitch_for(pm, app.size) * app.scale);
	if (!app.panel)
	{
		app.scale = 1;
		app.panel = panel_create(pm, panel_pitch_for(pm, app.size));
	}
	if (!app.panel)
	{
		fprintf(stderr, "scgui: no panel artwork for size %d\n", opt.size);
		machine_stop(app.mc);
		return 1;
	}
	int w = panel_width(app.panel), h = panel_height(app.panel);
	app.frame = calloc((size_t)w * h, sizeof(uint32_t));

	app.area = gtk_drawing_area_new();
	gtk_widget_set_size_request(app.area, w / app.scale, h / app.scale);
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(app.area), draw, &app, NULL);
	gtk_window_set_child(GTK_WINDOW(app.window), app.area);
	gtk_window_set_resizable(GTK_WINDOW(app.window), FALSE);
	set_title(&app);

	GtkEventController *buttons = gtk_event_controller_legacy_new();
	g_signal_connect(buttons, "event", G_CALLBACK(on_button_event), &app);
	gtk_widget_add_controller(app.area, buttons);
	GtkEventController *motion = gtk_event_controller_motion_new();
	g_signal_connect(motion, "motion", G_CALLBACK(on_motion), &app);
	gtk_widget_add_controller(app.area, motion);
	GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), &app);
	gtk_widget_add_controller(app.area, scroll);
	GtkEventController *key = gtk_event_controller_key_new();
	g_signal_connect(key, "key-pressed", G_CALLBACK(on_key), &app);
	gtk_widget_add_controller(app.window, key);
	g_signal_connect(app.window, "close-request", G_CALLBACK(on_close), &app);

	app.map = opt.map;
	controls_init(&app.ctl, app.panel, &actions, &app);
	controls_set_knob_notches(&app.ctl, app.cfg.knob_notches);
	controls_set_swap_buttons(&app.ctl, app.cfg.swap_buttons);
	controls_set_knob(&app.ctl, app.cfg.volume);
	controls_set_soft_power(&app.ctl, app.shown_model == SCEMU_MODEL_SC55MK2);
	machine_set_gain(app.mc, app.ctl.knob * app.ctl.knob);
	gtk_window_present(GTK_WINDOW(app.window));
	if (app.songs->len)
		play_index(&app, 0);

	app.loop = g_main_loop_new(NULL, FALSE);
	guint tick = g_timeout_add(33, on_tick, &app);
	g_main_loop_run(app.loop);
	g_source_remove(tick);
	if (app.config_timer)
		g_source_remove(app.config_timer);
	if (app.config_file[0])
		config_save(&app.cfg, app.config_file);

	machine_stop(app.mc);
	if (app.combo_popover)
		gtk_widget_unparent(app.combo_popover);
	if (app.logo_popover)
		gtk_widget_unparent(app.logo_popover);
	if (app.system_popover)
	{
		gtk_widget_unparent(app.system_popover);
		g_object_unref(app.system_model_action);
	}
	gtk_window_destroy(GTK_WINDOW(app.window));
	free(app.frame);
	panel_destroy(app.panel);
	g_ptr_array_free(app.songs, TRUE);
	g_main_loop_unref(app.loop);
	return 0;
}
