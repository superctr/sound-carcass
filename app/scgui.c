/* scgui: the Sound Canvas on the desktop -- the front panel in a window,
 * a playlist beside it.
 *
 * Copyright (c) 2026 ian karlsson
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define _POSIX_C_SOURCE 200809L
#include <limits.h>
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
#include "combos.h"

#define MIDI_PORTS_MAX 64
#define MIDI_SLOTS 5           /* MIDI IN A, MIDI IN B, MIDI OUT, Song A, Song B */

#define DEFAULT_PITCH 4
#define KNOB_STEP 0.05f

typedef struct app
{
	machine_t *mc;
	panel_t *panel;
	int pitch, scale;
	uint32_t *frame;
	GtkWidget *window, *area, *playlist_window, *list, *audio_window;
	GMainLoop *loop;
	GPtrArray *songs;          /* char * paths */
	int current;               /* index in songs, or -1 */
	bool power, paused;
	float knob;
	int pressed_element;       /* the element under the held mouse button, or -1 */
	uint64_t seen_generation;
	machine_state_t state;
	uint64_t latched;          /* elements queued with the right button, pressed with the next key */
	bool latched_down;         /* the queued keys are down right now */
	bool release_after_boot;
	double pointer_x, pointer_y;
	midi_port_info_t ports[MIDI_PORTS_MAX];
	int port_count;
	GtkWidget *midi_drop[MIDI_SLOTS];
	int midi_choice[MIDI_SLOTS];   /* index into ports, or -1 */
	GtkWidget *combo_popover;
	int combo_ids[64];
	int combo_hold_count;
	scemu_button_t combo_hold[4];
	int combo_press;
} app_t;

static const char *const midi_slot_names[MIDI_SLOTS] = { "MIDI IN A", "MIDI IN B", "MIDI OUT", "Song to A", "Song to B" };
static const bool midi_slot_is_input[MIDI_SLOTS] = { true, true, false, false, false };

/* ---------------------------------------------------------------- options */

typedef struct options
{
	const char *model, *rom;
	scemu_map_t map;
	uint32_t midi_rate;
	int pitch;
	bool keep_settings, no_cache, no_audio;
	double tail;
} options_t;

static void usage(FILE *fp)
{
	fprintf(fp,
	        "usage: scgui [options] [file.mid ...]\n"
	        "  --model NAME        sc88pro (default when its ROMs are found), sc88, sc88vl\n"
	        "  --rom PATH          a zip or directory with the ROM images\n"
	        "  --map sc55|sc88|sc88pro\n"
	        "                      play every part from that instrument map\n"
	        "  --midi-rate BAUD    31250 (default), 38400, 0\n"
	        "  --size 4|8          the window size: the display's dot pitch in pixels\n"
	        "  --keep-settings     keep the machine's settings memory across sessions\n"
	        "  --no-cache          boot the firmware every time\n"
	        "  --no-audio          run without a sound card\n"
	        "  --tail N            seconds after a song's last event (default 4)\n");
}

static int parse_options(int argc, char **argv, options_t *o, GPtrArray *songs)
{
	memset(o, 0, sizeof(*o));
	o->midi_rate = 31250;
	o->pitch = DEFAULT_PITCH;
	o->tail = 4;
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
			         : !strcmp(v, "sc88pro") ? SCEMU_MAP_SC88PRO : SCEMU_MAP_NATIVE;
		}
		else if (!strcmp(a, "--midi-rate") && n + 1 < argc)
			o->midi_rate = (uint32_t)atoi(argv[++n]);
		else if (!strcmp(a, "--size") && n + 1 < argc)
			o->pitch = atoi(argv[++n]);
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

/* ---------------------------------------------------------------- playlist */

static void play_index(app_t *app, int index);
static void latched_press(app_t *app, bool down);
static void latched_clear(app_t *app);

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
	int client = choice >= 0 ? app->ports[choice].client : -1;
	int port = choice >= 0 ? app->ports[choice].port : 0;
	if (midi_slot_is_input[slot])
		machine_midi_input(app->mc, slot, client, port);
	else
		machine_midi_output(app->mc, slot - 2, client, port);
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
	app->port_count = midi_io_list(app->ports, MIDI_PORTS_MAX);
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

static void on_midi_refresh(GtkButton *b, gpointer user)
{
	app_t *app = user;
	/* a port that is gone drops to none; the rest keep their choice by name */
	midi_port_info_t old[MIDI_PORTS_MAX];
	int old_choice[MIDI_SLOTS];
	memcpy(old, app->ports, sizeof(old));
	memcpy(old_choice, app->midi_choice, sizeof(old_choice));
	int count = midi_io_list(app->ports, MIDI_PORTS_MAX);
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

static GtkWidget *midi_section(app_t *app)
{
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
	gtk_widget_set_margin_start(grid, 8);
	gtk_widget_set_margin_end(grid, 8);
	gtk_widget_set_margin_top(grid, 8);
	gtk_widget_set_margin_bottom(grid, 8);
	for (int slot = 0; slot < MIDI_SLOTS; slot++)
	{
		GtkWidget *label = gtk_label_new(midi_slot_names[slot]);
		gtk_label_set_xalign(GTK_LABEL(label), 0);
		gtk_grid_attach(GTK_GRID(grid), label, 0, slot, 1, 1);
		app->midi_drop[slot] = gtk_drop_down_new(NULL, NULL);
		gtk_widget_set_hexpand(app->midi_drop[slot], TRUE);
		g_object_set_data(G_OBJECT(app->midi_drop[slot]), "slot", GINT_TO_POINTER(slot));
		g_signal_connect(app->midi_drop[slot], "notify::selected", G_CALLBACK(on_midi_selected), app);
		gtk_grid_attach(GTK_GRID(grid), app->midi_drop[slot], 1, slot, 1, 1);
	}
	GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
	gtk_widget_set_tooltip_text(refresh, "Look for ports again");
	gtk_widget_set_valign(refresh, GTK_ALIGN_START);
	g_signal_connect(refresh, "clicked", G_CALLBACK(on_midi_refresh), app);
	gtk_grid_attach(GTK_GRID(grid), refresh, 2, 0, 1, 1);
	midi_fill(app);
	return grid;
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

static void audio_show(app_t *app)
{
	if (!app->audio_window)
	{
		GtkWidget *w = gtk_window_new();
		gtk_window_set_title(GTK_WINDOW(w), "Audio");
		gtk_window_set_transient_for(GTK_WINDOW(w), GTK_WINDOW(app->window));
		gtk_window_set_hide_on_close(GTK_WINDOW(w), TRUE);
		char text[256];
		snprintf(text, sizeof(text), "Output: %s\nSample rate: 32000 Hz, the machine's own\nUnderruns so far: %u",
		         machine_audio_driver(app->mc), app->state.underruns);
		GtkWidget *label = gtk_label_new(text);
		gtk_widget_set_margin_start(label, 16);
		gtk_widget_set_margin_end(label, 16);
		gtk_widget_set_margin_top(label, 16);
		gtk_widget_set_margin_bottom(label, 16);
		gtk_window_set_child(GTK_WINDOW(w), label);
		app->audio_window = w;
	}
	gtk_window_present(GTK_WINDOW(app->audio_window));
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

static void set_title(app_t *app)
{
	char title[512];
	if (app->state.song[0])
		snprintf(title, sizeof(title), "%s%s%s — %s", app->state.title[0] ? app->state.title : app->state.song,
		         app->paused ? " (paused)" : "", app->power ? "" : " (off)", machine_model_label(app->mc));
	else
		snprintf(title, sizeof(title), "%s%s", machine_model_label(app->mc), app->power ? "" : " (off)");
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
		panel_set_lcd(app->panel, &st.lcd);
		panel_set_leds(app->panel, st.leds);
		if (song_changed)
			set_title(app);
		if (finished_now && app->current >= 0 && app->current + 1 < (int)app->songs->len)
			play_index(app, app->current + 1);
		if (app->release_after_boot && !st.booting && st.power)
		{
			app->release_after_boot = false;
			latched_press(app, false);
			latched_clear(app);
		}
	}
	if (panel_dirty(app->panel))
		gtk_widget_queue_draw(app->area);
	return G_SOURCE_CONTINUE;
}

/* the queued keys go down, in the order they were queued, before the key they modify */
static void latched_press(app_t *app, bool down)
{
	for (int e = 0; e < PANEL_ELEMENT_COUNT; e++)
		if ((app->latched >> e) & 1)
			machine_button(app->mc, (scemu_button_t)panel_element_button((panel_element_t)e), down);
	app->latched_down = down;
}

static void latched_clear(app_t *app)
{
	for (int e = 0; e < PANEL_ELEMENT_COUNT; e++)
		if ((app->latched >> e) & 1)
			panel_set_pressed(app->panel, (panel_element_t)e, false);
	app->latched = 0;
}

static void element_action(app_t *app, int e)
{
	switch (e)
	{
	case PANEL_SWITCH_POWER:
		app->power = !app->power;
		if (app->power && app->latched)
		{
			latched_press(app, true);
			app->release_after_boot = true;
		}
		machine_power(app->mc, app->power);
		if (!app->power)
		{
			app->current = -1;
			list_select(app, -1);
		}
		set_title(app);
		break;
	case PANEL_JACK_MIDI_IN_B:
		playlist_show(app);
		break;
	case PANEL_JACK_PHONES:
		audio_show(app);
		break;
	default:
		break;
	}
}

/* a combination from the manual: the held keys go down in order, the
 * pressed one follows, and everything comes up a moment later */
static gboolean combo_release(gpointer user)
{
	app_t *app = user;
	if (app->combo_press >= 0)
		machine_button(app->mc, (scemu_button_t)app->combo_press, false);
	for (int n = app->combo_hold_count - 1; n >= 0; n--)
		machine_button(app->mc, app->combo_hold[n], false);
	app->combo_hold_count = 0;
	app->combo_press = -1;
	return G_SOURCE_REMOVE;
}

static void on_combo_chosen(GtkButton *b, gpointer user)
{
	app_t *app = user;
	const combo_t *c = &combos[GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "combo"))];
	gtk_popover_popdown(GTK_POPOVER(app->combo_popover));
	if (app->combo_hold_count || app->combo_press >= 0)
		return;
	for (int n = 0; n < c->hold_count; n++)
	{
		machine_button(app->mc, c->hold[n], true);
		app->combo_hold[app->combo_hold_count++] = c->hold[n];
	}
	if (c->press != SCEMU_BUTTON_COUNT)
	{
		machine_button(app->mc, c->press, true);
		app->combo_press = c->press;
	}
	else
		app->combo_press = -1;
	g_timeout_add(150, combo_release, app);
}

static void combo_menu(app_t *app, int element, double x, double y)
{
	int button = panel_element_button((panel_element_t)element);
	int count = combos_for((scemu_button_t)button, app->combo_ids, 64);
	if (count == 0)
		return;
	if (!app->combo_popover)
	{
		app->combo_popover = gtk_popover_new();
		gtk_widget_set_parent(app->combo_popover, app->area);
	}
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	for (int n = 0; n < count; n++)
	{
		const combo_t *c = &combos[app->combo_ids[n]];
		GtkWidget *item = gtk_button_new_with_label(c->name);
		gtk_button_set_has_frame(GTK_BUTTON(item), FALSE);
		gtk_label_set_xalign(GTK_LABEL(gtk_button_get_child(GTK_BUTTON(item))), 0);
		char tip[400];
		snprintf(tip, sizeof(tip), "%s  (manual p.%d)", c->effect, c->page);
		gtk_widget_set_tooltip_text(item, tip);
		g_object_set_data(G_OBJECT(item), "combo", GINT_TO_POINTER(app->combo_ids[n]));
		g_signal_connect(item, "clicked", G_CALLBACK(on_combo_chosen), app);
		gtk_box_append(GTK_BOX(box), item);
	}
	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 360);
	gtk_popover_set_child(GTK_POPOVER(app->combo_popover), scroll);
	GdkRectangle at = { (int)x, (int)y, 1, 1 };
	gtk_popover_set_pointing_to(GTK_POPOVER(app->combo_popover), &at);
	gtk_popover_popup(GTK_POPOVER(app->combo_popover));
}

static void on_pressed(GtkGestureClick *g, int n_press, double x, double y, gpointer user)
{
	app_t *app = user;
	int e = panel_hit(app->panel, (int)(x * app->scale), (int)(y * app->scale));
	guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
	GdkModifierType mods = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
	if (e < 0)
		return;
	int b = panel_element_button((panel_element_t)e);
	if (b >= 0)
	{
		if (button == GDK_BUTTON_MIDDLE || (button == GDK_BUTTON_SECONDARY && (mods & GDK_CONTROL_MASK)))
			combo_menu(app, e, x, y);
		else if (button == GDK_BUTTON_SECONDARY)
		{
			app->latched ^= (uint64_t)1 << e;
			panel_set_pressed(app->panel, (panel_element_t)e, (app->latched >> e) & 1);
		}
		else
		{
			if (app->latched && !app->latched_down)
				latched_press(app, true);
			app->pressed_element = e;
			machine_button(app->mc, (scemu_button_t)b, true);
			panel_set_pressed(app->panel, (panel_element_t)e, true);
		}
	}
	else if (button == GDK_BUTTON_PRIMARY)
		element_action(app, e);
}

static void on_released(GtkGestureClick *g, int n_press, double x, double y, gpointer user)
{
	app_t *app = user;
	int e = app->pressed_element;
	if (e < 0)
		return;
	app->pressed_element = -1;
	int b = panel_element_button((panel_element_t)e);
	machine_button(app->mc, (scemu_button_t)b, false);
	panel_set_pressed(app->panel, (panel_element_t)e, false);
	if (app->latched_down)
	{
		latched_press(app, false);
		latched_clear(app);
	}
}

static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer user)
{
	app_t *app = user;
	app->pointer_x = x;
	app->pointer_y = y;
}

static gboolean on_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer user)
{
	app_t *app = user;
	int e = panel_hit(app->panel, (int)(app->pointer_x * app->scale), (int)(app->pointer_y * app->scale));
	if (e != PANEL_KNOB_VOLUME && e != PANEL_BUTTON_PREVIEW)
		return FALSE;
	app->knob -= (float)dy * KNOB_STEP;
	app->knob = app->knob < 0 ? 0 : app->knob > 1 ? 1 : app->knob;
	panel_set_knob(app->panel, app->knob);
	machine_set_gain(app->mc, app->knob * app->knob);
	return TRUE;
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
	app.pressed_element = -1;
	app.combo_press = -1;
	for (int n = 0; n < MIDI_SLOTS; n++)
		app.midi_choice[n] = -1;
	app.power = true;
	app.knob = 0.75f;

	int rc = parse_options(argc, argv, &opt, app.songs);
	if (rc <= 0)
		return rc == 0 ? 0 : 2;

	gtk_init();

	char exe_dir[PATH_MAX];
	session_exe_directory(argv[0], exe_dir, sizeof(exe_dir));
	machine_options_t mo = { opt.model, opt.rom, exe_dir, opt.map, opt.midi_rate, opt.tail,
	                         opt.keep_settings, opt.no_cache, opt.no_audio };
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

	app.window = gtk_window_new();
	app.scale = gtk_widget_get_scale_factor(app.window);
	app.pitch = opt.pitch;
	app.panel = panel_create(app.pitch * app.scale);
	if (!app.panel)
	{
		app.scale = 1;
		app.panel = panel_create(app.pitch);
	}
	if (!app.panel)
	{
		fprintf(stderr, "scgui: no panel artwork for size %d\n", opt.pitch);
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

	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
	g_signal_connect(click, "pressed", G_CALLBACK(on_pressed), &app);
	g_signal_connect(click, "released", G_CALLBACK(on_released), &app);
	gtk_widget_add_controller(app.area, GTK_EVENT_CONTROLLER(click));
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

	panel_set_knob(app.panel, app.knob);
	machine_set_gain(app.mc, app.knob * app.knob);
	gtk_window_present(GTK_WINDOW(app.window));
	if (app.songs->len)
		play_index(&app, 0);

	app.loop = g_main_loop_new(NULL, FALSE);
	guint tick = g_timeout_add(33, on_tick, &app);
	g_main_loop_run(app.loop);
	g_source_remove(tick);

	machine_stop(app.mc);
	if (app.combo_popover)
		gtk_widget_unparent(app.combo_popover);
	gtk_window_destroy(GTK_WINDOW(app.window));
	free(app.frame);
	panel_destroy(app.panel);
	g_ptr_array_free(app.songs, TRUE);
	g_main_loop_unref(app.loop);
	return 0;
}
