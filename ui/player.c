/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gdk/gdkx.h>
#include <string.h>

#include "app.h"
#include "i18n.h"
#include "log.h"

/* The playback controller: which entry plays, what happens when it ends,
 * and the backend's events turned into what the window shows. The backend
 * process starts the first time something plays, so a Kiplay that only
 * shows playlists doesn't pay for an idle mpv. */

#define CRASH_WINDOW_US (60 * G_USEC_PER_SEC)
#define MAX_CRASHES 3
#define STATUS_MESSAGE_S 6

static guint status_timer;

static gboolean restore_status(gpointer data)
{
    status_timer = 0;
    kp_playlists_update_status(data);
    return G_SOURCE_REMOVE;
}

/* A message in the status line for a few seconds. */
static void status_message(kp_app *app, const char *text)
{
    gtk_label_set_text(GTK_LABEL(app->status), text);
    if (status_timer)
        g_source_remove(status_timer);
    status_timer = g_timeout_add_seconds(STATUS_MESSAGE_S, restore_status, app);
}

static kp_playlist *playing_list(kp_app *app)
{
    long i = kp_plset_find(app->playlists, app->playing.playlist_id);
    return i >= 0 ? kp_plset_get(app->playlists, (size_t)i) : NULL;
}

static const kp_entry *playing_entry(kp_app *app)
{
    kp_playlist *pl = playing_list(app);
    return pl && app->playing.index >= 0 ? kp_playlist_get(pl, (size_t)app->playing.index) : NULL;
}

/* What the backend gets for a location: local files as paths, without
 * Audacious's "?N" subtune suffix (mpv doesn't take it; kiplay-audio will). */
static char *backend_path(const char *location)
{
    char *path = g_str_has_prefix(location, "file://") ? g_filename_from_uri(location, NULL, NULL)
                                                        : NULL;
    if (!path && g_str_has_prefix(location, "file://")) {
        /* "file:///a/b.nsf?3": the query makes g_filename_from_uri fail. */
        char *copy = g_strdup(location);
        char *q = strrchr(copy, '?');
        if (q)
            *q = '\0';
        path = g_filename_from_uri(copy, NULL, NULL);
        g_free(copy);
    }
    return path ? path : g_strdup(location);
}

static bool is_local(const char *location)
{
    return location[0] == '/' || g_str_has_prefix(location, "file://");
}

static void update_title(kp_app *app)
{
    const kp_entry *e = playing_entry(app);
    char *title;
    if (!app->playing.loaded && !e)
        title = g_strdup("Kiplay");
    else {
        /* Local files: the playlist's "Artist - Title". Streams: what mpv
         * says now (the song on a radio changes). */
        const char *name = e && e->title && is_local(e->location) ? e->title
                           : app->playing.title                   ? app->playing.title
                           : e                                    ? e->title
                                                                  : NULL;
        char *base = !name && e ? g_path_get_basename(e->location) : NULL;
        title = g_strdup_printf("%s — Kiplay", name ? name : base);
        g_free(base);
    }
    gtk_window_set_title(GTK_WINDOW(app->window), title);
    g_free(title);
}

static void update_buttons(kp_app *app)
{
    bool playing = app->playing.loaded && !app->playing.paused;
    gtk_widget_set_visible(app->play_btn, !playing);
    gtk_widget_set_visible(app->pause_btn, playing);
}

/* The playlist entry learns its length and, for local files with tags, its
 * title. Stream titles change with every song, so they aren't kept. */
static void store_info(kp_app *app, const char *title, double duration)
{
    kp_playlist *pl = playing_list(app);
    const kp_entry *e = playing_entry(app);
    if (!pl || !e || !is_local(e->location))
        return;
    if (kp_playlist_set_info(pl, (size_t)app->playing.index, title,
                             duration > 0 ? (int64_t)(duration * 1000) : -1)) {
        kp_playlists_refresh_row(app, app->playing.playlist_id, app->playing.index);
        kp_playlists_changed(app);
    }
}

/* Next entry to play after the current one, -1 at the end. */
static long next_index(kp_app *app, bool wrap)
{
    kp_playlist *pl = playing_list(app);
    long n = pl ? (long)kp_playlist_count(pl) : 0;
    if (n == 0)
        return -1;
    bool repeat = kp_prefs_get_bool(app->prefs, "playback", "repeat", false);
    if (kp_prefs_get_bool(app->prefs, "playback", "shuffle", false) && n > 1) {
        long i;
        do
            i = g_random_int_range(0, (gint32)n);
        while (i == app->playing.index);
        return i;
    }
    long i = app->playing.index + 1;
    if (i >= n)
        return repeat || wrap ? 0 : -1;
    return i;
}

static void reset_state(kp_app *app)
{
    app->playing.loaded = false;
    app->playing.paused = false;
    app->playing.position = 0;
    app->playing.duration = 0;
    g_clear_pointer(&app->playing.title, g_free);
}

static void stopped(kp_app *app)
{
    reset_state(app);
    app->playing.has_video = false;
    if (app->playing.backend)
        kp_backend_set_ticking(app->playing.backend, false);
    kp_layout_update_video(app);
    update_buttons(app);
    update_title(app);
    kp_controls_update(app);
}

static void on_event(void *ud, const kp_event *ev);

static bool ensure_backend(kp_app *app)
{
    if (app->playing.backend && kp_backend_running(app->playing.backend))
        return true;
    if (!app->playing.backend) {
        gtk_widget_realize(app->video);
        kp_backend_opts o = {
            .program = kp_prefs_get_str(app->prefs, "mpv", "program", "mpv"),
            .wid = (unsigned long)GDK_WINDOW_XID(app->video->window),
            .use_config = kp_prefs_get_bool(app->prefs, "mpv", "use_config", false),
            .volume = kp_prefs_get_int(app->prefs, "playback", "volume", 100, 0, 100),
            .extra = kp_prefs_get_str(app->prefs, "mpv", "extra", NULL),
        };
        app->playing.backend = kp_backend_new_mpv(&o, on_event, app);
    }
    char *err = NULL;
    if (!kp_backend_start(app->playing.backend, &err)) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                                              GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                              _("Cannot run the player program (mpv)"));
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(d), _("%s\n\nInstall mpv, or set its path in kiplay.ini "
                                     "([mpv] program = ...)."),
            err);
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        g_free(err);
        return false;
    }
    return true;
}

void kp_player_play_at(kp_app *app, int id, long index)
{
    long pi = kp_plset_find(app->playlists, id);
    kp_playlist *pl = pi >= 0 ? kp_plset_get(app->playlists, (size_t)pi) : NULL;
    const kp_entry *e = pl && index >= 0 ? kp_playlist_get(pl, (size_t)index) : NULL;
    if (!e || !ensure_backend(app))
        return;
    reset_state(app);
    kp_playlists_mark_playing(app, app->playing.playlist_id, -1);
    app->playing.playlist_id = id;
    app->playing.index = index;
    kp_playlists_mark_playing(app, id, index);
    kp_prefs_set_int(app->prefs, "playback", "playlist", id);
    kp_prefs_set_int(app->prefs, "playback", "index", (int)index);
    char *path = backend_path(e->location);
    kp_log_debug("play: %s", path);
    kp_backend_load(app->playing.backend, path, 0);
    g_free(path);
    update_title(app);
    kp_controls_update(app);
}

void kp_player_play(kp_app *app)
{
    if (app->playing.loaded) {
        if (app->playing.paused)
            kp_backend_set_pause(app->playing.backend, false);
        return;
    }
    long index;
    int id = kp_playlists_selection(app, &index);
    if (index < 0 && kp_plset_find(app->playlists, app->playing.playlist_id) >= 0) {
        /* Nothing selected: where it last played. */
        id = app->playing.playlist_id;
        index = app->playing.index >= 0 ? app->playing.index : 0;
    }
    kp_player_play_at(app, id, index < 0 ? 0 : index);
}

void kp_player_pause(kp_app *app)
{
    if (app->playing.loaded)
        kp_backend_set_pause(app->playing.backend, true);
}

void kp_player_toggle_pause(kp_app *app)
{
    if (!app->playing.loaded)
        kp_player_play(app);
    else
        kp_backend_set_pause(app->playing.backend, !app->playing.paused);
}

void kp_player_stop(kp_app *app)
{
    if (app->playing.backend)
        kp_backend_stop(app->playing.backend);
    stopped(app);
}

void kp_player_next(kp_app *app)
{
    long i = next_index(app, true);
    if (i >= 0)
        kp_player_play_at(app, app->playing.playlist_id, i);
}

void kp_player_prev(kp_app *app)
{
    /* Like most players: a few seconds in, "previous" restarts the song. */
    if (app->playing.loaded && app->playing.position > 3) {
        kp_player_seek(app, 0);
        return;
    }
    kp_playlist *pl = playing_list(app);
    long n = pl ? (long)kp_playlist_count(pl) : 0;
    if (n == 0)
        return;
    long i = app->playing.index - 1;
    if (i < 0)
        i = n - 1;
    kp_player_play_at(app, app->playing.playlist_id, i);
}

void kp_player_seek(kp_app *app, double seconds)
{
    if (app->playing.loaded)
        kp_backend_seek(app->playing.backend, seconds < 0 ? 0 : seconds);
}

void kp_player_seek_relative(kp_app *app, double seconds)
{
    if (app->playing.loaded)
        kp_backend_seek_relative(app->playing.backend, seconds);
}

void kp_player_set_volume(kp_app *app, int volume)
{
    kp_prefs_set_int(app->prefs, "playback", "volume", volume);
    if (app->playing.backend)
        kp_backend_set_volume(app->playing.backend, volume);
}

void kp_player_playlist_closed(kp_app *app, int id)
{
    if (id == app->playing.playlist_id) {
        kp_player_stop(app);
        app->playing.index = -1;
    }
}

/* The file ran out (or wouldn't play): on to the next one. */
static void advance(kp_app *app, bool failed)
{
    kp_playlist *pl = playing_list(app);
    long n = pl ? (long)kp_playlist_count(pl) : 0;
    if (failed) {
        const kp_entry *e = playing_entry(app);
        char *base = e ? g_path_get_basename(e->location) : g_strdup("?");
        char *msg = g_strdup_printf(_("Could not play %s"), base);
        status_message(app, msg);
        kp_log_warn("%s", msg);
        g_free(msg);
        g_free(base);
        /* A whole playlist of broken files must not spin forever. */
        if (++app->playing.failures >= n) {
            app->playing.failures = 0;
            stopped(app);
            return;
        }
    }
    long i = next_index(app, false);
    if (i < 0) {
        stopped(app);
        return;
    }
    kp_player_play_at(app, app->playing.playlist_id, i);
}

static void crashed(kp_app *app, const char *why)
{
    gint64 now = g_get_monotonic_time();
    if (now - app->playing.first_crash_us > CRASH_WINDOW_US) {
        app->playing.first_crash_us = now;
        app->playing.crashes = 0;
    }
    bool was_loaded = app->playing.loaded;
    double at = app->playing.position;
    kp_log_warn(_("the player program stopped (%s)"), why);
    stopped(app);
    if (++app->playing.crashes > MAX_CRASHES) {
        status_message(app, _("The player program keeps stopping; giving up for now."));
        return;
    }
    /* Back where it was. */
    if (was_loaded && ensure_backend(app)) {
        const kp_entry *e = playing_entry(app);
        if (e) {
            char *path = backend_path(e->location);
            kp_backend_load(app->playing.backend, path, at);
            g_free(path);
        }
    }
}

static void on_event(void *ud, const kp_event *ev)
{
    kp_app *app = ud;
    kp_playing *p = &app->playing;
    if (ev->type != KP_EV_TIME)
        kp_log_debug("event %d: %.2f %d %s", (int)ev->type, ev->value, ev->flag,
                     ev->text ? ev->text : "");
    switch (ev->type) {
    case KP_EV_LOADED:
        p->loaded = true;
        p->failures = 0;
        kp_backend_set_ticking(p->backend, true);
        kp_layout_update_video(app);
        update_buttons(app);
        break;
    case KP_EV_DURATION:
        p->duration = ev->value;
        store_info(app, NULL, ev->value);
        kp_controls_update(app);
        break;
    case KP_EV_TIME:
        p->position = ev->value;
        kp_controls_update(app);
        break;
    case KP_EV_PAUSED:
        p->paused = ev->flag;
        update_buttons(app);
        break;
    case KP_EV_VIDEO:
        p->has_video = ev->flag;
        /* Between two files the track list is briefly empty: only a
         * loaded file decides, or the area would blink. */
        if (p->loaded)
            kp_layout_update_video(app);
        break;
    case KP_EV_TITLE:
        /* The window shows mpv's idea (it follows stream titles too)... */
        g_free(p->title);
        p->title = g_strdup(ev->text);
        update_title(app);
        break;
    case KP_EV_TAGS:
        /* ...the playlist keeps the tags, as "Artist - Title". */
        if (ev->text) {
            store_info(app, ev->text, -1);
            update_title(app);
        }
        break;
    case KP_EV_ENDED:
        if (ev->flag && ev->text)
            kp_log_debug("ended with error: %s", ev->text);
        p->loaded = false;
        advance(app, ev->flag);
        break;
    case KP_EV_IDLE:
        break;
    case KP_EV_CRASHED:
        crashed(app, ev->text);
        break;
    }
}

void kp_player_init(kp_app *app)
{
    app->playing.playlist_id = kp_prefs_get_int(app->prefs, "playback", "playlist", -1, -1, 1000000);
    app->playing.index = kp_prefs_get_int(app->prefs, "playback", "index", -1, -1, 100000000);
    update_buttons(app);
    kp_controls_update(app);
}

void kp_player_free(kp_app *app)
{
    if (status_timer) {
        g_source_remove(status_timer);
        status_timer = 0;
    }
    kp_backend_free(app->playing.backend);
    app->playing.backend = NULL;
    g_clear_pointer(&app->playing.title, g_free);
}
