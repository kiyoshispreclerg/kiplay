/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gdk/gdkx.h>
#include <string.h>

#include "app.h"
#include "i18n.h"
#include "log.h"
#include "media.h"
#include "mpvopts.h"
#include "sidecar.h"
#include "util.h"

/* The playback controller: which entry plays, on which engine, what
 * happens when it ends, and the engines' events turned into what the
 * window shows. Music goes to kiplay-audio (small, gapless, game music),
 * the rest to mpv; each engine starts the first time it is needed, so a
 * Kiplay that only shows playlists runs neither, and an mpv nobody uses
 * any more is let go after a minute. */

#define CRASH_WINDOW_US (60 * G_USEC_PER_SEC)
#define MAX_CRASHES 3
#define STATUS_MESSAGE_S 6
#define MPV_IDLE_S 60
/* Resuming: not for the first seconds, nor when the end was near. */
#define RESUME_MIN_S 10
#define RESUME_END_S 10

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

void kp_player_message(kp_app *app, const char *text)
{
    status_message(app, text);
    if (app->playing.backend && app->playing.loaded && (app->show_video || app->fullscreen))
        kp_backend_show_text(app->playing.backend, text);
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

/* kiplay-audio's turn? ([playback] engine = mpv sends everything to mpv) */
static bool for_audio_engine(kp_app *app, const char *location)
{
    if (!strcmp(kp_prefs_get_str(app->prefs, "playback", "engine", "auto"), "mpv"))
        return false;
    return kp_media_kind_of(location) == KP_MEDIA_AUDIO;
}

/* Next to the kiplay binary (a build tree), else wherever PATH says. */
char *kp_engine_path(void)
{
    char *self = g_file_read_link("/proc/self/exe", NULL);
    char *dir = self ? g_path_get_dirname(self) : NULL;
    char *next = dir ? g_build_filename(dir, "kiplay-audio", NULL) : NULL;
    g_free(self);
    g_free(dir);
    if (next && g_file_test(next, G_FILE_TEST_IS_EXECUTABLE))
        return next;
    g_free(next);
    return g_strdup("kiplay-audio");
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
    kp_desktop_update(app);
}

static void update_buttons(kp_app *app)
{
    bool playing = app->playing.loaded && !app->playing.paused;
    gtk_widget_set_visible(app->play_btn, !playing);
    gtk_widget_set_visible(app->pause_btn, playing);
    kp_desktop_update(app);
}

/* The playlist entry learns its length and, if the tag reader found no
 * title, the player's "Artist - Title". Stream titles change with every
 * song, so they aren't kept. */
static void store_info(kp_app *app, const char *title, double duration)
{
    kp_playlist *pl = playing_list(app);
    const kp_entry *e = playing_entry(app);
    if (!pl || !e || !is_local(e->location))
        return;
    kp_tags t = {e->title ? NULL : title, NULL, NULL,
                 duration > 0 ? (int64_t)(duration * 1000) : -1};
    if (kp_playlist_set_tags(pl, (size_t)app->playing.index, &t)) {
        kp_playlists_refresh_row(app, app->playing.playlist_id, app->playing.index);
        kp_playlists_changed(app);
    }
}

/* ---- what is remembered per file ----------------------------------------- */

/* [files] resume: off, video (default: files that aren't music), all. */
static bool resume_applies(kp_app *app, const char *location)
{
    static const char *const modes[] = {"off", "video", "all", NULL};
    int mode = kp_prefs_get_choice(app->prefs, "files", "resume", modes, 1);
    return mode == 2 || (mode == 1 && kp_media_kind_of(location) == KP_MEDIA_VIDEO);
}

static bool remember_choices(kp_app *app)
{
    return kp_prefs_get_bool(app->prefs, "files", "remember", true);
}

/* Reads what is remembered of the entry that starts. */
static void load_file_settings(kp_app *app, const char *location)
{
    kp_playing *p = &app->playing;
    p->file_local = is_local(location);
    p->file_stored = p->file_local && kp_file_settings_load(NULL, location, &p->file);
    if (!p->file_stored)
        p->file = (kp_file_settings)KP_FILE_SETTINGS_INIT;
    if (!remember_choices(app)) {
        double pos = p->file.position;
        p->file = (kp_file_settings)KP_FILE_SETTINGS_INIT;
        p->file.position = pos;
    }
}

/* How the playing entry opens: its remembered choices, from start. */
static kp_load_opts load_opts(kp_app *app, double start)
{
    const kp_file_settings *f = &app->playing.file;
    return (kp_load_opts){start, f->aid, f->sid, f->audio_delay, f->sub_delay, f->aspect, NULL, 0};
}

/* Writes what is remembered of the playing entry before it goes;
 * finished: it played to the end (nothing to resume). */
static void save_file_settings(kp_app *app, bool finished)
{
    kp_playing *p = &app->playing;
    if (!p->location || !p->file_local)
        return;
    /* Not loaded (stopped already, or it failed): the position known. */
    if (finished)
        p->file.position = 0;
    else if (p->loaded)
        p->file.position = resume_applies(app, p->location) && p->position >= RESUME_MIN_S &&
                                   (p->duration <= 0 || p->position < p->duration - RESUME_END_S)
                               ? p->position
                               : 0;
    kp_file_settings fs = p->file;
    if (!remember_choices(app)) {
        double pos = fs.position;
        fs = (kp_file_settings)KP_FILE_SETTINGS_INIT;
        fs.position = pos;
    }
    bool empty = kp_file_settings_empty(&fs);
    if (empty && !p->file_stored)
        return; /* nothing there, nothing to say */
    if (kp_file_settings_save(NULL, p->location, &fs) == 0)
        p->file_stored = !empty;
    kp_log_debug("file settings: %s at %.1f s", empty ? "forgotten" : "saved", fs.position);
}

/* ---- what comes next ----------------------------------------------------- */

#define HISTORY_MAX 500

/* Where a followed entry is now; -1 if it's gone. */
static long ref_index(kp_app *app, kp_entry_ref *r)
{
    kp_playlist *pl = kp_playlists_get(app, r->id);
    long i = pl ? kp_playlist_find_entry(pl, r->location, r->hint) : -1;
    if (i >= 0)
        r->hint = i;
    return i;
}

static void forget_heard(kp_playing *p)
{
    if (p->heard)
        g_hash_table_remove_all(p->heard);
}

/* Shuffle: a random entry not heard yet this round (the playing one is
 * heard). When all were, a new round if wrap; -1 otherwise. */
static long shuffle_pick(kp_app *app, kp_playlist *pl, bool wrap)
{
    kp_playing *p = &app->playing;
    if (p->heard_id != p->playlist_id) {
        forget_heard(p);
        p->heard_id = p->playlist_id;
    }
    long n = (long)kp_playlist_count(pl);
    for (int round = 0; round < 2; round++) {
        long left = 0;
        for (long i = 0; i < n; i++)
            left += !g_hash_table_contains(p->heard, kp_playlist_get(pl, (size_t)i)->location);
        if (left > 0) {
            long k = g_random_int_range(0, (gint32)left);
            for (long i = 0; i < n; i++)
                if (!g_hash_table_contains(p->heard, kp_playlist_get(pl, (size_t)i)->location) &&
                    k-- == 0)
                    return i;
        }
        if (!wrap || n < 2)
            return -1;
        /* A new round, but not the same song twice in a row. */
        forget_heard(p);
        if (p->location)
            g_hash_table_add(p->heard, (gpointer)p->location);
    }
    return -1;
}

/* What plays after the current entry: the queue first, then the playlist
 * (shuffled or in order). false at the end. wrap: "next" pressed, which
 * goes around even without repeat. */
static bool pick_next(kp_app *app, bool wrap, int *id, long *index)
{
    while (app->queue->len) {
        kp_entry_ref *r = &g_array_index(app->queue, kp_entry_ref, 0);
        long i = ref_index(app, r);
        if (i >= 0) {
            *id = r->id;
            *index = i;
            return true;
        }
        g_array_remove_index(app->queue, 0); /* it went away */
    }
    kp_playlist *pl = playing_list(app);
    long n = pl ? (long)kp_playlist_count(pl) : 0;
    if (n == 0)
        return false;
    bool repeat = kp_prefs_get_bool(app->prefs, "playback", "repeat", false);
    *id = app->playing.playlist_id;
    if (kp_prefs_get_bool(app->prefs, "playback", "shuffle", false) && n > 1) {
        *index = shuffle_pick(app, pl, repeat || wrap);
        return *index >= 0;
    }
    long i = app->playing.index + 1;
    if (i >= n) {
        if (!repeat && !wrap)
            return false;
        i = 0;
    }
    *index = i;
    return true;
}

/* An entry starts: out of the queue, into what was heard. */
static void now_playing(kp_app *app, const kp_entry *e)
{
    kp_playing *p = &app->playing;
    p->location = e->location;
    for (guint i = 0; i < app->queue->len; i++)
        if (g_array_index(app->queue, kp_entry_ref, i).location == e->location) {
            g_array_remove_index(app->queue, i);
            kp_playlists_queue_changed(app);
            break;
        }
    if (p->heard_id != p->playlist_id) {
        forget_heard(p);
        p->heard_id = p->playlist_id;
    }
    g_hash_table_add(p->heard, (gpointer)e->location);
}

static void remember(kp_app *app)
{
    kp_playing *p = &app->playing;
    if (!p->location)
        return;
    kp_entry_ref r = {p->playlist_id, p->location, p->index};
    if (p->history->len >= HISTORY_MAX)
        g_array_remove_index(p->history, 0);
    g_array_append_val(p->history, r);
}

static void reset_state(kp_app *app)
{
    app->playing.loaded = false;
    app->playing.starting = false;
    app->playing.paused = false;
    app->playing.position = 0;
    app->playing.duration = 0;
    g_clear_pointer(&app->playing.title, g_free);
}

static gboolean free_idle_mpv(gpointer data)
{
    kp_app *app = data;
    app->playing.mpv_idle_timer = 0;
    if (app->playing.mpv && app->playing.backend != app->playing.mpv) {
        kp_log_debug("letting the idle mpv go");
        kp_backend_free(app->playing.mpv);
        app->playing.mpv = NULL;
    }
    return G_SOURCE_REMOVE;
}

/* mpv stays a while in case a video comes next, then goes. */
static void mpv_unused(kp_app *app)
{
    if (app->playing.mpv && !app->playing.mpv_idle_timer)
        app->playing.mpv_idle_timer = g_timeout_add_seconds(MPV_IDLE_S, free_idle_mpv, app);
}

static void stopped(kp_app *app)
{
    reset_state(app);
    app->playing.queued = -1;
    app->playing.has_video = false;
    if (app->playing.backend)
        kp_backend_set_ticking(app->playing.backend, false);
    kp_layout_update_video(app);
    kp_tracks_update(app, NULL, 0);
    update_buttons(app);
    update_title(app);
    kp_controls_update(app);
}

static void on_mpv_event(void *ud, const kp_event *ev);
static void on_audio_event(void *ud, const kp_event *ev);

static kp_backend *create(kp_app *app, bool audio)
{
    int volume = kp_prefs_get_int(app->prefs, "playback", "volume", 100, 0, 100);
    kp_backend_opts o = {.volume = app->playing.muted ? 0 : volume};
    if (audio) {
        char *program = kp_engine_path();
        o.program = program;
        o.device = kp_prefs_get_str(app->prefs, "playback", "device", "default");
        g_free(app->playing.audio_device);
        app->playing.audio_device = g_strdup(o.device);
        kp_backend *b = kp_backend_new_audio(&o, on_audio_event, app);
        g_free(program);
        return b;
    }
    gtk_widget_realize(app->video);
    o.program = kp_prefs_get_str(app->prefs, "mpv", "program", "mpv");
    o.wid = (unsigned long)GDK_WINDOW_XID(app->video->window);
    o.use_config = kp_prefs_get_bool(app->prefs, "mpv", "use_config", false);
    o.extra = kp_prefs_get_str(app->prefs, "mpv", "extra", NULL);
    char **args = kp_mpv_pref_args(app->prefs);
    o.args = (const char *const *)args;
    g_free(app->playing.mpv_signature);
    app->playing.mpv_signature = kp_mpv_pref_signature(app->prefs);
    kp_backend *b = kp_backend_new_mpv(&o, on_mpv_event, app);
    g_strfreev(args);
    return b;
}

/* Makes the engine for that kind of file the current one, started; the
 * other one stops playing. */
static bool ensure_backend(kp_app *app, bool audio)
{
    kp_playing *p = &app->playing;
    kp_backend **slot = audio ? &p->audio : &p->mpv;
    if (!*slot)
        *slot = create(app, audio);
    if (p->backend && p->backend != *slot) {
        kp_backend_set_ticking(p->backend, false);
        kp_backend_stop(p->backend);
    }
    p->backend = *slot;
    if (audio)
        mpv_unused(app);
    else if (p->mpv_idle_timer) {
        g_source_remove(p->mpv_idle_timer);
        p->mpv_idle_timer = 0;
    }
    char *err = NULL;
    if (kp_backend_start(*slot, &err))
        return true;
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s",
                                          audio ? _("Cannot run the music engine (kiplay-audio)")
                                                : _("Cannot run the player program (mpv)"));
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(d), "%s\n\n%s", err,
        audio ? _("Reinstall Kiplay, or play music through mpv with [playback] engine = mpv in "
                  "kiplay.ini.")
              : _("Install mpv, or set its path in kiplay.ini ([mpv] program = ...)."));
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    g_free(err);
    return false;
}

/* The subtitle files that sit next to a local video, YouTube's captions
 * already converted to ASS. n = 0 when there are none (or the setting says
 * not to look), and then mpv does its own search. Free with free_subs. */
static kp_sub_file *sidecar_subs(kp_app *app, const char *location, int *n)
{
    static const char *const autos[] = {"exact", "fuzzy", "no", NULL};
    *n = 0;
    if (!is_local(location) || kp_media_kind_of(location) != KP_MEDIA_VIDEO ||
        kp_prefs_get_choice(app->prefs, "subtitles", "auto", autos, 0) == 2)
        return NULL;
    char *path = backend_path(location);
    GPtrArray *found = kp_sidecars_find(path);
    GArray *out = g_array_new(FALSE, FALSE, sizeof(kp_sub_file));
    kp_ytass_opts o;
    kp_sidecar_ass_opts(app->prefs, &o);
    for (guint i = 0; i < found->len; i++) {
        const kp_sidecar *sc = g_ptr_array_index(found, i);
        if (sc->kind == KP_SIDECAR_CHAT)
            continue;
        char *err = NULL;
        char *file = kp_sidecar_play_path(sc, &o, NULL, &err);
        if (!file) {
            kp_log_warn(_("cannot read the captions %s: %s"), sc->path, err);
            g_free(err);
            continue;
        }
        /* The title is how the track shows in the menu. */
        char *title = *sc->lang ? g_strdup(sc->lang) : g_path_get_basename(sc->path);
        kp_sub_file f = {file, title, *sc->lang ? g_strdup(sc->lang) : NULL};
        g_array_append_val(out, f);
    }
    g_ptr_array_free(found, TRUE);
    g_free(path);
    *n = (int)out->len;
    if (*n)
        kp_log_debug("%d subtitle file(s) next to %s", *n, location);
    return (kp_sub_file *)g_array_free(out, FALSE);
}

static void free_subs(kp_sub_file *subs, int n)
{
    for (int i = 0; i < n; i++) {
        g_free((char *)subs[i].path);
        g_free((char *)subs[i].title);
        g_free((char *)subs[i].lang);
    }
    g_free(subs);
}

/* Starts location on the right engine, with what is remembered of it. */
static bool start_playing(kp_app *app, const char *location, double at)
{
    bool audio = for_audio_engine(app, location);
    if (!ensure_backend(app, audio))
        return false;
    /* kiplay-audio takes "?N" and file:// itself; mpv gets a plain path. */
    char *path = audio ? g_strdup(location) : backend_path(location);
    kp_log_debug("play on %s: %s", kp_backend_name(app->playing.backend), path);
    kp_load_opts lo = load_opts(app, at);
    char *local = audio || !is_local(location) ? NULL : backend_path(location);
    kp_chat_file_changed(app, local);
    g_free(local);
    kp_sub_file *subs = audio ? NULL : sidecar_subs(app, location, &lo.n_sub_files);
    lo.sub_files = subs;
    kp_backend_load(app->playing.backend, path, &lo);
    free_subs(subs, lo.n_sub_files);
    app->playing.starting = true;
    g_free(path);
    return true;
}

/* Plays entry index of playlist id; with remember_it, the entry that was
 * playing goes into the history first ("previous" comes back to it). */
static void play_entry(kp_app *app, int id, long index, bool remember_it)
{
    kp_playlist *pl = kp_playlists_get(app, id);
    const kp_entry *e = pl && index >= 0 ? kp_playlist_get(pl, (size_t)index) : NULL;
    if (!e)
        return;
    if (remember_it)
        remember(app);
    save_file_settings(app, false);
    reset_state(app);
    app->playing.queued = -1;
    kp_playlists_mark_playing(app, app->playing.playlist_id, -1);
    app->playing.playlist_id = id;
    app->playing.index = index;
    now_playing(app, e);
    kp_playlists_mark_playing(app, id, index);
    kp_prefs_set_int(app->prefs, "playback", "playlist", id);
    kp_prefs_set_int(app->prefs, "playback", "index", (int)index);
    load_file_settings(app, e->location);
    kp_tracks_file_changed(app);
    double start = resume_applies(app, e->location) ? app->playing.file.position : 0;
    if (!start_playing(app, e->location, start))
        return;
    if (start > 0) {
        char pos[32], *msg;
        kp_format_duration(pos, sizeof pos, (int64_t)(start * 1000));
        msg = g_strdup_printf(_("Resuming at %s"), pos);
        status_message(app, msg);
        g_free(msg);
    }
    update_title(app);
    kp_controls_update(app);
}

void kp_player_play_at(kp_app *app, int id, long index)
{
    play_entry(app, id, index, true);
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
    /* A file on its way pauses when it comes, rather than starting over. */
    if (!app->playing.loaded && app->playing.starting)
        kp_backend_set_pause(app->playing.backend, !app->playing.paused);
    else if (!app->playing.loaded)
        kp_player_play(app);
    else
        kp_backend_set_pause(app->playing.backend, !app->playing.paused);
}

void kp_player_stop(kp_app *app)
{
    save_file_settings(app, false);
    if (app->playing.backend)
        kp_backend_stop(app->playing.backend);
    stopped(app);
    mpv_unused(app);
}

void kp_player_next(kp_app *app)
{
    int id;
    long i;
    if (pick_next(app, true, &id, &i))
        kp_player_play_at(app, id, i);
}

void kp_player_prev(kp_app *app)
{
    /* Like most players: a few seconds in, "previous" restarts the song. */
    if (app->playing.loaded && app->playing.position > 3) {
        kp_player_seek(app, 0);
        return;
    }
    /* Shuffled, "previous" is what played before, not the row above. */
    GArray *h = app->playing.history;
    if (kp_prefs_get_bool(app->prefs, "playback", "shuffle", false)) {
        while (h->len) {
            kp_entry_ref r = g_array_index(h, kp_entry_ref, h->len - 1);
            g_array_remove_index(h, h->len - 1);
            long i = ref_index(app, &r);
            if (i >= 0) {
                play_entry(app, r.id, i, false);
                return;
            }
        }
    }
    kp_playlist *pl = playing_list(app);
    long n = pl ? (long)kp_playlist_count(pl) : 0;
    if (n == 0)
        return;
    long i = app->playing.index - 1;
    if (i < 0)
        i = n - 1;
    play_entry(app, app->playing.playlist_id, i, false);
}

void kp_player_seek(kp_app *app, double seconds)
{
    if (!app->playing.loaded)
        return;
    seconds = seconds < 0 ? 0 : seconds;
    kp_backend_seek(app->playing.backend, seconds);
    kp_desktop_seeked(app, seconds);
}

void kp_player_seek_relative(kp_app *app, double seconds)
{
    if (!app->playing.loaded)
        return;
    kp_backend_seek_relative(app->playing.backend, seconds);
    double to = app->playing.position + seconds;
    if (app->playing.duration > 0 && to > app->playing.duration)
        to = app->playing.duration;
    kp_desktop_seeked(app, to < 0 ? 0 : to);
}

/* What the engines get: the setting, or 0 while muted. */
static void send_volume(kp_app *app)
{
    int v = app->playing.muted ? 0
                               : kp_prefs_get_int(app->prefs, "playback", "volume", 100, 0, 100);
    if (app->playing.mpv)
        kp_backend_set_volume(app->playing.mpv, v);
    if (app->playing.audio)
        kp_backend_set_volume(app->playing.audio, v);
}

void kp_player_set_volume(kp_app *app, int volume)
{
    kp_prefs_set_int(app->prefs, "playback", "volume", volume);
    /* Turning the volume is wanting to hear it. */
    if (app->playing.muted) {
        app->playing.muted = false;
        kp_tracks_mute_changed(app);
    }
    send_volume(app);
    kp_desktop_update(app);
}

void kp_player_toggle_mute(kp_app *app)
{
    app->playing.muted = !app->playing.muted;
    send_volume(app);
    kp_tracks_mute_changed(app);
    kp_player_message(app, app->playing.muted ? _("Muted") : _("Sound on"));
}

void kp_player_open(kp_app *app, char **locations, bool enqueue)
{
    kp_playlists_open(app, locations, !enqueue);
    gtk_window_present(GTK_WINDOW(app->window));
}

/* Drops the followed entries of playlist id from a list (all of them, or
 * only the ones gone from it). */
static void drop_refs(kp_app *app, GArray *refs, int id, bool all)
{
    for (guint i = refs->len; i-- > 0;) {
        kp_entry_ref *r = &g_array_index(refs, kp_entry_ref, i);
        if (r->id == id && (all || ref_index(app, r) < 0))
            g_array_remove_index(refs, i);
    }
}

void kp_player_playlist_closed(kp_app *app, int id)
{
    kp_playing *p = &app->playing;
    if (id == p->playlist_id) {
        kp_player_stop(app);
        p->index = -1;
        p->location = NULL;
        forget_heard(p);
    }
    drop_refs(app, app->queue, id, true);
    drop_refs(app, p->history, id, true);
}

void kp_player_playlist_changed(kp_app *app, int id)
{
    kp_playing *p = &app->playing;
    kp_playlist *pl = kp_playlists_get(app, id);
    if (!pl)
        return;
    if (id == p->playlist_id && p->location) {
        long i = kp_playlist_find_entry(pl, p->location, p->index);
        if (i >= 0 && i != p->index) {
            p->index = i;
            kp_playlists_mark_playing(app, id, i);
            kp_prefs_set_int(app->prefs, "playback", "index", (int)i);
        }
    }
    guint before = app->queue->len;
    drop_refs(app, app->queue, id, false);
    if (app->queue->len != before)
        kp_playlists_queue_changed(app);
    kp_player_requeue(app);
}

void kp_player_queue_toggle(kp_app *app, int id, long index)
{
    kp_playlist *pl = kp_playlists_get(app, id);
    const kp_entry *e = pl ? kp_playlist_get(pl, (size_t)index) : NULL;
    if (!e)
        return;
    for (guint i = 0; i < app->queue->len; i++)
        if (g_array_index(app->queue, kp_entry_ref, i).location == e->location) {
            g_array_remove_index(app->queue, i);
            kp_player_requeue(app);
            return;
        }
    kp_entry_ref r = {id, e->location, index};
    g_array_append_val(app->queue, r);
    kp_player_requeue(app);
}

int kp_player_queue_pos(kp_app *app, int id, const kp_entry *e)
{
    (void)id; /* the location string names the entry by itself */
    for (guint i = 0; app->queue && i < app->queue->len; i++)
        if (g_array_index(app->queue, kp_entry_ref, i).location == e->location)
            return (int)i + 1;
    return 0;
}

void kp_player_set_stop_after(kp_app *app, bool on)
{
    app->playing.stop_after = on;
    kp_player_requeue(app);
}

void kp_player_info_changed(kp_app *app)
{
    update_title(app);
}

/* The file ran out (or wouldn't play): on to the next one. */
static void advance(kp_app *app, bool failed)
{
    if (!failed)
        save_file_settings(app, true);
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
    if (app->playing.stop_after) {
        kp_player_set_stop_after(app, false);
        if (app->stop_after_item)
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app->stop_after_item), FALSE);
        stopped(app);
        return;
    }
    int id;
    long i;
    if (!pick_next(app, false, &id, &i)) {
        stopped(app);
        return;
    }
    kp_player_play_at(app, id, i);
}

/* Gapless: tells an engine that can do it what comes after the current
 * file (if that one is for the same engine), so it starts without a gap. */
void kp_player_requeue(kp_app *app)
{
    kp_playing *p = &app->playing;
    if (!p->backend || !p->loaded || !(kp_backend_caps(p->backend) & KP_CAP_GAPLESS))
        return;
    int id;
    long i;
    kp_playlist *pl = NULL;
    if (!p->stop_after && pick_next(app, false, &id, &i))
        pl = kp_playlists_get(app, id);
    const kp_entry *e = pl ? kp_playlist_get(pl, (size_t)i) : NULL;
    if (e && for_audio_engine(app, e->location)) {
        p->queued_id = id;
        p->queued = i;
        kp_backend_queue(p->backend, e->location);
    } else {
        p->queued = -1;
        kp_backend_queue(p->backend, NULL);
    }
}

/* The engine moved on to the queued file by itself. */
static void took_next(kp_app *app)
{
    kp_playing *p = &app->playing;
    kp_playlist *pl = p->queued >= 0 ? kp_playlists_get(app, p->queued_id) : NULL;
    const kp_entry *e = pl ? kp_playlist_get(pl, (size_t)p->queued) : NULL;
    if (!e)
        return;
    remember(app);
    save_file_settings(app, true);
    reset_state(app);
    kp_playlists_mark_playing(app, p->playlist_id, -1);
    p->playlist_id = p->queued_id;
    p->index = p->queued;
    p->queued = -1;
    now_playing(app, e);
    kp_playlists_mark_playing(app, p->playlist_id, p->index);
    kp_prefs_set_int(app->prefs, "playback", "playlist", p->playlist_id);
    kp_prefs_set_int(app->prefs, "playback", "index", (int)p->index);
    /* Gapless only joins music on kiplay-audio: it started from the top,
     * with nothing to choose. */
    load_file_settings(app, e->location);
    kp_tracks_file_changed(app);
    update_title(app);
    kp_controls_update(app);
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
    const kp_entry *e = playing_entry(app);
    if (was_loaded && e)
        start_playing(app, e->location, at);
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
        p->starting = false;
        p->failures = 0;
        kp_backend_set_ticking(p->backend, true);
        kp_layout_update_video(app);
        kp_tracks_refresh(app);
        update_buttons(app);
        kp_player_requeue(app);
        break;
    case KP_EV_DURATION:
        p->duration = ev->value;
        store_info(app, NULL, ev->value);
        kp_controls_update(app);
        kp_desktop_update(app);
        break;
    case KP_EV_TIME:
        p->position = ev->value;
        kp_chat_set_time(app, ev->value);
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
        p->starting = false;
        advance(app, ev->flag);
        break;
    case KP_EV_NEXT:
        took_next(app);
        break;
    case KP_EV_TRACKS:
        kp_tracks_update(app, ev->tracks, ev->n_tracks);
        break;
    case KP_EV_IDLE:
        break;
    case KP_EV_CRASHED:
        crashed(app, ev->text);
        break;
    }
}

/* Events from an engine that isn't the current one (a late "ended"
 * after switching) are dropped; if it went away, it just goes. */
static bool current(kp_app *app, kp_backend *b, const kp_event *ev)
{
    if (app->playing.backend == b)
        return true;
    if (ev->type == KP_EV_CRASHED)
        kp_log_debug("%s (not in use) went away: %s", kp_backend_name(b), ev->text);
    return false;
}

static void on_mpv_event(void *ud, const kp_event *ev)
{
    kp_app *app = ud;
    if (current(app, app->playing.mpv, ev))
        on_event(app, ev);
}

static void on_audio_event(void *ud, const kp_event *ev)
{
    kp_app *app = ud;
    if (current(app, app->playing.audio, ev))
        on_event(app, ev);
}

void kp_player_init(kp_app *app)
{
    kp_playing *p = &app->playing;
    p->queued = -1;
    p->history = g_array_new(FALSE, FALSE, sizeof(kp_entry_ref));
    p->heard = g_hash_table_new(g_direct_hash, g_direct_equal);
    p->heard_id = -1;
    p->playlist_id = kp_prefs_get_int(app->prefs, "playback", "playlist", -1, -1, 1000000);
    p->index = kp_prefs_get_int(app->prefs, "playback", "index", -1, -1, 100000000);
    /* Where it stopped last time, followed if the playlist changes. */
    const kp_entry *e = playing_entry(app);
    p->location = e ? e->location : NULL;
    update_buttons(app);
    kp_controls_update(app);
}

/* Starts a running engine again with the options it has now; the file
 * playing on it goes on where it was. */
static void restart_engine(kp_app *app, bool audio)
{
    kp_playing *p = &app->playing;
    kp_backend **slot = audio ? &p->audio : &p->mpv;
    if (!*slot)
        return;
    bool current = p->backend == *slot && p->loaded;
    save_file_settings(app, false); /* p->file.position: where it was */
    double at = p->position;
    bool paused = p->paused;
    kp_log_debug("restarting %s for the new options", kp_backend_name(*slot));
    if (p->backend == *slot)
        p->backend = NULL;
    kp_backend_free(*slot);
    *slot = NULL;
    const kp_entry *e = playing_entry(app);
    if (!current || !e)
        return;
    reset_state(app);
    if (start_playing(app, e->location, at) && paused)
        kp_backend_set_pause(p->backend, true);
}

void kp_player_prefs_changed(kp_app *app)
{
    kp_playing *p = &app->playing;
    char *sig = kp_mpv_pref_signature(app->prefs);
    if (p->mpv && g_strcmp0(sig, p->mpv_signature) != 0)
        restart_engine(app, false);
    g_free(sig);
    if (p->audio && g_strcmp0(kp_prefs_get_str(app->prefs, "playback", "device", "default"),
                              p->audio_device) != 0)
        restart_engine(app, true);
}

void kp_player_free(kp_app *app)
{
    save_file_settings(app, false);
    if (status_timer) {
        g_source_remove(status_timer);
        status_timer = 0;
    }
    if (app->playing.mpv_idle_timer) {
        g_source_remove(app->playing.mpv_idle_timer);
        app->playing.mpv_idle_timer = 0;
    }
    kp_backend_free(app->playing.mpv);
    kp_backend_free(app->playing.audio);
    app->playing.backend = app->playing.mpv = app->playing.audio = NULL;
    g_clear_pointer(&app->playing.title, g_free);
    g_clear_pointer(&app->playing.mpv_signature, g_free);
    g_clear_pointer(&app->playing.audio_device, g_free);
    if (app->playing.history)
        g_array_free(app->playing.history, TRUE);
    app->playing.history = NULL;
    g_clear_pointer(&app->playing.heard, g_hash_table_destroy);
}
