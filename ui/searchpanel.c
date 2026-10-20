/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gdk/gdkkeysyms.h>

#include "app.h"
#include "i18n.h"
#include "plmodel.h"
#include "search.h"

/* The search panel (View: beside the playlists): type words, see the
 * entries of every playlist that have them all, with the playlist each is
 * in; a double click plays it and shows its tab at its row.
 *
 * Nothing of it exists while it is closed: the widgets, the searchable
 * text of every entry (made when the panel opens, dropped when it closes)
 * and the timer all go with it. */

#define FILTER_DELAY_MS 150
#define PANEL_WIDTH 300
#define MAX_ROWS 2000 /* shown; the count says how many matched */

typedef struct {
    int id;
    long index;
    char *hay; /* kp_search_haystack */
} item;

typedef struct {
    kp_app *app;
    GtkWidget *box, *entry, *view, *count;
    GtkListStore *store; /* playlist id, index */
    item *items;
    size_t n;
    bool stale; /* a playlist changed: the items must be made again */
    guint timer;
} panel;

static panel *sp; /* NULL while closed */

enum { COL_ID, COL_INDEX };

static void items_free(panel *p)
{
    for (size_t i = 0; i < p->n; i++)
        g_free(p->items[i].hay);
    g_free(p->items);
    p->items = NULL;
    p->n = 0;
}

static void items_make(panel *p)
{
    items_free(p);
    kp_plset *set = p->app->playlists;
    size_t total = 0;
    for (size_t k = 0; k < kp_plset_count(set); k++)
        total += kp_playlist_count(kp_plset_get(set, k));
    p->items = g_new(item, total ? total : 1);
    for (size_t k = 0; k < kp_plset_count(set); k++) {
        kp_playlist *pl = kp_plset_get(set, k);
        int id = kp_plset_id(set, k);
        for (size_t i = 0; i < kp_playlist_count(pl); i++) {
            p->items[p->n].id = id;
            p->items[p->n].index = (long)i;
            p->items[p->n].hay = kp_search_haystack(kp_playlist_get(pl, i));
            p->n++;
        }
    }
    p->stale = false;
}

static void show_row(GtkTreeViewColumn *col, GtkCellRenderer *r, GtkTreeModel *m, GtkTreeIter *it,
                     gpointer data)
{
    (void)col;
    panel *p = data;
    int id;
    long index;
    gtk_tree_model_get(m, it, COL_ID, &id, COL_INDEX, &index, -1);
    kp_playlist *pl = kp_playlists_get(p->app, id);
    const kp_entry *e = pl && index >= 0 ? kp_playlist_get(pl, (size_t)index) : NULL;
    if (!e) {
        g_object_set(r, "text", "", NULL);
        return;
    }
    char *title = kp_entry_display_title(e, true);
    char *t = g_markup_escape_text(title, -1);
    char *n = g_markup_escape_text(kp_playlist_name(pl), -1);
    char *markup = g_strdup_printf("%s\n<small>%s  ·  #%ld</small>", t, n, index + 1);
    g_object_set(r, "markup", markup, NULL);
    g_free(markup);
    g_free(n);
    g_free(t);
    g_free(title);
}

static void filter(panel *p)
{
    if (p->stale)
        items_make(p);
    char **words = kp_search_words(gtk_entry_get_text(GTK_ENTRY(p->entry)));
    size_t found = 0, shown = 0;
    /* Detached while filling: one model change instead of thousands. */
    g_object_ref(p->store);
    gtk_tree_view_set_model(GTK_TREE_VIEW(p->view), NULL);
    gtk_list_store_clear(p->store);
    if (words && words[0])
        for (size_t i = 0; i < p->n; i++)
            if (kp_search_match(p->items[i].hay, words)) {
                found++;
                if (shown < MAX_ROWS) {
                    gtk_list_store_insert_with_values(p->store, NULL, -1, COL_ID, p->items[i].id,
                                                      COL_INDEX, p->items[i].index, -1);
                    shown++;
                }
            }
    gtk_tree_view_set_model(GTK_TREE_VIEW(p->view), GTK_TREE_MODEL(p->store));
    g_object_unref(p->store);
    g_strfreev(words);
    char *text = g_strdup_printf(ngettext("%zu file", "%zu files", found), found);
    if (found > shown) {
        char *more = g_strdup_printf(_("%s (first %zu shown)"), text, shown);
        g_free(text);
        text = more;
    }
    gtk_label_set_text(GTK_LABEL(p->count), text);
    g_free(text);
    if (shown) {
        GtkTreePath *path = gtk_tree_path_new_first();
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(p->view), path, NULL, FALSE);
        gtk_tree_path_free(path);
    }
}

static gboolean filter_now(gpointer data)
{
    panel *p = data;
    p->timer = 0;
    filter(p);
    return G_SOURCE_REMOVE;
}

static void later(panel *p)
{
    if (p->timer)
        g_source_remove(p->timer);
    p->timer = g_timeout_add(FILTER_DELAY_MS, filter_now, p);
}

static void on_changed(GtkEditable *e, gpointer data)
{
    (void)e;
    later(data);
}

static void close_panel(kp_app *app)
{
    panel *p = sp;
    if (!p)
        return;
    sp = NULL;
    if (p->timer)
        g_source_remove(p->timer);
    gtk_widget_destroy(p->box);
    items_free(p);
    g_free(p);
    kp_playlists_focus(app);
}

/* Plays the entry of a row and shows it in its playlist. */
static void play_row(panel *p, GtkTreeModel *m, GtkTreeIter *it)
{
    int id;
    long index;
    gtk_tree_model_get(m, it, COL_ID, &id, COL_INDEX, &index, -1);
    kp_playlist *pl = kp_playlists_get(p->app, id);
    if (!pl || index < 0 || (size_t)index >= kp_playlist_count(pl))
        return;
    kp_playlists_select(p->app, id, index);
    kp_player_play_at(p->app, id, index);
}

static void on_activated(GtkTreeView *v, GtkTreePath *path, GtkTreeViewColumn *c, gpointer data)
{
    (void)c;
    panel *p = data;
    GtkTreeModel *m = gtk_tree_view_get_model(v);
    GtkTreeIter it;
    if (m && gtk_tree_model_get_iter(m, &it, path))
        play_row(p, m, &it);
}

static gboolean on_entry_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    panel *p = data;
    switch (ev->keyval) {
    case GDK_Escape:
        close_panel(p->app);
        return TRUE;
    case GDK_Return:
    case GDK_KP_Enter: {
        if (p->timer) { /* typed fast: what is in the field now counts */
            g_source_remove(p->timer);
            p->timer = 0;
            filter(p);
        }
        GtkTreeModel *m;
        GtkTreeIter it;
        if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(p->view)),
                                            &m, &it))
            play_row(p, m, &it);
        return TRUE;
    }
    case GDK_Up:
    case GDK_Down:
    case GDK_Page_Up:
    case GDK_Page_Down: {
        gtk_widget_grab_focus(p->view);
        gboolean handled;
        g_signal_emit_by_name(p->view, "key-press-event", ev, &handled);
        gtk_widget_grab_focus(p->entry);
        gtk_editable_set_position(GTK_EDITABLE(p->entry), -1);
        return TRUE;
    }
    default:
        return FALSE;
    }
}

static void open_panel(kp_app *app)
{
    if (app->fullscreen || app->compact)
        return; /* the playlists aren't there */
    if (!app->playlists_shown && app->playlists_item)
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app->playlists_item), TRUE);
    panel *p = g_new0(panel, 1);
    p->app = app;
    sp = p;
    items_make(p);

    p->box = gtk_vbox_new(FALSE, 4);
    gtk_container_set_border_width(GTK_CONTAINER(p->box), 4);
    gtk_widget_set_size_request(p->box, PANEL_WIDTH, -1);
    p->entry = gtk_entry_new();
    gtk_entry_set_icon_from_stock(GTK_ENTRY(p->entry), GTK_ENTRY_ICON_PRIMARY, GTK_STOCK_FIND);
    gtk_box_pack_start(GTK_BOX(p->box), p->entry, FALSE, FALSE, 0);

    p->store = gtk_list_store_new(2, G_TYPE_INT, G_TYPE_LONG);
    p->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(p->store));
    g_object_unref(p->store);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(p->view), FALSE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(p->view), FALSE);
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_column_pack_start(col, r, TRUE);
    gtk_tree_view_column_set_cell_data_func(col, r, show_row, p, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(p->view), col);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), p->view);
    gtk_box_pack_start(GTK_BOX(p->box), scroll, TRUE, TRUE, 0);

    p->count = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(p->count), 0.0f, 0.5f);
    gtk_box_pack_start(GTK_BOX(p->box), p->count, FALSE, FALSE, 0);

    g_signal_connect(p->entry, "changed", G_CALLBACK(on_changed), p);
    g_signal_connect(p->entry, "key-press-event", G_CALLBACK(on_entry_key), p);
    g_signal_connect(p->view, "row-activated", G_CALLBACK(on_activated), p);
    filter(p);
    gtk_box_pack_start(GTK_BOX(app->side), p->box, FALSE, FALSE, 0);
    gtk_widget_show_all(p->box);
    gtk_widget_grab_focus(p->entry);
}

void kp_search_panel_toggle(kp_app *app)
{
    if (sp)
        close_panel(app);
    else
        open_panel(app);
}

void kp_search_panel_changed(kp_app *app)
{
    (void)app;
    if (!sp)
        return;
    sp->stale = true;
    later(sp);
}

void kp_search_panel_close(kp_app *app)
{
    if (sp) {
        panel *p = sp;
        sp = NULL;
        if (p->timer)
            g_source_remove(p->timer);
        gtk_widget_destroy(p->box);
        items_free(p);
        g_free(p);
    }
    (void)app;
}
