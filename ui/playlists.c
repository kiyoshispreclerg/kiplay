/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <gdk/gdkkeysyms.h>
#include <string.h>

#include "app.h"
#include "i18n.h"
#include "log.h"
#include "util.h"

/* Each notebook page is a scrolled window holding one tree view; the page
 * carries the playlist id ("kp-id"), which survives reordering. */

enum { COL_NUM, COL_TITLE, COL_LENGTH, COL_WEIGHT, N_COLS };

static guint save_timer;

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
    int page = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
    if (page >= 0)
        kp_prefs_set_int(app->prefs, "playlists", "active",
                         kp_plset_id(app->playlists, (size_t)page));
}

static int page_id(GtkWidget *page)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(page), "kp-id"));
}

static GtkWidget *current_page(kp_app *app, kp_playlist **pl)
{
    int n = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
    if (n < 0)
        return NULL;
    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app->notebook), n);
    if (pl) {
        long i = kp_plset_find(app->playlists, page_id(page));
        *pl = i >= 0 ? kp_plset_get(app->playlists, (size_t)i) : NULL;
    }
    return page;
}

static GtkTreeView *view_of(GtkWidget *page)
{
    return GTK_TREE_VIEW(gtk_bin_get_child(GTK_BIN(page)));
}

/* What the title column shows until tags are read (phase 3): the file
 * name without the folder. */
static char *display_title(const kp_entry *e)
{
    if (e->title)
        return g_strdup(e->title);
    char *path = g_str_has_prefix(e->location, "file://")
                     ? g_filename_from_uri(e->location, NULL, NULL) : NULL;
    const char *src = path ? path : e->location;
    char *base = strchr(src, '/') ? g_path_get_basename(src) : g_strdup(src);
    char *shown = g_filename_display_name(base);
    g_free(base);
    g_free(path);
    return shown;
}

static void append_row(GtkListStore *store, size_t i, const kp_entry *e)
{
    char len[32] = "";
    if (e->length_ms >= 0)
        kp_format_duration(len, sizeof len, e->length_ms);
    char *title = display_title(e);
    GtkTreeIter it;
    gtk_list_store_insert_with_values(store, &it, -1, COL_NUM, (int)(i + 1), COL_TITLE, title,
                                      COL_LENGTH, len, COL_WEIGHT, PANGO_WEIGHT_NORMAL, -1);
    g_free(title);
}

/* Rewrites row i from the entry, keeping its weight. */
static void set_row(GtkListStore *store, GtkTreeIter *it, const kp_entry *e)
{
    char len[32] = "";
    if (e->length_ms >= 0)
        kp_format_duration(len, sizeof len, e->length_ms);
    char *title = display_title(e);
    gtk_list_store_set(store, it, COL_TITLE, title, COL_LENGTH, len, -1);
    g_free(title);
}

static void refill(GtkListStore *store, const kp_playlist *pl)
{
    gtk_list_store_clear(store);
    for (size_t i = 0; i < kp_playlist_count(pl); i++)
        append_row(store, i, kp_playlist_get(pl, i));
}

/* The page of the playlist with that id, or NULL. */
static GtkWidget *page_for(kp_app *app, int id)
{
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(app->notebook));
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app->notebook), i);
        if (page_id(page) == id)
            return page;
    }
    return NULL;
}

static bool row_iter(kp_app *app, int id, long index, GtkListStore **store, GtkTreeIter *it)
{
    GtkWidget *page = index >= 0 ? page_for(app, id) : NULL;
    if (!page)
        return false;
    *store = GTK_LIST_STORE(gtk_tree_view_get_model(view_of(page)));
    return gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(*store), it, NULL, (int)index);
}

void kp_playlists_mark_playing(kp_app *app, int id, long index)
{
    static int marked_id = -1;
    static long marked_index = -1;
    GtkListStore *store;
    GtkTreeIter it;
    if (row_iter(app, marked_id, marked_index, &store, &it))
        gtk_list_store_set(store, &it, COL_WEIGHT, PANGO_WEIGHT_NORMAL, -1);
    marked_id = id;
    marked_index = index;
    if (row_iter(app, id, index, &store, &it))
        gtk_list_store_set(store, &it, COL_WEIGHT, PANGO_WEIGHT_BOLD, -1);
}

void kp_playlists_refresh_row(kp_app *app, int id, long index)
{
    GtkListStore *store;
    GtkTreeIter it;
    long pi = kp_plset_find(app->playlists, id);
    const kp_entry *e = pi >= 0 ? kp_playlist_get(kp_plset_get(app->playlists, (size_t)pi),
                                                  (size_t)index)
                                : NULL;
    if (e && row_iter(app, id, index, &store, &it))
        set_row(store, &it, e);
    kp_playlists_update_status(app);
}

void kp_playlists_changed(kp_app *app)
{
    schedule_save(app);
}

int kp_playlists_selection(kp_app *app, long *index)
{
    *index = -1;
    GtkWidget *page = current_page(app, NULL);
    if (!page)
        return -1;
    GtkTreeModel *model;
    GList *rows = gtk_tree_selection_get_selected_rows(
        gtk_tree_view_get_selection(view_of(page)), &model);
    if (rows)
        *index = gtk_tree_path_get_indices(rows->data)[0];
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    return page_id(page);
}

void kp_playlists_update_status(kp_app *app)
{
    kp_playlist *pl;
    if (!current_page(app, &pl) || !pl) {
        gtk_label_set_text(GTK_LABEL(app->status), "");
        return;
    }
    size_t n = kp_playlist_count(pl), unknown;
    char total[32];
    kp_format_duration(total, sizeof total, kp_playlist_total_ms(pl, &unknown));
    char *text = n == 0 ? g_strdup(_("Empty playlist"))
                 : unknown == n
                     ? g_strdup_printf(ngettext("%zu track", "%zu tracks", n), n)
                     : g_strdup_printf(ngettext("%zu track, %s", "%zu tracks, %s", n), n, total);
    gtk_label_set_text(GTK_LABEL(app->status), text);
    g_free(text);
}

/* ---- adding files -------------------------------------------------------- */

static void add_locations(kp_app *app, GtkWidget *page, kp_playlist *pl, char **uris)
{
    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(view_of(page)));
    for (char **u = uris; *u; u++) {
        if (!**u)
            continue;
        /* Local files are kept as plain paths; anything else as the URI. */
        char *path = g_filename_from_uri(*u, NULL, NULL);
        kp_playlist_add(pl, path ? path : *u, NULL, -1);
        g_free(path);
        size_t i = kp_playlist_count(pl) - 1;
        append_row(store, i, kp_playlist_get(pl, i));
    }
    schedule_save(app);
    kp_playlists_update_status(app);
}

void kp_playlists_add_files(kp_app *app)
{
    kp_playlist *pl;
    GtkWidget *page = current_page(app, &pl);
    if (!page || !pl)
        return;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        _("Add files"), GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_OPEN, GTK_STOCK_CANCEL,
        GTK_RESPONSE_CANCEL, GTK_STOCK_ADD, GTK_RESPONSE_ACCEPT, NULL);
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
        add_locations(app, page, pl, uris);
        g_strfreev(uris);
        g_slist_free(list);
        char *folder = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(dlg));
        if (folder)
            kp_prefs_set_str(app->prefs, "playlists", "last_folder", folder);
        g_free(folder);
    }
    gtk_widget_destroy(dlg);
}

static void on_drop(GtkWidget *widget, GdkDragContext *ctx, gint x, gint y,
                    GtkSelectionData *sel, guint info, guint time, gpointer data)
{
    (void)widget;
    (void)x;
    (void)y;
    (void)info;
    kp_app *app = data;
    char **uris = gtk_selection_data_get_uris(sel);
    kp_playlist *pl;
    GtkWidget *page = current_page(app, &pl);
    if (uris && page && pl)
        add_locations(app, page, pl, uris);
    g_strfreev(uris);
    gtk_drag_finish(ctx, uris != NULL, FALSE, time);
}

void kp_playlists_remove_selected(kp_app *app)
{
    kp_playlist *pl;
    GtkWidget *page = current_page(app, &pl);
    if (!page || !pl)
        return;
    GtkTreeView *view = view_of(page);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    GtkTreeModel *model;
    GList *rows = gtk_tree_selection_get_selected_rows(sel, &model);
    if (!rows)
        return;
    /* From the last to the first, so the indexes stay right. The playing
     * entry keeps its place; if it goes, "next" continues from there. */
    bool playing_here = page_id(page) == app->playing.playlist_id && app->playing.index >= 0;
    long playing = app->playing.index;
    bool gone = false;
    rows = g_list_reverse(rows);
    for (GList *r = rows; r; r = r->next) {
        long i = gtk_tree_path_get_indices(r->data)[0];
        kp_playlist_remove(pl, (size_t)i);
        if (playing_here && i <= playing) {
            gone |= i == playing;
            playing--; /* gone: "next" is the entry that took its place */
        }
    }
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    refill(GTK_LIST_STORE(model), pl);
    if (playing_here) {
        app->playing.index = playing;
        kp_playlists_mark_playing(app, app->playing.playlist_id, gone ? -1 : playing);
    }
    schedule_save(app);
    kp_playlists_update_status(app);
}

static gboolean on_view_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    if (ev->keyval == GDK_Delete && !(ev->state & gtk_accelerator_get_default_mod_mask())) {
        kp_playlists_remove_selected(data);
        return TRUE;
    }
    return FALSE;
}

/* ---- tabs ---------------------------------------------------------------- */

static gboolean on_tab_click(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    kp_app *app = data;
    if (ev->type != GDK_2BUTTON_PRESS || ev->button != 1)
        return FALSE;
    GtkWidget *page = g_object_get_data(G_OBJECT(w), "kp-page");
    gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook),
                                  gtk_notebook_page_num(GTK_NOTEBOOK(app->notebook), page));
    kp_playlists_rename(app);
    return TRUE;
}

static GtkWidget *tab_label(kp_app *app, GtkWidget *page, const char *name)
{
    /* An event box so a double click on the tab renames it. */
    GtkWidget *box = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(box), FALSE);
    GtkWidget *label = gtk_label_new(name);
    gtk_container_add(GTK_CONTAINER(box), label);
    g_object_set_data(G_OBJECT(box), "kp-page", page);
    g_signal_connect(box, "button-press-event", G_CALLBACK(on_tab_click), app);
    gtk_widget_show_all(box);
    return box;
}

/* Double click or Enter on a row plays it. */
static void on_row_activated(GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col,
                             gpointer data)
{
    (void)col;
    kp_app *app = data;
    GtkWidget *page = gtk_widget_get_parent(GTK_WIDGET(view));
    kp_player_play_at(app, page_id(page), gtk_tree_path_get_indices(path)[0]);
}

static GtkWidget *add_page(kp_app *app, size_t i)
{
    kp_playlist *pl = kp_plset_get(app->playlists, i);
    GtkListStore *store =
        gtk_list_store_new(N_COLS, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    refill(store, pl);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    gtk_tree_view_set_rules_hint(GTK_TREE_VIEW(view), TRUE);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(view), COL_TITLE);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)),
                                GTK_SELECTION_MULTIPLE);

    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    g_object_set(r, "xalign", 1.0, NULL);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(view), -1, "#", r, "text", COL_NUM,
                                                "weight", COL_WEIGHT, NULL);
    r = gtk_cell_renderer_text_new();
    g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *col =
        gtk_tree_view_column_new_with_attributes(_("Title"), r, "text", COL_TITLE, "weight",
                                                 COL_WEIGHT, NULL);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);
    r = gtk_cell_renderer_text_new();
    g_object_set(r, "xalign", 1.0, NULL);
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(view), -1, _("Length"), r, "text",
                                                COL_LENGTH, "weight", COL_WEIGHT, NULL);

    static const GtkTargetEntry targets[] = {{(char *)"text/uri-list", 0, 0}};
    gtk_drag_dest_set(view, GTK_DEST_DEFAULT_ALL, targets, 1, GDK_ACTION_COPY);
    g_signal_connect(view, "drag-data-received", G_CALLBACK(on_drop), app);
    g_signal_connect(view, "key-press-event", G_CALLBACK(on_view_key), app);
    g_signal_connect(view, "row-activated", G_CALLBACK(on_row_activated), app);

    GtkWidget *page = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(page), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(page), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(page), view);
    g_object_set_data(G_OBJECT(page), "kp-id", GINT_TO_POINTER(kp_plset_id(app->playlists, i)));
    gtk_widget_show_all(page);

    gtk_notebook_insert_page(GTK_NOTEBOOK(app->notebook), page,
                             tab_label(app, page, kp_playlist_name(pl)), (int)i);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(app->notebook), page, TRUE);
    return page;
}

static void on_reordered(GtkNotebook *nb, GtkWidget *page, guint to, gpointer data)
{
    (void)nb;
    kp_app *app = data;
    long from = kp_plset_find(app->playlists, page_id(page));
    if (from >= 0)
        kp_plset_move(app->playlists, (size_t)from, to);
    schedule_save(app);
}

static gboolean status_idle(gpointer data)
{
    kp_playlists_update_status(data);
    return G_SOURCE_REMOVE;
}

static void on_switch(GtkNotebook *nb, gpointer page, guint num, gpointer data)
{
    (void)nb;
    (void)page;
    (void)num;
    kp_app *app = data;
    /* switch-page fires before the current page changes. */
    g_idle_add(status_idle, app);
    schedule_save(app);
}

void kp_playlists_new(kp_app *app)
{
    char *name = g_strdup_printf(_("Playlist %zu"), kp_plset_count(app->playlists) + 1);
    int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
    size_t at = kp_plset_insert(app->playlists, (size_t)(cur + 1), name);
    g_free(name);
    add_page(app, at);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), (int)at);
    schedule_save(app);
}

void kp_playlists_rename(kp_app *app)
{
    kp_playlist *pl;
    GtkWidget *page = current_page(app, &pl);
    if (!page || !pl)
        return;
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
            GtkWidget *box = gtk_notebook_get_tab_label(GTK_NOTEBOOK(app->notebook), page);
            gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(box))), name);
            schedule_save(app);
        }
        g_free(name);
    }
    gtk_widget_destroy(dlg);
}

void kp_playlists_close(kp_app *app)
{
    kp_playlist *pl;
    GtkWidget *page = current_page(app, &pl);
    if (!page || !pl)
        return;
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
    kp_player_playlist_closed(app, page_id(page));
    long i = kp_plset_find(app->playlists, page_id(page));
    gtk_widget_destroy(page);
    if (i >= 0)
        kp_plset_remove(app->playlists, (size_t)i);
    /* There is always at least one playlist. */
    if (kp_plset_count(app->playlists) == 0)
        kp_playlists_new(app);
    schedule_save(app);
}

void kp_playlists_init(kp_app *app)
{
    app->notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(app->notebook), TRUE);
    g_object_ref_sink(app->notebook); /* layout.c moves it around */
    if (kp_plset_count(app->playlists) == 0) {
        kp_plset_insert(app->playlists, 0, _("Playlist 1"));
        schedule_save(app);
    }
    for (size_t i = 0; i < kp_plset_count(app->playlists); i++)
        add_page(app, i);
    long active = kp_plset_find(app->playlists,
                                kp_prefs_get_int(app->prefs, "playlists", "active", 0, 0, 1000000));
    if (active >= 0)
        gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), (int)active);
    g_signal_connect(app->notebook, "page-reordered", G_CALLBACK(on_reordered), app);
    g_signal_connect(app->notebook, "switch-page", G_CALLBACK(on_switch), app);
}
