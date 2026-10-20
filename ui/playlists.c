/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <gdk/gdkkeysyms.h>
#include <string.h>

#include "app.h"
#include "i18n.h"
#include "log.h"
#include "media.h"
#include "plmodel.h"
#include "util.h"

/* The tabs. Each notebook page is a scrolled window carrying a page_t
 * ("kp-page"); its playlist is shown through a KpPlModel (plmodel.h), and
 * the tree view itself is only made the first time the tab is shown: with
 * twenty tabs, nineteen views of thousands of rows would cost memory and
 * start-up time for nothing.
 *
 * Tags: rows that appear on screen with nothing known, and every file
 * added, go to the tag reader (src/tagreader.h); answers update the row
 * and are saved with the playlist. */

const char *const kp_column_names[] = {"number", "artist", "album", "length", "path", NULL};

typedef struct {
    int id;
    KpPlModel *model;
    GtkWidget *scroll; /* the notebook page */
    GtkWidget *view;   /* NULL until the tab is first shown */
    GtkWidget *tab_icon, *tab_label;
    /* GTK 2 drops a multiple selection when a drag starts on it: a press
     * on a selected row leaves the selection alone until the release. */
    bool hold_selection, dragging;
} page_t;

/* The columns: model column, what kiplay.ini calls it (width_<key>),
 * header, alignment, default width. The title is the one that grows. */
static const struct {
    unsigned bit; /* 0: always shown */
    int col;
    const char *key, *header;
    float xalign;
    int width;
} columns[] = {
    {KP_COLUMN_NUM, KP_COL_NUM, "number", "#", 1.0f, 56},
    {0, KP_COL_TITLE, "title", N_("Title"), 0.0f, 200},
    {KP_COLUMN_ARTIST, KP_COL_ARTIST, "artist", N_("Artist"), 0.0f, 150},
    {KP_COLUMN_ALBUM, KP_COL_ALBUM, "album", N_("Album"), 0.0f, 150},
    {KP_COLUMN_LENGTH, KP_COL_LENGTH, "length", N_("Length"), 1.0f, 64},
    {KP_COLUMN_PATH, KP_COL_PATH, "path", N_("Path"), 0.0f, 300},
};

static guint save_timer, status_timer;

static gboolean save_now(gpointer data)
{
    save_timer = 0;
    kp_playlists_save(data);
    return G_SOURCE_REMOVE;
}

/* Changes are written a moment later, in one go. */
static void schedule_save(kp_app *app)
{
    if (!save_timer)
        save_timer = g_timeout_add_seconds(1, save_now, app);
}

void kp_playlists_changed(kp_app *app)
{
    kp_search_panel_changed(app);
    schedule_save(app);
}

/* ---- pages --------------------------------------------------------------- */

static page_t *page_of(GtkWidget *scroll)
{
    return scroll ? g_object_get_data(G_OBJECT(scroll), "kp-page") : NULL;
}

static page_t *page_at(kp_app *app, int n)
{
    return page_of(gtk_notebook_get_nth_page(GTK_NOTEBOOK(app->notebook), n));
}

static page_t *page_for(kp_app *app, int id)
{
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(app->notebook));
    for (int i = 0; i < n; i++) {
        page_t *pg = page_at(app, i);
        if (pg && pg->id == id)
            return pg;
    }
    return NULL;
}

static page_t *current_page(kp_app *app)
{
    int n = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
    return n >= 0 ? page_at(app, n) : NULL;
}

static kp_playlist *list_of(page_t *pg)
{
    return kp_plmodel_playlist(pg->model);
}

kp_playlist *kp_playlists_get(kp_app *app, int id)
{
    long i = kp_plset_find(app->playlists, id);
    return i >= 0 ? kp_plset_get(app->playlists, (size_t)i) : NULL;
}

kp_playlist *kp_playlists_visible(kp_app *app, int *id)
{
    page_t *pg = current_page(app);
    if (id)
        *id = pg ? pg->id : -1;
    return pg ? list_of(pg) : NULL;
}

/* ---- saving -------------------------------------------------------------- */

static void save_widths(kp_app *app, page_t *pg)
{
    if (!pg || !pg->view)
        return;
    for (size_t c = 0; c < G_N_ELEMENTS(columns); c++) {
        GtkTreeViewColumn *col = gtk_tree_view_get_column(GTK_TREE_VIEW(pg->view), (int)c);
        if (!col || !gtk_tree_view_column_get_visible(col) || columns[c].col == KP_COL_TITLE)
            continue;
        char key[32];
        g_snprintf(key, sizeof key, "width_%s", columns[c].key);
        kp_prefs_set_int(app->prefs, "playlists", key, gtk_tree_view_column_get_width(col));
    }
}

void kp_playlists_save(kp_app *app)
{
    if (save_timer) {
        g_source_remove(save_timer);
        save_timer = 0;
    }
    if (kp_plset_save(app->playlists) < 0)
        kp_log_warn(_("could not save the playlists: %s"), g_strerror(errno));
    char *order = kp_plset_order(app->playlists);
    kp_prefs_set_str(app->prefs, "playlists", "order", order);
    g_free(order);
    page_t *pg = current_page(app);
    if (pg) {
        kp_prefs_set_int(app->prefs, "playlists", "active", pg->id);
        save_widths(app, pg);
    }
}

/* ---- status line --------------------------------------------------------- */

void kp_playlists_update_status(kp_app *app)
{
    page_t *pg = current_page(app);
    if (!pg) {
        gtk_label_set_text(GTK_LABEL(app->status), "");
        return;
    }
    kp_playlist *pl = list_of(pg);
    size_t n = kp_playlist_count(pl), unknown;
    char total[32];
    kp_format_duration(total, sizeof total, kp_playlist_total_ms(pl, &unknown));
    char *text = n == 0 ? g_strdup(_("Empty playlist"))
                 : unknown == n
                     ? g_strdup_printf(ngettext("%zu track", "%zu tracks", n), n)
                     : g_strdup_printf(ngettext("%zu track, %s", "%zu tracks, %s", n), n, total);
    size_t reading = app->tags ? kp_tagreader_pending(app->tags) : 0;
    if (reading) {
        char *more = g_strdup_printf(ngettext("%s — reading tags (%zu file left)",
                                              "%s — reading tags (%zu files left)", reading),
                                     text, reading);
        g_free(text);
        text = more;
    }
    gtk_label_set_text(GTK_LABEL(app->status), text);
    g_free(text);
}

static gboolean status_now(gpointer data)
{
    status_timer = 0;
    kp_playlists_update_status(data);
    return G_SOURCE_REMOVE;
}

/* Tags arrive by the hundred: the status follows a few times a second. */
static void status_soon(kp_app *app)
{
    if (!status_timer)
        status_timer = g_timeout_add(250, status_now, app);
}

/* ---- tags ---------------------------------------------------------------- */

static bool is_local(const char *location)
{
    return location[0] == '/' || g_str_has_prefix(location, "file://");
}

static void read_tags(kp_app *app, page_t *pg, long index)
{
    kp_playlist *pl = list_of(pg);
    const kp_entry *e = kp_playlist_get(pl, (size_t)index);
    if (!e || e->scanned)
        return;
    kp_playlist_set_scanned(pl, (size_t)index);
    if (app->tags && is_local(e->location)) {
        kp_tagreader_add(app->tags, pg->id, index, e->location);
        status_soon(app);
    }
}

static void on_need_tags(void *ud, KpPlModel *m, long index)
{
    kp_app *app = ud;
    page_t *pg = page_for(app, kp_plmodel_id(m));
    if (pg)
        read_tags(app, pg, index);
}

static void on_tags(void *ud, int id, long index, const char *location, const kp_tags *tags)
{
    kp_app *app = ud;
    status_soon(app);
    page_t *pg = tags ? page_for(app, id) : NULL;
    if (!pg)
        return;
    kp_playlist *pl = list_of(pg);
    /* The playlist may have changed since the request. */
    const kp_entry *e = kp_playlist_get(pl, (size_t)index);
    if (!e || strcmp(e->location, location) != 0) {
        index = -1;
        for (size_t i = 0; i < kp_playlist_count(pl) && index < 0; i++)
            if (!strcmp(kp_playlist_get(pl, i)->location, location))
                index = (long)i;
        if (index < 0)
            return;
    }
    if (!kp_playlist_set_tags(pl, (size_t)index, tags))
        return;
    kp_plmodel_changed(pg->model, index);
    schedule_save(app);
    if (id == app->playing.playlist_id && index == app->playing.index)
        kp_player_info_changed(app);
}

static int on_queue_pos(void *ud, const kp_entry *e)
{
    kp_app *app = ud;
    return app->queue->len ? kp_player_queue_pos(app, -1, e) : 0;
}

/* ---- rows ---------------------------------------------------------------- */

void kp_playlists_mark_playing(kp_app *app, int id, long index)
{
    static int marked_id = -1;
    page_t *old = page_for(app, marked_id);
    if (old) {
        kp_plmodel_set_playing(old->model, -1);
        gtk_widget_hide(old->tab_icon);
    }
    marked_id = index >= 0 ? id : -1;
    page_t *pg = page_for(app, id);
    if (pg && index >= 0) {
        kp_plmodel_set_playing(pg->model, index);
        gtk_widget_show(pg->tab_icon);
    }
}

void kp_playlists_scroll_to_playing(kp_app *app, int id, long index)
{
    page_t *pg = page_for(app, id);
    /* A list nobody has opened has no view yet: it starts at the top
     * and nothing is lost. */
    if (!pg || !pg->view || index < 0 || (size_t)index >= kp_playlist_count(list_of(pg)))
        return;
    GtkTreePath *p = gtk_tree_path_new_from_indices((int)index, -1);
    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(pg->view), p, NULL, TRUE, 0.5f, 0.0f);
    gtk_tree_path_free(p);
}

void kp_playlists_refresh_row(kp_app *app, int id, long index)
{
    page_t *pg = page_for(app, id);
    if (pg)
        kp_plmodel_changed(pg->model, index);
    kp_playlists_update_status(app);
}

void kp_playlists_queue_changed(kp_app *app)
{
    page_t *pg = current_page(app);
    if (pg && pg->view)
        gtk_widget_queue_draw(pg->view);
}

/* Selected rows of a view, ascending; *n of them. g_free() the array. */
static size_t *selected_rows(page_t *pg, size_t *n)
{
    *n = 0;
    if (!pg->view)
        return NULL;
    GList *rows = gtk_tree_selection_get_selected_rows(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(pg->view)), NULL);
    size_t *out = g_new(size_t, g_list_length(rows) + 1);
    for (GList *r = rows; r; r = r->next)
        out[(*n)++] = (size_t)gtk_tree_path_get_indices(r->data)[0];
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    /* GTK gives them in order already; don't count on it. */
    for (size_t i = 1; i < *n; i++)
        for (size_t j = i; j > 0 && out[j - 1] > out[j]; j--) {
            size_t t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return out;
}

int kp_playlists_selection(kp_app *app, long *index)
{
    *index = -1;
    page_t *pg = current_page(app);
    if (!pg)
        return -1;
    size_t n;
    size_t *rows = selected_rows(pg, &n);
    if (n)
        *index = (long)rows[0];
    g_free(rows);
    return pg->id;
}

static void ensure_view(kp_app *app, page_t *pg);

static void select_rows(page_t *pg, long first, long n)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(pg->view));
    gtk_tree_selection_unselect_all(sel);
    if (n <= 0)
        return;
    GtkTreePath *a = gtk_tree_path_new_from_indices((int)first, -1);
    GtkTreePath *b = gtk_tree_path_new_from_indices((int)(first + n - 1), -1);
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(pg->view), a, NULL, FALSE);
    gtk_tree_selection_select_range(sel, a, b);
    gtk_tree_path_free(a);
    gtk_tree_path_free(b);
}

void kp_playlists_focus(kp_app *app)
{
    page_t *pg = current_page(app);
    if (pg && pg->view)
        gtk_widget_grab_focus(pg->view);
}

void kp_playlists_select(kp_app *app, int id, long index)
{
    page_t *pg = page_for(app, id);
    if (!pg || index < 0 || (size_t)index >= kp_playlist_count(list_of(pg)))
        return;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook),
                                  gtk_notebook_page_num(GTK_NOTEBOOK(app->notebook), pg->scroll));
    ensure_view(app, pg);
    select_rows(pg, index, 1);
    GtkTreePath *p = gtk_tree_path_new_from_indices((int)index, -1);
    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(pg->view), p, NULL, TRUE, 0.5f, 0.0f);
    gtk_tree_path_free(p);
    gtk_widget_grab_focus(pg->view);
}

/* ---- adding -------------------------------------------------------------- */

/* Adding runs in a thread: folders are walked and playlist files read
 * there, so a big folder doesn't freeze the window. What comes back is a
 * flat list, in order. */
typedef struct {
    char *location;
    char *title, *artist, *album;
    int64_t length_ms;
} found;

typedef struct {
    kp_app *app;
    int id;
    long at; /* -1: at the end */
    bool dedupe, play;
    char **uris;
    GArray *out; /* found */
} add_job;

static void found_clear(found *f)
{
    g_free(f->location);
    g_free(f->title);
    g_free(f->artist);
    g_free(f->album);
}

static void push(GArray *out, const char *location, const kp_tags *t)
{
    found f = {g_strdup(location), NULL, NULL, NULL, -1};
    if (t) {
        f.title = g_strdup(t->title);
        f.artist = g_strdup(t->artist);
        f.album = g_strdup(t->album);
        f.length_ms = t->length_ms;
    }
    g_array_append_val(out, f);
}

typedef struct {
    char *key, *name;
} dir_item;

static gint cmp_items(gconstpointer a, gconstpointer b)
{
    return strcmp(((const dir_item *)a)->key, ((const dir_item *)b)->key);
}

/* Media files under dir, by name ("2 x" before "10 x"), depth first. */
static void walk(const char *dir, GArray *out, int depth)
{
    if (depth > 32)
        return; /* a link loop */
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d)
        return;
    GArray *items = g_array_new(FALSE, FALSE, sizeof(dir_item));
    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (name[0] == '.')
            continue;
        char *display = g_filename_display_name(name);
        dir_item it = {g_utf8_collate_key_for_filename(display, -1), g_strdup(name)};
        g_array_append_val(items, it);
        g_free(display);
    }
    g_dir_close(d);
    g_array_sort(items, cmp_items);
    for (guint i = 0; i < items->len; i++) {
        dir_item *it = &g_array_index(items, dir_item, i);
        char *path = g_build_filename(dir, it->name, NULL);
        if (g_file_test(path, G_FILE_TEST_IS_DIR))
            walk(path, out, depth + 1);
        else if (kp_media_kind_of(path) != KP_MEDIA_NONE)
            push(out, path, NULL);
        g_free(path);
        g_free(it->key);
        g_free(it->name);
    }
    g_array_free(items, TRUE);
}

static void add_thread(GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
    (void)src;
    (void)cancel;
    add_job *job = data;
    for (char **u = job->uris; *u; u++) {
        if (!**u)
            continue;
        /* Local files as plain paths, subtunes and the rest as URIs. */
        char *loc = kp_media_location(*u);
        char *path = loc[0] == '/' ? loc : NULL;
        if (path && g_file_test(path, G_FILE_TEST_IS_DIR))
            walk(path, job->out, 0);
        else if (path && kp_media_is_playlist(path)) {
            kp_playlist *pl = kp_playlist_load(path, "");
            for (size_t i = 0; pl && i < kp_playlist_count(pl); i++) {
                const kp_entry *e = kp_playlist_get(pl, i);
                kp_tags t = {e->title, e->artist, e->album, e->length_ms};
                push(job->out, e->location, &t);
            }
            kp_playlist_free(pl);
        } else
            push(job->out, loc, NULL);
        g_free(loc);
    }
    g_task_return_boolean(task, TRUE);
}

static void add_job_free(add_job *job)
{
    for (guint i = 0; i < job->out->len; i++)
        found_clear(&g_array_index(job->out, found, i));
    g_array_free(job->out, TRUE);
    g_strfreev(job->uris);
    g_free(job);
}

static void add_done(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src;
    (void)res;
    add_job *job = data;
    kp_app *app = job->app;
    page_t *pg = page_for(app, job->id);
    if (!pg || !job->out->len) {
        add_job_free(job);
        return;
    }
    kp_playlist *pl = list_of(pg);
    long n = (long)kp_playlist_count(pl);
    long at = job->at < 0 || job->at > n ? n : job->at;
    /* Opened from outside: a file already there is played, not added. */
    GHashTable *have = NULL;
    if (job->dedupe) {
        have = g_hash_table_new(g_str_hash, g_str_equal);
        for (long i = 0; i < n; i++)
            g_hash_table_insert(have, (gpointer)kp_playlist_get(pl, (size_t)i)->location,
                                GINT_TO_POINTER((int)i + 1));
    }
    long first = -1, added = 0;
    for (guint i = 0; i < job->out->len; i++) {
        found *f = &g_array_index(job->out, found, i);
        long where = have ? GPOINTER_TO_INT(g_hash_table_lookup(have, f->location)) - 1 : -1;
        if (where < 0) {
            kp_tags t = {f->title, f->artist, f->album, f->length_ms};
            where = at + added;
            kp_playlist_insert(pl, (size_t)where, f->location, &t);
            added++;
        }
        if (first < 0)
            first = where;
    }
    if (have)
        g_hash_table_destroy(have);
    kp_plmodel_inserted(pg->model, at, added);
    /* Files that came without tags get them read. */
    for (long i = at; i < at + added; i++) {
        const kp_entry *e = kp_playlist_get(pl, (size_t)i);
        if (!e->title || e->length_ms < 0)
            read_tags(app, pg, i);
    }
    if (pg->id == app->playing.playlist_id)
        kp_player_playlist_changed(app, pg->id);
    schedule_save(app);
    kp_playlists_update_status(app);
    if (pg->view && first >= 0) {
        if (job->play || added)
            select_rows(pg, job->play ? first : at, job->play ? 1 : added);
        GtkTreePath *p = gtk_tree_path_new_from_indices((int)first, -1);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(pg->view), p, NULL, FALSE, 0, 0);
        gtk_tree_path_free(p);
    }
    if (job->play && first >= 0)
        kp_player_play_at(app, pg->id, first);
    add_job_free(job);
}

static void add_locations(kp_app *app, page_t *pg, long at, char **uris, bool dedupe, bool play)
{
    add_job *job = g_new0(add_job, 1);
    *job = (add_job){app, pg->id, at, dedupe, play, g_strdupv(uris),
                     g_array_new(FALSE, FALSE, sizeof(found))};
    GTask *task = g_task_new(NULL, NULL, add_done, job);
    g_task_set_task_data(task, job, NULL);
    g_task_run_in_thread(task, add_thread);
    g_object_unref(task);
}

static void new_page(kp_app *app, const char *name);

void kp_playlists_open(kp_app *app, char **locations, bool play, bool new_playlist)
{
    if (new_playlist) {
        /* One folder names the playlist after itself. */
        char *name = NULL;
        if (locations[0] && !locations[1] && g_path_is_absolute(locations[0]) &&
            kp_dir_exists(locations[0])) {
            name = g_path_get_basename(locations[0]);
            if (!name[0] || !strcmp(name, G_DIR_SEPARATOR_S)) {
                g_free(name);
                name = NULL;
            }
        }
        if (name)
            new_page(app, name);
        else
            kp_playlists_new(app);
        g_free(name);
    }
    page_t *pg = current_page(app);
    if (pg)
        add_locations(app, pg, -1, locations, true, play);
}

static void choose_and_add(kp_app *app, bool folder)
{
    page_t *pg = current_page(app);
    if (!pg)
        return;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        folder ? _("Add folder") : _("Add files"), GTK_WINDOW(app->window),
        folder ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER : GTK_FILE_CHOOSER_ACTION_OPEN,
        GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_ADD, GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dlg), TRUE);
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(dlg), FALSE);
    const char *last = kp_prefs_get_str(app->prefs, "playlists", "last_folder", NULL);
    if (last)
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), last);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        GSList *list = gtk_file_chooser_get_uris(GTK_FILE_CHOOSER(dlg));
        guint n = g_slist_length(list), i = 0;
        char **uris = g_new0(char *, n + 1);
        for (GSList *l = list; l; l = l->next)
            uris[i++] = l->data;
        add_locations(app, pg, -1, uris, false, false);
        g_strfreev(uris);
        g_slist_free(list);
        char *dir = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(dlg));
        if (dir)
            kp_prefs_set_str(app->prefs, "playlists", "last_folder", dir);
        g_free(dir);
    }
    gtk_widget_destroy(dlg);
}

void kp_playlists_add_files(kp_app *app)
{
    choose_and_add(app, false);
}

void kp_playlists_add_folder(kp_app *app)
{
    choose_and_add(app, true);
}

/* ---- removing, queueing, moving ------------------------------------------ */

void kp_playlists_remove_selected(kp_app *app)
{
    page_t *pg = current_page(app);
    size_t n;
    size_t *rows = pg ? selected_rows(pg, &n) : NULL;
    if (!rows || !n) {
        g_free(rows);
        return;
    }
    kp_playlist *pl = list_of(pg);
    /* The playing entry keeps its place; if it goes, "next" continues
     * from there: the index drops by the rows removed before it. */
    bool here = pg->id == app->playing.playlist_id && app->playing.index >= 0;
    long playing = app->playing.index, before = 0;
    bool gone = false;
    for (size_t i = 0; here && i < n; i++) {
        if ((long)rows[i] < playing)
            before++;
        gone |= (long)rows[i] == playing;
    }
    kp_playlist_remove_rows(pl, rows, n);
    for (size_t i = n; i-- > 0;)
        kp_plmodel_deleted(pg->model, (long)rows[i]);
    g_free(rows);
    if (here) {
        app->playing.index = playing - before - (gone ? 1 : 0);
        if (gone)
            app->playing.location = NULL;
        kp_playlists_mark_playing(app, pg->id, gone ? -1 : app->playing.index);
    }
    kp_player_playlist_changed(app, pg->id);
    schedule_save(app);
    kp_playlists_update_status(app);
}

void kp_playlists_toggle_queue(kp_app *app)
{
    page_t *pg = current_page(app);
    size_t n;
    size_t *rows = pg ? selected_rows(pg, &n) : NULL;
    for (size_t i = 0; rows && i < n; i++)
        kp_player_queue_toggle(app, pg->id, (long)rows[i]);
    g_free(rows);
    kp_playlists_queue_changed(app);
}

/* ---- clipboard, context menu --------------------------------------------- */

enum { CLIP_TEXT, CLIP_URIS };

static const GtkTargetEntry clip_targets[] = {
    {(char *)"text/uri-list", 0, CLIP_URIS},
    {(char *)"UTF8_STRING", 0, CLIP_TEXT},
    {(char *)"text/plain", 0, CLIP_TEXT},
};

/* What the clipboard holds is the locations (a strv) of the copied rows:
 * as text, one per line, and as a text/uri-list, which file managers
 * understand too. */
static void clip_get(GtkClipboard *cb, GtkSelectionData *sel, guint info, gpointer owner)
{
    (void)cb;
    char **locs = owner;
    GString *out = g_string_new("");
    for (char **l = locs; *l; l++) {
        if (info == CLIP_URIS && (*l)[0] == '/') {
            char *uri = g_filename_to_uri(*l, NULL, NULL);
            g_string_append(out, uri ? uri : *l);
            g_free(uri);
        } else {
            g_string_append(out, *l);
        }
        g_string_append(out, info == CLIP_URIS ? "\r\n" : "\n");
    }
    gtk_selection_data_set(sel, gtk_selection_data_get_target(sel), 8, (const guchar *)out->str,
                           (gint)out->len);
    g_string_free(out, TRUE);
}

static void clip_clear(GtkClipboard *cb, gpointer owner)
{
    (void)cb;
    g_strfreev(owner);
}

/* The selected rows' locations onto the clipboard; false if none. */
static bool copy_selected(kp_app *app, page_t *pg)
{
    size_t n;
    size_t *rows = selected_rows(pg, &n);
    if (!n) {
        g_free(rows);
        return false;
    }
    kp_playlist *pl = list_of(pg);
    char **locs = g_new0(char *, n + 1);
    for (size_t i = 0; i < n; i++)
        locs[i] = g_strdup(kp_playlist_get(pl, rows[i])->location);
    g_free(rows);
    GtkClipboard *cb = gtk_widget_get_clipboard(app->window, GDK_SELECTION_CLIPBOARD);
    if (!gtk_clipboard_set_with_data(cb, clip_targets, G_N_ELEMENTS(clip_targets), clip_get,
                                     clip_clear, locs)) {
        g_strfreev(locs);
        return false;
    }
    return true;
}

void kp_playlists_copy(kp_app *app)
{
    page_t *pg = current_page(app);
    if (pg)
        copy_selected(app, pg);
}

void kp_playlists_cut(kp_app *app)
{
    page_t *pg = current_page(app);
    if (pg && copy_selected(app, pg))
        kp_playlists_remove_selected(app);
}

typedef struct {
    kp_app *app;
    int id;
    long at;
} paste_ctx;

/* Lines that look like a location: a path or something://. */
static char **location_lines(const char *text)
{
    char **lines = g_strsplit_set(text, "\r\n", -1);
    GPtrArray *out = g_ptr_array_new();
    for (char **l = lines; *l; l++) {
        char *s = g_strstrip(*l);
        if (s[0] == '/' || strstr(s, "://"))
            g_ptr_array_add(out, g_strdup(s));
    }
    g_ptr_array_add(out, NULL);
    g_strfreev(lines);
    return (char **)g_ptr_array_free(out, FALSE);
}

static void paste_add(paste_ctx *ctx, char **locs)
{
    page_t *pg = page_for(ctx->app, ctx->id);
    if (pg && locs && *locs)
        add_locations(ctx->app, pg, ctx->at, locs, false, false);
    g_strfreev(locs);
    g_free(ctx);
}

static void paste_text(GtkClipboard *cb, const gchar *text, gpointer data)
{
    (void)cb;
    paste_add(data, text ? location_lines(text) : NULL);
}

static void paste_uris(GtkClipboard *cb, gchar **uris, gpointer data)
{
    if (uris && *uris)
        paste_add(data, g_strdupv(uris));
    else
        gtk_clipboard_request_text(cb, paste_text, data);
}

/* After the last selected row, else at the end. */
void kp_playlists_paste(kp_app *app)
{
    page_t *pg = current_page(app);
    if (!pg)
        return;
    size_t n;
    size_t *rows = selected_rows(pg, &n);
    paste_ctx *ctx = g_new(paste_ctx, 1);
    *ctx = (paste_ctx){app, pg->id, n ? (long)rows[n - 1] + 1 : -1};
    g_free(rows);
    gtk_clipboard_request_uris(gtk_widget_get_clipboard(app->window, GDK_SELECTION_CLIPBOARD),
                               paste_uris, ctx);
}

void kp_playlists_play_selected(kp_app *app)
{
    long index;
    int id = kp_playlists_selection(app, &index);
    if (id >= 0 && index >= 0)
        kp_player_play_at(app, id, index);
}

/* The folder of the first selected file, in the file manager. */
void kp_playlists_open_folder(kp_app *app)
{
    long index;
    int id = kp_playlists_selection(app, &index);
    kp_playlist *pl = id >= 0 && index >= 0 ? kp_playlists_get(app, id) : NULL;
    if (!pl)
        return;
    char *path = kp_media_location(kp_playlist_get(pl, (size_t)index)->location);
    char *file = g_str_has_prefix(path, "file://") ? g_filename_from_uri(path, NULL, NULL)
                                                   : g_strdup(path);
    char *q = file ? strrchr(file, '?') : NULL;
    if (q && !g_file_test(file, G_FILE_TEST_EXISTS))
        *q = '\0';
    if (file && file[0] == '/') {
        char *dir = g_path_get_dirname(file);
        char *uri = g_filename_to_uri(dir, NULL, NULL);
        GError *err = NULL;
        if (uri && !g_app_info_launch_default_for_uri(uri, NULL, &err)) {
            kp_log_warn(_("could not open the folder: %s"), err ? err->message : "?");
            g_clear_error(&err);
        }
        g_free(uri);
        g_free(dir);
    }
    g_free(file);
    g_free(path);
}

static GtkWidget *row_menu_item(GtkWidget *menu, const char *label, void (*cb)(kp_app *),
                                kp_app *app, const char *action, bool enabled)
{
    GtkWidget *it = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect_swapped(it, "activate", G_CALLBACK(cb), app);
    if (action)
        kp_shortcuts_attach(it, action);
    gtk_widget_set_sensitive(it, enabled);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    return it;
}

static void row_menu_separator(GtkWidget *menu)
{
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

/* Right click on rows: acts on the selection (which first becomes the row
 * clicked, unless that one is already selected). */
static void row_menu(kp_app *app, page_t *pg, GdkEventButton *ev)
{
    size_t n;
    size_t *rows = selected_rows(pg, &n);
    bool local = false;
    if (n) {
        const char *loc = kp_playlist_get(list_of(pg), rows[0])->location;
        local = loc[0] == '/' || g_str_has_prefix(loc, "file://");
    }
    g_free(rows);
    GtkWidget *m = gtk_menu_new();
    row_menu_item(m, _("_Play"), kp_playlists_play_selected, app, NULL, n > 0);
    row_menu_item(m, _("_Queue / unqueue"), kp_playlists_toggle_queue, app, "queue", n > 0);
    row_menu_separator(m);
    row_menu_item(m, _("Cu_t"), kp_playlists_cut, app, "cut", n > 0);
    row_menu_item(m, _("_Copy"), kp_playlists_copy, app, "copy", n > 0);
    row_menu_item(m, _("_Paste"), kp_playlists_paste, app, "paste", true);
    row_menu_item(m, _("Remove _selected"), kp_playlists_remove_selected, app, "remove", n > 0);
    row_menu_separator(m);
    row_menu_item(m, _("Open containing _folder"), kp_playlists_open_folder, app, NULL,
                  n > 0 && local);
    gtk_widget_show_all(m);
    g_signal_connect(m, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup(GTK_MENU(m), NULL, NULL, NULL, NULL, ev->button, ev->time);
}

/* Moves the selection to before row "to" (count: the end). */
static void move_selection(kp_app *app, page_t *pg, size_t to)
{
    size_t n;
    size_t *rows = selected_rows(pg, &n);
    kp_playlist *pl = list_of(pg);
    if (n) {
        int *order = g_new(int, kp_playlist_count(pl) + 1);
        size_t first = kp_playlist_move(pl, rows, n, to, order);
        kp_plmodel_reordered(pg->model, order);
        g_free(order);
        select_rows(pg, (long)first, (long)n);
        kp_player_playlist_changed(app, pg->id);
        schedule_save(app);
    }
    g_free(rows);
}

/* ---- drag and drop ------------------------------------------------------- */

enum { TARGET_ROWS, TARGET_URIS };

static const GtkTargetEntry row_targets[] = {
    {(char *)"KIPLAY_ROWS", GTK_TARGET_SAME_WIDGET, TARGET_ROWS},
};
static const GtkTargetEntry drop_targets[] = {
    {(char *)"KIPLAY_ROWS", GTK_TARGET_SAME_WIDGET, TARGET_ROWS},
    {(char *)"text/uri-list", 0, TARGET_URIS},
};

static gboolean select_allowed(GtkTreeSelection *sel, GtkTreeModel *m, GtkTreePath *p,
                               gboolean selected, gpointer data)
{
    (void)sel;
    (void)m;
    (void)p;
    (void)selected;
    page_t *pg = data;
    return !pg->hold_selection;
}

static gboolean on_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    page_t *pg = data;
    if (ev->type == GDK_BUTTON_PRESS && ev->button == 3) {
        kp_app *app = g_object_get_data(G_OBJECT(w), "kp-app");
        GtkTreePath *path;
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(w));
        if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), (int)ev->x, (int)ev->y, &path,
                                          NULL, NULL, NULL)) {
            if (!gtk_tree_selection_path_is_selected(sel, path))
                gtk_tree_view_set_cursor(GTK_TREE_VIEW(w), path, NULL, FALSE);
            gtk_tree_path_free(path);
        }
        gtk_widget_grab_focus(w);
        row_menu(app, pg, ev);
        return TRUE;
    }
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 1 ||
        (ev->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)))
        return FALSE;
    GtkTreePath *path;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), (int)ev->x, (int)ev->y, &path, NULL,
                                       NULL, NULL))
        return FALSE;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(w));
    pg->hold_selection = gtk_tree_selection_path_is_selected(sel, path) &&
                         gtk_tree_selection_count_selected_rows(sel) > 1;
    gtk_tree_path_free(path);
    return FALSE;
}

static gboolean on_release(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    page_t *pg = data;
    if (!pg->hold_selection)
        return FALSE;
    pg->hold_selection = false;
    /* A click without a drag: now it selects just that row. */
    GtkTreePath *path;
    if (!pg->dragging && gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), (int)ev->x,
                                                       (int)ev->y, &path, NULL, NULL, NULL)) {
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(w), path, NULL, FALSE);
        gtk_tree_path_free(path);
    }
    return FALSE;
}

static void on_drag_begin(GtkWidget *w, GdkDragContext *ctx, gpointer data)
{
    (void)w;
    (void)ctx;
    page_t *pg = data;
    pg->dragging = true;
    kp_log_debug("drag: begin");
}

static void on_drag_end(GtkWidget *w, GdkDragContext *ctx, gpointer data)
{
    (void)w;
    (void)ctx;
    page_t *pg = data;
    pg->dragging = pg->hold_selection = false;
}

/* The drop itself: asks for the data, which arrives in on_drop. (The tree
 * view's own handler wants a model that is a GtkTreeDragDest.) */
static gboolean on_drag_drop(GtkWidget *w, GdkDragContext *ctx, gint x, gint y, guint time,
                             gpointer data)
{
    (void)x;
    (void)y;
    (void)data;
    g_signal_stop_emission_by_name(w, "drag-drop");
    gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(w), NULL, GTK_TREE_VIEW_DROP_BEFORE);
    GdkAtom target = gtk_drag_dest_find_target(w, ctx, NULL);
    if (target == GDK_NONE)
        return FALSE;
    gtk_drag_get_data(w, ctx, target, time);
    return TRUE;
}

/* Where a drop at (x, y) goes: before that row, or after it. */
static size_t drop_index(page_t *pg, int x, int y)
{
    GtkTreePath *path;
    GtkTreeViewDropPosition pos;
    size_t n = kp_playlist_count(list_of(pg));
    if (!gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(pg->view), x, y, &path, &pos))
        return n;
    size_t i = (size_t)gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    if (pos == GTK_TREE_VIEW_DROP_AFTER || pos == GTK_TREE_VIEW_DROP_INTO_OR_AFTER)
        i++;
    return i > n ? n : i;
}

static void on_drop(GtkWidget *w, GdkDragContext *ctx, gint x, gint y, GtkSelectionData *sel,
                    guint info, guint time, gpointer data)
{
    page_t *pg = data;
    kp_app *app = g_object_get_data(G_OBJECT(w), "kp-app");
    /* The tree view's own handler wants a model that knows DnD. */
    g_signal_stop_emission_by_name(w, "drag-data-received");
    kp_log_debug("drag: dropped (%s) at %d,%d", info == TARGET_ROWS ? "rows" : "files", x, y);
    bool ok = false;
    if (info == TARGET_ROWS) {
        move_selection(app, pg, drop_index(pg, x, y));
        ok = true;
    } else {
        char **uris = gtk_selection_data_get_uris(sel);
        if (uris) {
            add_locations(app, pg, (long)drop_index(pg, x, y), uris, false, false);
            ok = true;
        }
        g_strfreev(uris);
    }
    gtk_drag_finish(ctx, ok, FALSE, time);
}

/* ---- the view ------------------------------------------------------------ */

static void on_row_activated(GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col,
                             gpointer data)
{
    (void)view;
    (void)col;
    page_t *pg = data;
    kp_app *app = g_object_get_data(G_OBJECT(view), "kp-app");
    kp_player_play_at(app, pg->id, gtk_tree_path_get_indices(path)[0]);
}

void kp_playlists_move_selected(kp_app *app, int delta)
{
    page_t *pg = current_page(app);
    size_t n;
    size_t *rows = pg ? selected_rows(pg, &n) : NULL;
    if (!rows)
        return;
    size_t count = kp_playlist_count(list_of(pg));
    if (n && delta < 0 && rows[0] > 0)
        move_selection(app, pg, rows[0] - 1);
    else if (n && delta > 0 && rows[n - 1] + 1 < count)
        move_selection(app, pg, rows[n - 1] + 2);
    g_free(rows);
}

static void show_columns(kp_app *app, page_t *pg)
{
    kp_plmodel_set_split_artist(pg->model, app->columns & KP_COLUMN_ARTIST);
    if (!pg->view)
        return;
    for (size_t c = 0; c < G_N_ELEMENTS(columns); c++)
        gtk_tree_view_column_set_visible(
            gtk_tree_view_get_column(GTK_TREE_VIEW(pg->view), (int)c),
            !columns[c].bit || (app->columns & columns[c].bit));
    gtk_widget_queue_draw(pg->view);
}

static void ensure_view(kp_app *app, page_t *pg)
{
    if (pg->view)
        return;
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(pg->model));
    GtkTreeView *tv = GTK_TREE_VIEW(view);
    pg->view = view;
    g_object_set_data(G_OBJECT(view), "kp-app", app);
    gtk_tree_view_set_rules_hint(tv, TRUE);
    /* Ctrl+J finds files; the built-in search would read every row. */
    gtk_tree_view_set_enable_search(tv, FALSE);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(tv);
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_MULTIPLE);
    gtk_tree_selection_set_select_function(sel, select_allowed, pg, NULL);

    for (size_t c = 0; c < G_N_ELEMENTS(columns); c++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        g_object_set(r, "xalign", columns[c].xalign, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            columns[c].col == KP_COL_NUM ? columns[c].header : _(columns[c].header), r, "text",
            columns[c].col, "weight", KP_COL_WEIGHT, NULL);
        /* Fixed sizes: the view measures one row, not every row. */
        gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
        char key[32];
        g_snprintf(key, sizeof key, "width_%s", columns[c].key);
        gtk_tree_view_column_set_fixed_width(
            col, kp_prefs_get_int(app->prefs, "playlists", key, columns[c].width, 16, 4000));
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_column_set_expand(col, columns[c].col == KP_COL_TITLE);
        gtk_tree_view_append_column(tv, col);
    }
    gtk_tree_view_set_fixed_height_mode(tv, TRUE);

    gtk_tree_view_enable_model_drag_source(tv, GDK_BUTTON1_MASK, row_targets,
                                           G_N_ELEMENTS(row_targets), GDK_ACTION_MOVE);
    gtk_tree_view_enable_model_drag_dest(tv, drop_targets, G_N_ELEMENTS(drop_targets),
                                         GDK_ACTION_MOVE | GDK_ACTION_COPY);
    g_signal_connect(view, "drag-drop", G_CALLBACK(on_drag_drop), pg);
    g_signal_connect(view, "drag-data-received", G_CALLBACK(on_drop), pg);
    g_signal_connect(view, "drag-begin", G_CALLBACK(on_drag_begin), pg);
    g_signal_connect(view, "drag-end", G_CALLBACK(on_drag_end), pg);
    g_signal_connect(view, "button-press-event", G_CALLBACK(on_press), pg);
    g_signal_connect(view, "button-release-event", G_CALLBACK(on_release), pg);
    g_signal_connect(view, "row-activated", G_CALLBACK(on_row_activated), pg);

    show_columns(app, pg);
    gtk_container_add(GTK_CONTAINER(pg->scroll), view);
    gtk_widget_show(view);
    /* The playing tab opens on the playing row. */
    long playing = kp_plmodel_playing(pg->model);
    if (playing >= 0) {
        GtkTreePath *p = gtk_tree_path_new_from_indices((int)playing, -1);
        gtk_tree_view_scroll_to_cell(tv, p, NULL, TRUE, 0.5f, 0.0f);
        gtk_tree_path_free(p);
    }
}

void kp_playlists_set_columns(kp_app *app, unsigned cols)
{
    save_widths(app, current_page(app));
    app->columns = cols;
    GString *names = g_string_new("");
    for (int i = 0; i < KP_COLUMN_COUNT; i++)
        if (cols & (1u << i))
            g_string_append_printf(names, "%s%s", names->len ? "," : "", kp_column_names[i]);
    kp_prefs_set_str(app->prefs, "playlists", "columns", names->str);
    g_string_free(names, TRUE);
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(app->notebook));
    for (int i = 0; i < n; i++)
        show_columns(app, page_at(app, i));
}

/* ---- tabs ---------------------------------------------------------------- */

static void tab_menu_item(GtkWidget *menu, const char *label, void (*cb)(kp_app *), kp_app *app)
{
    GtkWidget *it = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect_swapped(it, "activate", G_CALLBACK(cb), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    gtk_widget_show(it);
}

/* Right click on a tab: the playlist of that tab, not the one on screen. */
static void tab_menu(kp_app *app, GdkEventButton *ev)
{
    GtkWidget *m = gtk_menu_new();
    tab_menu_item(m, _("_Rename..."), kp_playlists_rename, app);
    tab_menu_item(m, _("_Export playlist..."), kp_playlists_export, app);
    GtkWidget *sep = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(m), sep);
    gtk_widget_show(sep);
    tab_menu_item(m, _("Re_move"), kp_playlists_close, app);
    g_signal_connect(m, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup(GTK_MENU(m), NULL, NULL, NULL, NULL, ev->button, ev->time);
}

static gboolean on_tab_click(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    kp_app *app = data;
    if (ev->type != GDK_2BUTTON_PRESS && ev->type != GDK_BUTTON_PRESS)
        return FALSE;
    if ((ev->type == GDK_2BUTTON_PRESS && ev->button != 1) ||
        (ev->type == GDK_BUTTON_PRESS && ev->button != 3))
        return FALSE;
    page_t *pg = g_object_get_data(G_OBJECT(w), "kp-page");
    gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook),
                                  gtk_notebook_page_num(GTK_NOTEBOOK(app->notebook), pg->scroll));
    if (ev->button == 3)
        tab_menu(app, ev);
    else
        kp_playlists_rename(app);
    return TRUE;
}

static GtkWidget *tab_label(kp_app *app, page_t *pg, const char *name)
{
    /* An event box so a double click on the tab renames it; the icon marks
     * the tab that plays. */
    GtkWidget *box = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(box), FALSE);
    GtkWidget *hbox = gtk_hbox_new(FALSE, 3);
    pg->tab_icon = gtk_image_new_from_stock(GTK_STOCK_MEDIA_PLAY, GTK_ICON_SIZE_MENU);
    pg->tab_label = gtk_label_new(name);
    gtk_box_pack_start(GTK_BOX(hbox), pg->tab_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), pg->tab_label, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(box), hbox);
    g_object_set_data(G_OBJECT(box), "kp-page", pg);
    g_signal_connect(box, "button-press-event", G_CALLBACK(on_tab_click), app);
    gtk_widget_show(hbox);
    gtk_widget_show(pg->tab_label);
    gtk_widget_show(box);
    return box;
}

static void page_free(gpointer data)
{
    page_t *pg = data;
    g_object_unref(pg->model);
    g_free(pg);
}

static page_t *add_page(kp_app *app, size_t i)
{
    kp_playlist *pl = kp_plset_get(app->playlists, i);
    page_t *pg = g_new0(page_t, 1);
    pg->id = kp_plset_id(app->playlists, i);
    pg->model = kp_plmodel_new(pl, pg->id);
    kp_plmodel_set_hooks(pg->model, on_queue_pos, on_need_tags, app);
    kp_plmodel_set_split_artist(pg->model, app->columns & KP_COLUMN_ARTIST);
    pg->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(pg->scroll), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(pg->scroll), GTK_SHADOW_IN);
    g_object_set_data_full(G_OBJECT(pg->scroll), "kp-page", pg, page_free);
    gtk_widget_show(pg->scroll);
    gtk_notebook_insert_page(GTK_NOTEBOOK(app->notebook), pg->scroll,
                             tab_label(app, pg, kp_playlist_name(pl)), (int)i);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(app->notebook), pg->scroll, TRUE);
    return pg;
}

/* The "+" tab: always last, never a playlist. Switching to it makes one. */
static GtkWidget *ghost;

static void on_reordered(GtkNotebook *nb, GtkWidget *child, guint to, gpointer data)
{
    kp_app *app = data;
    if (child == ghost)
        return;
    /* A tab dropped after the "+": the "+" goes back to the end. */
    int last = gtk_notebook_get_n_pages(nb) - 1;
    if (ghost && gtk_notebook_page_num(nb, ghost) != last) {
        gtk_notebook_reorder_child(nb, ghost, -1);
        if (to > (guint)last - 1)
            to = (guint)last - 1;
    }
    long from = kp_plset_find(app->playlists, page_of(child)->id);
    if (from >= 0)
        kp_plset_move(app->playlists, (size_t)from, to);
    schedule_save(app);
}

static gboolean status_idle(gpointer data)
{
    kp_playlists_update_status(data);
    return G_SOURCE_REMOVE;
}

static void new_page(kp_app *app, const char *name);

/* The "+" was selected (switch-page can't change pages itself): add the
 * playlist at the end, unless something else already took over (closing
 * the last playlist lands here too). */
static gboolean ghost_idle(gpointer data)
{
    kp_app *app = data;
    GtkNotebook *nb = GTK_NOTEBOOK(app->notebook);
    if (!ghost || gtk_notebook_get_nth_page(nb, gtk_notebook_get_current_page(nb)) != ghost)
        return G_SOURCE_REMOVE;
    size_t n = kp_plset_count(app->playlists);
    if (n > 0)
        gtk_notebook_set_current_page(nb, (int)n - 1);
    char *name = g_strdup_printf(_("Playlist %zu"), n + 1);
    new_page(app, name);
    g_free(name);
    return G_SOURCE_REMOVE;
}

static void on_switch(GtkNotebook *nb, gpointer page, guint num, gpointer data)
{
    (void)page;
    kp_app *app = data;
    /* switch-page fires before the current page changes; the GtkNotebookPage
     * it gets isn't the child in GTK 2. */
    page_t *pg = page_of(gtk_notebook_get_nth_page(nb, (int)num));
    if (pg)
        ensure_view(app, pg);
    else if (ghost && gtk_notebook_get_nth_page(nb, (int)num) == ghost)
        g_idle_add(ghost_idle, app);
    g_idle_add(status_idle, app);
    schedule_save(app);
}

/* Where a new tab goes: after the current one (never after the "+"). */
static size_t new_position(kp_app *app)
{
    int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
    size_t at = (size_t)(cur < 0 ? 0 : cur + 1);
    size_t n = kp_plset_count(app->playlists);
    return at > n ? n : at;
}

/* A new tab after the current one, selected. */
static void new_page(kp_app *app, const char *name)
{
    size_t at = kp_plset_insert(app->playlists, new_position(app), name);
    add_page(app, at);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), (int)at);
    schedule_save(app);
}

void kp_playlists_new(kp_app *app)
{
    char *name = g_strdup_printf(_("Playlist %zu"), kp_plset_count(app->playlists) + 1);
    new_page(app, name);
    g_free(name);
}

void kp_playlists_rename(kp_app *app)
{
    page_t *pg = current_page(app);
    if (!pg)
        return;
    kp_playlist *pl = list_of(pg);
    GtkWidget *dlg = gtk_dialog_new_with_buttons(_("Rename playlist"), GTK_WINDOW(app->window),
                                                 GTK_DIALOG_MODAL | GTK_DIALOG_NO_SEPARATOR,
                                                 GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                                 GTK_STOCK_OK, GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), kp_playlist_name(pl));
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 30);
    gtk_container_set_border_width(GTK_CONTAINER(entry), 6);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dlg)->vbox), entry, TRUE, TRUE, 6);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        char *name = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))));
        if (*name) {
            kp_playlist_set_name(pl, name);
            gtk_label_set_text(GTK_LABEL(pg->tab_label), name);
            schedule_save(app);
        }
        g_free(name);
    }
    gtk_widget_destroy(dlg);
}

void kp_playlists_close(kp_app *app)
{
    page_t *pg = current_page(app);
    if (!pg)
        return;
    kp_playlist *pl = list_of(pg);
    if (kp_playlist_count(pl) > 0) {
        GtkWidget *q = gtk_message_dialog_new(
            GTK_WINDOW(app->window), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
            _("Close the playlist \"%s\"?"), kp_playlist_name(pl));
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(q),
            ngettext("Its %zu track will be removed from the list (the file stays).",
                     "Its %zu tracks will be removed from the list (the files stay).",
                     kp_playlist_count(pl)),
            kp_playlist_count(pl));
        gtk_dialog_add_buttons(GTK_DIALOG(q), GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                               GTK_STOCK_CLOSE, GTK_RESPONSE_ACCEPT, NULL);
        int r = gtk_dialog_run(GTK_DIALOG(q));
        gtk_widget_destroy(q);
        if (r != GTK_RESPONSE_ACCEPT)
            return;
    }
    int id = pg->id;
    kp_player_playlist_closed(app, id);
    if (app->tags)
        kp_tagreader_forget(app->tags, id);
    long i = kp_plset_find(app->playlists, id);
    /* GTK would pick the "+" after the last tab: pick the one before. */
    GtkNotebook *nb = GTK_NOTEBOOK(app->notebook);
    int cur = gtk_notebook_get_current_page(nb);
    if (cur > 0 && cur + 2 == gtk_notebook_get_n_pages(nb))
        gtk_notebook_set_current_page(nb, cur - 1);
    /* The page (and its model) goes before the playlist it shows. */
    gtk_widget_destroy(pg->scroll);
    if (i >= 0)
        kp_plset_remove(app->playlists, (size_t)i);
    /* There is always at least one playlist. */
    if (kp_plset_count(app->playlists) == 0)
        kp_playlists_new(app);
    schedule_save(app);
}

/* ---- import and export --------------------------------------------------- */

static GtkFileFilter *playlist_filter(void)
{
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, _("Playlists (m3u, m3u8, pls)"));
    const char *const pats[] = {"*.m3u", "*.M3U", "*.m3u8", "*.M3U8", "*.pls", "*.PLS"};
    for (size_t i = 0; i < G_N_ELEMENTS(pats); i++)
        gtk_file_filter_add_pattern(f, pats[i]);
    return f;
}

void kp_playlists_import(kp_app *app)
{
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        _("Import playlist"), GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_OPEN,
        GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), playlist_filter());
    const char *last = kp_prefs_get_str(app->prefs, "playlists", "last_folder", NULL);
    if (last)
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), last);
    char *path = NULL;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
        path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
    gtk_widget_destroy(dlg);
    if (!path)
        return;
    char *base = g_path_get_basename(path);
    char *dot = strrchr(base, '.');
    if (dot && dot != base)
        *dot = '\0';
    char *name = g_filename_display_name(base);
    kp_playlist *in = kp_playlist_load(path, name);
    if (!in) {
        GtkWidget *e = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                                              GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                              _("Cannot read %s"), name);
        gtk_dialog_run(GTK_DIALOG(e));
        gtk_widget_destroy(e);
    } else {
        size_t at = kp_plset_insert(app->playlists, new_position(app),
                                    *kp_playlist_name(in) ? kp_playlist_name(in) : name);
        kp_playlist *pl = kp_plset_get(app->playlists, at);
        for (size_t i = 0; i < kp_playlist_count(in); i++) {
            const kp_entry *e = kp_playlist_get(in, i);
            kp_tags t = {e->title, e->artist, e->album, e->length_ms};
            kp_playlist_add(pl, e->location, &t);
        }
        kp_playlist_free(in);
        add_page(app, at);
        gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), (int)at);
        schedule_save(app);
    }
    g_free(name);
    g_free(base);
    g_free(path);
}

void kp_playlists_export(kp_app *app)
{
    page_t *pg = current_page(app);
    if (!pg)
        return;
    kp_playlist *pl = list_of(pg);
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        _("Export playlist"), GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_SAVE,
        GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), playlist_filter());
    char *suggested = g_strdup_printf("%s.m3u8", kp_playlist_name(pl));
    g_strdelimit(suggested, "/", '-');
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), suggested);
    g_free(suggested);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (kp_playlist_save(pl, path) < 0) {
            GtkWidget *e = gtk_message_dialog_new(GTK_WINDOW(dlg), GTK_DIALOG_MODAL,
                                                  GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                                  _("Cannot write %s"), path);
            gtk_dialog_run(GTK_DIALOG(e));
            gtk_widget_destroy(e);
        }
        g_free(path);
    }
    gtk_widget_destroy(dlg);
}

/* ---- set up -------------------------------------------------------------- */

static unsigned columns_from_prefs(kp_app *app)
{
    const char *s = kp_prefs_get_str(app->prefs, "playlists", "columns", NULL);
    if (!s)
        return KP_COLUMNS_DEFAULT;
    unsigned cols = 0;
    char **names = g_strsplit(s, ",", -1);
    for (char **n = names; *n; n++)
        for (int i = 0; i < KP_COLUMN_COUNT; i++)
            if (!strcmp(g_strstrip(*n), kp_column_names[i]))
                cols |= 1u << i;
    g_strfreev(names);
    return cols;
}

/* kiplay-audio next to the kiplay binary (a build tree), else in PATH. */
static char *engine_path(void)
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

void kp_playlists_init(kp_app *app)
{
    app->columns = columns_from_prefs(app);
    app->queue = g_array_new(FALSE, FALSE, sizeof(kp_entry_ref));
    char *engine = engine_path();
    app->tags = kp_tagreader_new(engine, on_tags, app);
    g_free(engine);
    app->notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(app->notebook), TRUE);
    gtk_widget_show(app->notebook);
    /* layout.c moves this box around; the search panel joins the notebook in it. */
    app->side = gtk_hbox_new(FALSE, 0);
    g_object_ref_sink(app->side);
    gtk_box_pack_start(GTK_BOX(app->side), app->notebook, TRUE, TRUE, 0);
    if (kp_plset_count(app->playlists) == 0) {
        kp_plset_insert(app->playlists, 0, _("Playlist 1"));
        schedule_save(app);
    }
    for (size_t i = 0; i < kp_plset_count(app->playlists); i++)
        add_page(app, i);
    ghost = gtk_vbox_new(FALSE, 0);
    GtkWidget *plus = gtk_label_new("+");
    gtk_widget_set_tooltip_text(plus, _("New playlist"));
    gtk_widget_show(plus);
    gtk_widget_show(ghost);
    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), ghost, plus);
    long active = kp_plset_find(app->playlists,
                                kp_prefs_get_int(app->prefs, "playlists", "active", 0, 0, 1000000));
    g_signal_connect(app->notebook, "switch-page", G_CALLBACK(on_switch), app);
    g_signal_connect(app->notebook, "page-reordered", G_CALLBACK(on_reordered), app);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), active >= 0 ? (int)active : 0);
    page_t *pg = current_page(app);
    if (pg)
        ensure_view(app, pg);
}

void kp_playlists_free(kp_app *app)
{
    if (status_timer) {
        g_source_remove(status_timer);
        status_timer = 0;
    }
    /* The queue stays: widgets being destroyed may still draw. */
    kp_tagreader_free(app->tags);
    app->tags = NULL;
}
