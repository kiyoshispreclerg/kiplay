/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Playlist > "Split into tracks...": one file cut into several, as
 * marked by times typed the way video descriptions mark chapters
 * (splitspec.h reads them). The text starts with the file's chapters, or
 * those of the yt-dlp info.json / description beside it. A preview lists
 * the pieces with what each gets as tags, and a play button on each row
 * plays the original from that point, to hear that the cut is in the
 * right place. The cut itself is kiplay-audio --split (audio/split.h), in
 * its own process, with a progress bar; the original is never touched.
 * Optionally the pieces then take the original's place in the playlist.
 *
 * One dialog at a time, not modal (the player's controls stay at hand).
 * Memory: the dialog's state lives until the window is destroyed; the
 * entry is followed by its location string (kp_entry_ref), never kept as
 * a pointer to the entry. */
#include <errno.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "child.h"
#include "i18n.h"
#include "lineio.h"
#include "log.h"
#include "media.h"
#include "splitspec.h"
#include "util.h"

enum { TAGS_TEXT, TAGS_KEEP, TAGS_EACH, TAGS_COUNT };
enum { INTO_TITLE, INTO_ARTIST, INTO_ALBUM, INTO_COUNT };
enum { FOLDER_SAME, FOLDER_NEW, FOLDER_COUNT };
static const char *const tags_names[] = {"text", "keep", "each", NULL};
static const char *const into_names[] = {"title", "artist", "album", NULL};
static const char *const folder_names[] = {"same", "new", NULL};

/* The tags a piece gets: title, artist, album. */
enum { F_TITLE, F_ARTIST, F_ALBUM, F_COUNT };

enum { COL_NUM, COL_START, COL_LENGTH, COL_TITLE, COL_ARTIST, COL_ALBUM, COL_EDITABLE, N_COLS };

typedef struct {
    char *f[F_COUNT]; /* typed in the list (NULL: not touched) */
} edit_t;

typedef struct {
    kp_app *app;
    int id;               /* the entry: playlist id, */
    const char *location; /* its location string, */
    long hint;            /* and where it was */
    char *path;           /* the file */
    char *orig[F_COUNT];  /* the entry's tags ("" if unknown) */
    double duration;
    bool info_ok, exact;

    GtkWidget *dialog, *text, *status, *view, *mode_label;
    GtkWidget *tags_radio[TAGS_COUNT], *into, *artist_title;
    GtkWidget *folder_radio[FOLDER_COUNT], *names_label, *replace;
    GtkWidget *progress, *ok_btn, *form;
    GtkListStore *store;
    GtkTreeViewColumn *play_col;
    GArray *pieces;   /* kp_split_piece, of the text as it is now */
    GArray *edits;    /* edit_t per row */
    bool can_split;   /* the text and the names are fine */

    GCancellable *cancel; /* the --split-info question */
    kp_child *child;      /* the cut, while it runs */
    kp_lineio *io;
    bool running, finished;
    /* How it ended, handled on the main loop after the line or exit
     * callback that said so (they can't free what called them). */
    guint conclude_id;
    bool result_ok;
    char *why;
} split_dlg;

static split_dlg *open_dlg;

/* ---- what can be cut ------------------------------------------------------ */

/* The file of a location, if local and not a subtune. g_free it. */
static char *local_file(const char *location)
{
    char *loc = kp_media_location(location);
    char *file = NULL;
    if (g_str_has_prefix(loc, "file://"))
        file = strchr(loc, '?') ? NULL : g_filename_from_uri(loc, NULL, NULL);
    else if (loc[0] == '/')
        file = g_strdup(loc);
    g_free(loc);
    return file;
}

static const kp_entry *selected_entry(kp_app *app, int *id, long *index)
{
    *id = kp_playlists_selection(app, index);
    kp_playlist *pl = *id >= 0 && *index >= 0 ? kp_playlists_get(app, *id) : NULL;
    return pl ? kp_playlist_get(pl, (size_t)*index) : NULL;
}

bool kp_split_possible(kp_app *app)
{
    int id;
    long index;
    const kp_entry *e = selected_entry(app, &id, &index);
    if (!e || kp_media_kind_of(e->location) == KP_MEDIA_NONE)
        return false;
    char *file = local_file(e->location);
    bool ok = file != NULL;
    g_free(file);
    return ok;
}

/* ---- the pieces and their tags -------------------------------------------- */

static int tags_mode(split_dlg *d)
{
    for (int i = 0; i < TAGS_COUNT; i++)
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->tags_radio[i])))
            return i;
    return TAGS_TEXT;
}

static edit_t *edit_at(split_dlg *d, guint row)
{
    if (row >= d->edits->len)
        g_array_set_size(d->edits, row + 1);
    return &g_array_index(d->edits, edit_t, row);
}

/* What piece i is tagged with (strings owned by d or its pieces). An
 * empty field keeps the original's (and says so to the engine). */
static void piece_fields(split_dlg *d, guint i, const char *out[F_COUNT], char **tmp)
{
    const kp_split_piece *p = &g_array_index(d->pieces, kp_split_piece, i);
    for (int f = 0; f < F_COUNT; f++)
        out[f] = d->orig[f];
    tmp[0] = tmp[1] = NULL;
    int mode = tags_mode(d);
    if (mode == TAGS_KEEP)
        return;
    if (p->text[0]) {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->artist_title)) &&
            kp_split_artist_title(p->text, &tmp[0], &tmp[1])) {
            out[F_ARTIST] = tmp[0];
            out[F_TITLE] = tmp[1];
        } else {
            int into = gtk_combo_box_get_active(GTK_COMBO_BOX(d->into));
            out[into == INTO_ARTIST ? F_ARTIST : into == INTO_ALBUM ? F_ALBUM : F_TITLE] = p->text;
        }
    }
    if (mode == TAGS_EACH && i < d->edits->len) {
        const edit_t *e = &g_array_index(d->edits, edit_t, i);
        for (int f = 0; f < F_COUNT; f++)
            if (e->f[f])
                out[f] = e->f[f];
    }
}

static double piece_end(split_dlg *d, const kp_split_piece *p)
{
    return p->end >= 0 ? p->end : d->duration;
}

static char *dest_dir(split_dlg *d)
{
    char *dir = g_path_get_dirname(d->path);
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->folder_radio[FOLDER_NEW])))
        return dir;
    char *base = g_path_get_basename(d->path);
    char *dot = strrchr(base, '.');
    if (dot && dot != base)
        *dot = '\0';
    char *sub = g_build_filename(dir, base, NULL);
    g_free(base);
    g_free(dir);
    return sub;
}

static const char *extension(split_dlg *d)
{
    const char *slash = strrchr(d->path, '/'), *dot = strrchr(d->path, '.');
    return dot && dot > slash + 1 ? dot + 1 : "";
}

/* Where piece i goes. g_free it. */
static char *dest_path(split_dlg *d, const char *dir, guint i)
{
    const char *f[F_COUNT];
    char *tmp[2];
    piece_fields(d, i, f, tmp);
    char *name = kp_split_file_name((int)i + 1, (int)d->pieces->len, f[F_TITLE], extension(d));
    char *path = g_build_filename(dir, name, NULL);
    g_free(name);
    g_free(tmp[0]);
    g_free(tmp[1]);
    return path;
}

static void set_status(split_dlg *d, const char *text, bool bad)
{
    if (bad) {
        char *m = g_markup_printf_escaped("<span foreground=\"#c01c28\">%s</span>", text);
        gtk_label_set_markup(GTK_LABEL(d->status), m);
        g_free(m);
    } else
        gtk_label_set_text(GTK_LABEL(d->status), text);
}

static void update_ok(split_dlg *d)
{
    gtk_widget_set_sensitive(d->ok_btn, d->info_ok && d->can_split && !d->running);
}

/* The file names (the first ones, as an example), and none already there. */
static bool check_names(split_dlg *d)
{
    guint n = d->pieces->len;
    if (!n) {
        gtk_label_set_text(GTK_LABEL(d->names_label), "");
        return false;
    }
    char *dir = dest_dir(d);
    GString *eg = g_string_new(NULL);
    char *clash = NULL;
    for (guint i = 0; i < n; i++) {
        char *p = dest_path(d, dir, i);
        if (i < 2) {
            char *b = g_path_get_basename(p);
            g_string_append_printf(eg, "%s%s", i ? ", " : "", b);
            g_free(b);
        }
        if (!clash && g_file_test(p, G_FILE_TEST_EXISTS))
            clash = g_path_get_basename(p);
        g_free(p);
    }
    if (n > 2)
        g_string_append(eg, ", …");
    /* TRANSLATORS: the names the new files get, e.g. "Files: 01 - Intro.mp3, 02 - Song.mp3, …" */
    char *label = g_strdup_printf(_("Files: %s"), eg->str);
    gtk_label_set_text(GTK_LABEL(d->names_label), label);
    g_free(label);
    g_string_free(eg, TRUE);
    g_free(dir);
    if (clash) {
        char *m = g_strdup_printf(_("%s already exists: it would not be replaced."), clash);
        set_status(d, m, true);
        g_free(m);
        g_free(clash);
        return false;
    }
    return true;
}

static void fill_store(split_dlg *d)
{
    gtk_list_store_clear(d->store);
    bool each = tags_mode(d) == TAGS_EACH;
    for (guint i = 0; i < d->pieces->len; i++) {
        const kp_split_piece *p = &g_array_index(d->pieces, kp_split_piece, i);
        const char *f[F_COUNT];
        char *tmp[2];
        piece_fields(d, i, f, tmp);
        char *start = kp_split_format_time(p->start), len[32];
        kp_format_duration(len, sizeof len, (int64_t)((piece_end(d, p) - p->start) * 1000));
        GtkTreeIter it;
        gtk_list_store_append(d->store, &it);
        gtk_list_store_set(d->store, &it, COL_NUM, (int)i + 1, COL_START, start, COL_LENGTH,
                           d->duration > 0 || p->end >= 0 ? len : "", COL_TITLE, f[F_TITLE],
                           COL_ARTIST, f[F_ARTIST], COL_ALBUM, f[F_ALBUM], COL_EDITABLE, each, -1);
        g_free(start);
        g_free(tmp[0]);
        g_free(tmp[1]);
    }
}

/* The text changed (or how it is read): pieces, list, status. */
static void reparse(split_dlg *d)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->text));
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(buf, &a, &b);
    char *text = gtk_text_buffer_get_text(buf, &a, &b, FALSE);
    kp_split_result res;
    if (d->pieces)
        g_array_unref(d->pieces);
    d->pieces = kp_split_parse(text, d->duration, &res);
    g_free(text);
    fill_store(d);
    d->can_split = false;
    char *msg = NULL;
    switch (res.status) {
    case KP_SPLIT_OK:
        if (d->pieces->len < 2) {
            set_status(d, _("One track only: add the time where the next one starts."), false);
            break;
        }
        msg = g_strdup_printf(ngettext("%u track", "%u tracks", d->pieces->len), d->pieces->len);
        if (res.as_durations) {
            char *m2 = g_strdup_printf("%s %s", msg, _("(the times were read as lengths)"));
            g_free(msg);
            msg = m2;
        }
        set_status(d, msg, false);
        d->can_split = check_names(d);
        break;
    case KP_SPLIT_EMPTY:
        set_status(d, _("Type one time per line, such as \"0:00 Intro\"."), false);
        break;
    case KP_SPLIT_UNORDERED:
        msg = g_strdup_printf(_("Line %d: the time isn't after the one before."), res.line);
        set_status(d, msg, true);
        break;
    case KP_SPLIT_PAST_END: {
        char len[32];
        kp_format_duration(len, sizeof len, (int64_t)(d->duration * 1000));
        msg = g_strdup_printf(_("Line %d: the time is past the end of the file (%s)."), res.line, len);
        set_status(d, msg, true);
        break;
    }
    }
    g_free(msg);
    if (res.status != KP_SPLIT_OK || d->pieces->len < 2)
        check_names(d);
    update_ok(d);
}

static void on_text_changed(GtkTextBuffer *buf, gpointer data)
{
    (void)buf;
    reparse(data);
}

static void on_option(GtkWidget *w, gpointer data)
{
    (void)w;
    split_dlg *d = data;
    int mode = tags_mode(d);
    gtk_widget_set_sensitive(d->into, mode != TAGS_KEEP);
    gtk_widget_set_sensitive(d->artist_title, mode != TAGS_KEEP);
    reparse(d);
}

static void on_edited(GtkCellRendererText *r, gchar *path, gchar *text, gpointer data)
{
    split_dlg *d = data;
    int f = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "kp-field"));
    int row = atoi(path);
    if (row < 0 || (guint)row >= d->pieces->len)
        return;
    edit_t *e = edit_at(d, (guint)row);
    g_free(e->f[f]);
    e->f[f] = g_strdup(text);
    reparse(d);
}

/* ---- hearing a cut -------------------------------------------------------- */

static void play_piece(split_dlg *d, guint i)
{
    if (i >= d->pieces->len)
        return;
    kp_playlist *pl = kp_playlists_get(d->app, d->id);
    long at = pl ? kp_playlist_find_entry(pl, d->location, d->hint) : -1;
    if (at < 0) {
        set_status(d, _("The track isn't in the playlist any more."), true);
        return;
    }
    d->hint = at;
    kp_player_play_from(d->app, d->id, at, g_array_index(d->pieces, kp_split_piece, i).start);
}

static gboolean on_view_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    split_dlg *d = data;
    GtkTreePath *path;
    GtkTreeViewColumn *col;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 1 ||
        !gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), (int)ev->x, (int)ev->y, &path, &col, NULL,
                                       NULL))
        return FALSE;
    int row = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    if (col != d->play_col)
        return FALSE;
    play_piece(d, (guint)row);
    return TRUE;
}

static void on_row_activated(GtkTreeView *v, GtkTreePath *path, GtkTreeViewColumn *col,
                             gpointer data)
{
    (void)v;
    (void)col;
    play_piece(data, (guint)gtk_tree_path_get_indices(path)[0]);
}

static void on_stop(GtkWidget *w, gpointer data)
{
    (void)w;
    kp_player_stop(((split_dlg *)data)->app);
}

/* ---- the file's chapters and length (kiplay-audio --split-info) ------------ */

/* The engine's English reasons, for people. */
static const char *reason(const char *engine)
{
    if (!strcmp(engine, "the file has video"))
        return _("Files with video can't be split.");
    if (!strcmp(engine, "no sound in the file"))
        return _("The file has no sound.");
    if (!strcmp(engine, "not a recording"))
        return _("Game music and MIDI can't be split.");
    return engine;
}

static void set_text_if_empty(split_dlg *d, const char *text)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->text));
    if (gtk_text_buffer_get_char_count(buf) == 0 && text)
        gtk_text_buffer_set_text(buf, text, -1);
}

static void info_done(GObject *src, GAsyncResult *res, gpointer data)
{
    char *out = NULL;
    GError *err = NULL;
    bool ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL, &err);
    if (!ok && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err);
        return; /* the dialog is gone */
    }
    split_dlg *d = data;
    g_clear_object(&d->cancel);
    GArray *starts = g_array_new(FALSE, FALSE, sizeof(double));
    GPtrArray *titles = g_ptr_array_new_with_free_func(g_free);
    char *problem = ok ? NULL : g_strdup(err ? err->message : "?");
    char **lines = g_strsplit(out ? out : "", "\n", -1);
    for (int i = 0; lines[i]; i++) {
        const char *l = lines[i];
        if (g_str_has_prefix(l, "duration "))
            d->duration = g_ascii_strtod(l + 9, NULL);
        else if (g_str_has_prefix(l, "mode "))
            d->exact = !strcmp(l + 5, "exact");
        else if (g_str_has_prefix(l, "chapter ")) {
            char *end;
            double t = g_ascii_strtod(l + 8, &end);
            g_array_append_val(starts, t);
            g_ptr_array_add(titles, g_strdup(*end == '\t' ? end + 1 : ""));
        } else if (g_str_has_prefix(l, "error ") && !problem)
            problem = g_strdup(l + 6);
    }
    g_strfreev(lines);
    g_free(out);
    g_clear_error(&err);
    if (problem || d->duration <= 0) {
        char *m = g_strdup_printf(_("Can't split this file: %s"),
                                  problem ? reason(problem) : _("its length is unknown."));
        set_status(d, m, true);
        g_free(m);
        gtk_label_set_text(GTK_LABEL(d->mode_label), "");
    } else {
        d->info_ok = true;
        gtk_label_set_text(GTK_LABEL(d->mode_label),
                           d->exact ? _("Lossless format: cut on the exact sample, no loss.")
                                    : _("Cut without re-encoding, so no loss: each cut lands "
                                        "within some 25 ms of its time."));
        g_ptr_array_add(titles, NULL);
        char *text = kp_split_text_for(d->path, (const double *)(void *)starts->data,
                                       (char *const *)titles->pdata, (int)starts->len);
        set_text_if_empty(d, text);
        g_free(text);
        reparse(d);
    }
    g_array_unref(starts);
    g_ptr_array_unref(titles);
    update_ok(d);
}

static void ask_info(split_dlg *d)
{
    char *engine = kp_engine_path();
    const char *argv[] = {engine, "--split-info", d->path, NULL};
    GError *err = NULL;
    GSubprocess *p = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE, &err);
    g_free(engine);
    if (!p) {
        char *m = g_strdup_printf(_("Can't split this file: %s"), err->message);
        set_status(d, m, true);
        g_free(m);
        g_error_free(err);
        return;
    }
    d->cancel = g_cancellable_new();
    g_subprocess_communicate_utf8_async(p, NULL, d->cancel, info_done, d);
    g_object_unref(p);
}

/* ---- the cut -------------------------------------------------------------- */

static void set_running(split_dlg *d, bool on)
{
    d->running = on;
    gtk_widget_set_sensitive(d->form, !on);
    if (on) {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(d->progress), 0);
        gtk_widget_show(d->progress);
    } else
        gtk_widget_hide(d->progress);
    update_ok(d);
}

static void stop_child(split_dlg *d)
{
    if (d->io) {
        kp_lineio_free(d->io);
        d->io = NULL;
    }
    if (d->child) {
        kp_child_stop(d->child);
        d->child = NULL;
    }
}

static void save_prefs(split_dlg *d)
{
    kp_prefs *p = d->app->prefs;
    kp_prefs_set_str(p, "split", "tags", tags_names[tags_mode(d)]);
    kp_prefs_set_str(p, "split", "into", into_names[gtk_combo_box_get_active(GTK_COMBO_BOX(d->into))]);
    kp_prefs_set_bool(p, "split", "artist_title",
                      gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->artist_title)));
    kp_prefs_set_str(p, "split", "folder",
                     folder_names[gtk_toggle_button_get_active(
                         GTK_TOGGLE_BUTTON(d->folder_radio[FOLDER_NEW]))]);
    kp_prefs_set_bool(p, "split", "replace",
                      gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->replace)));
    int w, h;
    gtk_window_get_size(GTK_WINDOW(d->dialog), &w, &h);
    kp_prefs_set_int(p, "split", "width", w);
    kp_prefs_set_int(p, "split", "height", h);
}

/* The pieces are there: into the playlist, if wanted, and done. */
static void succeeded(split_dlg *d)
{
    guint n = d->pieces->len;
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->replace))) {
        char *dir = dest_dir(d);
        char **locs = g_new0(char *, n + 1);
        kp_tags *tags = g_new0(kp_tags, n);
        char **tmp = g_new0(char *, 2 * n);
        long heard = -1;
        const kp_playing *pl = &d->app->playing;
        bool plays = pl->loaded && pl->playlist_id == d->id && pl->location == d->location;
        for (guint i = 0; i < n; i++) {
            const kp_split_piece *p = &g_array_index(d->pieces, kp_split_piece, i);
            const char *f[F_COUNT];
            piece_fields(d, i, f, tmp + 2 * i);
            locs[i] = dest_path(d, dir, i);
            tags[i] = (kp_tags){f[F_TITLE][0] ? f[F_TITLE] : NULL,
                                f[F_ARTIST][0] ? f[F_ARTIST] : NULL,
                                f[F_ALBUM][0] ? f[F_ALBUM] : NULL,
                                (int64_t)((piece_end(d, p) - p->start) * 1000)};
            if (plays && pl->position >= p->start)
                heard = (long)i;
        }
        if (!kp_playlists_replace(d->app, d->id, d->location, d->hint, locs, tags, n, heard))
            kp_player_message(d->app, _("The tracks were made, but the original isn't in the "
                                        "playlist any more."));
        for (guint i = 0; i < 2 * n; i++)
            g_free(tmp[i]);
        g_free(tmp);
        g_free(tags);
        g_strfreev(locs);
        g_free(dir);
    }
    char *m = g_strdup_printf(ngettext("Split into %u track.", "Split into %u tracks.", n), n);
    kp_player_message(d->app, m);
    g_free(m);
    save_prefs(d);
    gtk_widget_destroy(d->dialog);
}

static void failed(split_dlg *d, const char *why)
{
    set_running(d, false);
    char *m = g_strdup_printf(_("The file wasn't split: %s"), reason(why));
    set_status(d, m, true);
    g_free(m);
}

static gboolean conclude(gpointer data)
{
    split_dlg *d = data;
    d->conclude_id = 0;
    stop_child(d);
    if (d->result_ok)
        succeeded(d); /* destroys the dialog */
    else
        failed(d, d->why);
    return G_SOURCE_REMOVE;
}

static void finish(split_dlg *d, bool ok, const char *why)
{
    if (d->finished)
        return;
    d->finished = true;
    d->result_ok = ok;
    g_free(d->why);
    d->why = g_strdup(why ? why : "");
    d->conclude_id = g_idle_add(conclude, d);
}

static void on_line(void *ud, const char *line)
{
    split_dlg *d = ud;
    if (d->finished)
        return;
    if (!line)
        finish(d, false, _("the engine stopped."));
    else if (g_str_has_prefix(line, "progress ")) {
        double f = g_ascii_strtod(line + 9, NULL);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(d->progress), CLAMP(f, 0, 1));
    } else if (!strcmp(line, "ok"))
        finish(d, true, NULL);
    else if (g_str_has_prefix(line, "error "))
        finish(d, false, line + 6);
}

static void on_child_exit(void *ud, const char *why)
{
    finish(ud, false, why);
}

/* A tag on the protocol's line: no tab, no line break. */
static char *field(const char *s)
{
    char *f = g_strdup(s ? s : "");
    for (char *c = f; *c; c++)
        if (*c == '\t' || *c == '\n' || *c == '\r')
            *c = ' ';
    return f;
}

static void start_split(split_dlg *d)
{
    char *dir = dest_dir(d);
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        char *m = g_strdup_printf(_("Can't make the folder %s: %s"), dir, g_strerror(errno));
        set_status(d, m, true);
        g_free(m);
        g_free(dir);
        return;
    }
    char *engine = kp_engine_path();
    const char *argv[] = {engine, "--split", NULL};
    char *err = NULL;
    d->finished = false;
    d->child = kp_child_spawn(argv, G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE,
                              on_child_exit, d, &err);
    g_free(engine);
    if (!d->child) {
        set_running(d, false);
        failed(d, err ? err : "?");
        g_free(err);
        g_free(dir);
        return;
    }
    GSubprocess *proc = kp_child_proc(d->child);
    d->io = kp_lineio_new("split", on_line, d);
    kp_lineio_attach(d->io, g_subprocess_get_stdout_pipe(proc), g_subprocess_get_stdin_pipe(proc));
    set_running(d, true);
    set_status(d, _("Splitting…"), false);

    bool keep = tags_mode(d) == TAGS_KEEP;
    char *line = g_strdup_printf("src %s", d->path);
    kp_lineio_send(d->io, line);
    g_free(line);
    for (guint i = 0; i < d->pieces->len; i++) {
        const kp_split_piece *p = &g_array_index(d->pieces, kp_split_piece, i);
        const char *f[F_COUNT];
        char *tmp[2], *fl[F_COUNT];
        piece_fields(d, i, f, tmp);
        /* Unchanged fields stay as the file has them (the playlist may
         * show a title the file doesn't have). */
        for (int k = 0; k < F_COUNT; k++)
            fl[k] = field(keep || f[k] == d->orig[k] ? "" : f[k]);
        char start[G_ASCII_DTOSTR_BUF_SIZE], end[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd(start, sizeof start, "%.6f", p->start);
        g_ascii_formatd(end, sizeof end, "%.6f", i + 1 < d->pieces->len ? p->end : -1.0);
        char *dest = dest_path(d, dir, i);
        line = g_strdup_printf("piece %s\t%s\t%s\t%s\t%s\t%s", start, end, fl[F_TITLE],
                               fl[F_ARTIST], fl[F_ALBUM], dest);
        kp_lineio_send(d->io, line);
        g_free(line);
        g_free(dest);
        for (int k = 0; k < F_COUNT; k++)
            g_free(fl[k]);
        g_free(tmp[0]);
        g_free(tmp[1]);
    }
    kp_lineio_send(d->io, "go");
    g_free(dir);
}

/* ---- the window ----------------------------------------------------------- */

static void on_response(GtkDialog *dlg, gint response, gpointer data)
{
    split_dlg *d = data;
    if (response == GTK_RESPONSE_OK) {
        if (!d->running && d->info_ok && d->can_split)
            start_split(d);
        return;
    }
    if (d->running) {
        /* Cancel stops the cut (the engine takes its .part files away)
         * and leaves the window open. */
        d->finished = true;
        stop_child(d);
        set_running(d, false);
        set_status(d, _("Cancelled."), false);
        if (response == GTK_RESPONSE_CANCEL)
            return;
    }
    save_prefs(d);
    gtk_widget_destroy(GTK_WIDGET(dlg));
}

static void on_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    split_dlg *d = data;
    if (d->cancel) {
        g_cancellable_cancel(d->cancel);
        g_clear_object(&d->cancel);
    }
    d->finished = true;
    if (d->conclude_id)
        g_source_remove(d->conclude_id);
    g_free(d->why);
    stop_child(d);
    if (d->pieces)
        g_array_unref(d->pieces);
    for (guint i = 0; i < d->edits->len; i++)
        for (int f = 0; f < F_COUNT; f++)
            g_free(g_array_index(d->edits, edit_t, i).f[f]);
    g_array_unref(d->edits);
    for (int f = 0; f < F_COUNT; f++)
        g_free(d->orig[f]);
    g_object_unref(d->store);
    g_free(d->path);
    if (open_dlg == d)
        open_dlg = NULL;
    g_free(d);
}

static GtkWidget *bold_label(const char *text)
{
    GtkWidget *l = gtk_label_new(NULL);
    char *m = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(l), m);
    g_free(m);
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    return l;
}

static GtkWidget *wrapped_label(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    return l;
}

static void add_text_column(split_dlg *d, const char *title, int col, int field_index, bool expand)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(title, r, "text", col, NULL);
    if (field_index >= 0) {
        gtk_tree_view_column_add_attribute(c, r, "editable", COL_EDITABLE);
        g_object_set_data(G_OBJECT(r), "kp-field", GINT_TO_POINTER(field_index));
        g_signal_connect(r, "edited", G_CALLBACK(on_edited), d);
        g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        gtk_tree_view_column_set_resizable(c, TRUE);
        gtk_tree_view_column_set_min_width(c, 80);
    } else
        g_object_set(r, "xalign", 1.0, NULL);
    gtk_tree_view_column_set_expand(c, expand);
    gtk_tree_view_append_column(GTK_TREE_VIEW(d->view), c);
}

static GtkWidget *build_preview(split_dlg *d)
{
    d->store = gtk_list_store_new(N_COLS, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
    d->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(d->store));
    gtk_tree_view_set_rules_hint(GTK_TREE_VIEW(d->view), TRUE);
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    g_object_set(icon, "stock-id", GTK_STOCK_MEDIA_PLAY, "stock-size", GTK_ICON_SIZE_MENU, NULL);
    d->play_col = gtk_tree_view_column_new_with_attributes("", icon, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(d->view), d->play_col);
    add_text_column(d, "#", COL_NUM, -1, false);
    add_text_column(d, _("Start"), COL_START, -1, false);
    add_text_column(d, _("Length"), COL_LENGTH, -1, false);
    add_text_column(d, _("Title"), COL_TITLE, F_TITLE, true);
    add_text_column(d, _("Artist"), COL_ARTIST, F_ARTIST, false);
    add_text_column(d, _("Album"), COL_ALBUM, F_ALBUM, false);
    gtk_widget_set_tooltip_text(d->view, _("Click ▶ (or double-click a row) to hear the original "
                                           "from where that track starts."));
    g_signal_connect(d->view, "button-press-event", G_CALLBACK(on_view_press), d);
    g_signal_connect(d->view, "row-activated", G_CALLBACK(on_row_activated), d);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), d->view);
    return sw;
}

static GtkWidget *build_tags(split_dlg *d)
{
    kp_prefs *p = d->app->prefs;
    GtkWidget *box = gtk_vbox_new(FALSE, 2);
    GtkWidget *row = gtk_hbox_new(FALSE, 6);
    d->tags_radio[TAGS_TEXT] = gtk_radio_button_new_with_mnemonic(NULL, _("Use the _text, as the"));
    d->into = gtk_combo_box_new_text();
    gtk_combo_box_append_text(GTK_COMBO_BOX(d->into), _("Title"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(d->into), _("Artist"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(d->into), _("Album"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(d->into),
                             kp_prefs_get_choice(p, "split", "into", into_names, INTO_TITLE));
    d->artist_title = gtk_check_button_new_with_mnemonic(_("\"Artist - Title\" fills _both"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->artist_title),
                                 kp_prefs_get_bool(p, "split", "artist_title", false));
    gtk_box_pack_start(GTK_BOX(row), d->tags_radio[TAGS_TEXT], FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), d->into, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), d->artist_title, FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    GSList *g = gtk_radio_button_get_group(GTK_RADIO_BUTTON(d->tags_radio[TAGS_TEXT]));
    d->tags_radio[TAGS_KEEP] = gtk_radio_button_new_with_mnemonic(g, _("_Keep the original's tags"));
    g = gtk_radio_button_get_group(GTK_RADIO_BUTTON(d->tags_radio[TAGS_KEEP]));
    d->tags_radio[TAGS_EACH] =
        gtk_radio_button_new_with_mnemonic(g, _("Type them _one by one (click a cell in the list)"));
    gtk_box_pack_start(GTK_BOX(box), d->tags_radio[TAGS_KEEP], FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), d->tags_radio[TAGS_EACH], FALSE, FALSE, 0);
    int mode = kp_prefs_get_choice(p, "split", "tags", tags_names, TAGS_TEXT);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->tags_radio[mode]), TRUE);
    for (int i = 0; i < TAGS_COUNT; i++)
        g_signal_connect(d->tags_radio[i], "toggled", G_CALLBACK(on_option), d);
    g_signal_connect(d->into, "changed", G_CALLBACK(on_option), d);
    g_signal_connect(d->artist_title, "toggled", G_CALLBACK(on_option), d);
    return box;
}

static GtkWidget *build_output(split_dlg *d)
{
    kp_prefs *p = d->app->prefs;
    GtkWidget *box = gtk_vbox_new(FALSE, 2);
    GtkWidget *row = gtk_hbox_new(FALSE, 6);
    d->folder_radio[FOLDER_SAME] = gtk_radio_button_new_with_mnemonic(NULL, _("In the file's _folder"));
    GSList *g = gtk_radio_button_get_group(GTK_RADIO_BUTTON(d->folder_radio[FOLDER_SAME]));
    d->folder_radio[FOLDER_NEW] =
        gtk_radio_button_new_with_mnemonic(g, _("In a _new folder named after the file"));
    gtk_box_pack_start(GTK_BOX(row), d->folder_radio[FOLDER_SAME], FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), d->folder_radio[FOLDER_NEW], FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    int folder = kp_prefs_get_choice(p, "split", "folder", folder_names, FOLDER_SAME);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->folder_radio[folder]), TRUE);
    for (int i = 0; i < FOLDER_COUNT; i++)
        g_signal_connect(d->folder_radio[i], "toggled", G_CALLBACK(on_option), d);
    d->names_label = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(d->names_label), 0, 0.5);
    gtk_label_set_ellipsize(GTK_LABEL(d->names_label), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(box), d->names_label, FALSE, FALSE, 0);
    d->replace = gtk_check_button_new_with_mnemonic(
        _("_Replace the original in the playlist with the tracks, in its place"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->replace),
                                 kp_prefs_get_bool(p, "split", "replace", true));
    gtk_box_pack_start(GTK_BOX(box), d->replace, FALSE, FALSE, 0);
    return box;
}

void kp_split_dialog(kp_app *app)
{
    if (open_dlg) {
        gtk_window_present(GTK_WINDOW(open_dlg->dialog));
        return;
    }
    int id;
    long index;
    const kp_entry *e = selected_entry(app, &id, &index);
    char *path = e ? local_file(e->location) : NULL;
    if (!path)
        return;
    split_dlg *d = open_dlg = g_new0(split_dlg, 1);
    d->app = app;
    d->id = id;
    d->location = e->location;
    d->hint = index;
    d->path = path;
    d->orig[F_TITLE] = g_strdup(e->title ? e->title : "");
    d->orig[F_ARTIST] = g_strdup(e->artist ? e->artist : "");
    d->orig[F_ALBUM] = g_strdup(e->album ? e->album : "");
    d->duration = e->length_ms > 0 ? (double)e->length_ms / 1000 : 0;
    d->edits = g_array_new(FALSE, TRUE, sizeof(edit_t));

    char *base = g_path_get_basename(path);
    char *title = g_strdup_printf(_("Split into tracks: %s"), base);
    g_free(base);
    d->dialog = gtk_dialog_new_with_buttons(title, GTK_WINDOW(app->window),
                                            GTK_DIALOG_DESTROY_WITH_PARENT, GTK_STOCK_CANCEL,
                                            GTK_RESPONSE_CANCEL, NULL);
    g_free(title);
    d->ok_btn = gtk_dialog_add_button(GTK_DIALOG(d->dialog), _("_Split"), GTK_RESPONSE_OK);
    gtk_button_set_image(GTK_BUTTON(d->ok_btn),
                         gtk_image_new_from_stock(GTK_STOCK_CUT, GTK_ICON_SIZE_BUTTON));
    gtk_dialog_set_default_response(GTK_DIALOG(d->dialog), GTK_RESPONSE_OK);
    gtk_window_set_default_size(GTK_WINDOW(d->dialog),
                                kp_prefs_get_int(app->prefs, "split", "width", 640, 300, 4000),
                                kp_prefs_get_int(app->prefs, "split", "height", 620, 300, 4000));

    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(d->dialog));
    d->form = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(d->form), 8);
    gtk_box_pack_start(GTK_BOX(area), d->form, TRUE, TRUE, 0);
    GtkWidget *f = d->form;

    gtk_box_pack_start(GTK_BOX(f), bold_label(_("Times")), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(f),
                       wrapped_label(_("One line per track: where it starts and its title, as in "
                                       "video descriptions (\"0:00 Intro\", \"1:02:03 - Title\").")),
                       FALSE, FALSE, 0);
    d->text = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(d->text), GTK_WRAP_WORD_CHAR);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), d->text);
    gtk_widget_set_size_request(sw, -1, 110);
    GtkWidget *paned = gtk_vpaned_new();
    gtk_paned_pack1(GTK_PANED(paned), sw, TRUE, FALSE);

    GtkWidget *prev = gtk_vbox_new(FALSE, 4);
    d->status = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(d->status), 0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(d->status), TRUE);
    gtk_box_pack_start(GTK_BOX(prev), d->status, FALSE, FALSE, 2);
    GtkWidget *head = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(head), bold_label(_("Tracks")), TRUE, TRUE, 0);
    GtkWidget *stop = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(stop), gtk_image_new_from_stock(GTK_STOCK_MEDIA_STOP,
                                                                    GTK_ICON_SIZE_MENU));
    gtk_button_set_relief(GTK_BUTTON(stop), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(stop, _("Stop"));
    g_signal_connect(stop, "clicked", G_CALLBACK(on_stop), d);
    gtk_box_pack_end(GTK_BOX(head), stop, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(prev), head, FALSE, FALSE, 0);
    GtkWidget *list = build_preview(d);
    gtk_widget_set_size_request(list, -1, 120);
    gtk_box_pack_start(GTK_BOX(prev), list, TRUE, TRUE, 0);
    gtk_paned_pack2(GTK_PANED(paned), prev, TRUE, FALSE);
    gtk_box_pack_start(GTK_BOX(f), paned, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(f), bold_label(_("Tags")), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(f), build_tags(d), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(f), bold_label(_("Files")), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(f), build_output(d), FALSE, FALSE, 0);
    d->mode_label = wrapped_label(_("Reading the file…"));
    gtk_box_pack_start(GTK_BOX(f), d->mode_label, FALSE, FALSE, 4);

    d->progress = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(area), d->progress, FALSE, FALSE, 4);

    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->text));
    g_signal_connect(buf, "changed", G_CALLBACK(on_text_changed), d);
    g_signal_connect(d->dialog, "response", G_CALLBACK(on_response), d);
    g_signal_connect(d->dialog, "destroy", G_CALLBACK(on_destroy), d);
    gtk_widget_show_all(d->dialog);
    gtk_widget_hide(d->progress);
    on_option(NULL, d);
    gtk_widget_grab_focus(d->text);
    ask_info(d);
}
