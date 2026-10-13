/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gdk/gdkkeysyms.h>

#include "app.h"
#include "i18n.h"
#include "plmodel.h"
#include "search.h"

/* "Jump to file" (Ctrl+J, as in Audacious): type words, see the entries
 * of the visible playlist that have them all, play one or queue it. The
 * searchable text of every entry is made once, when the dialog opens, and
 * dropped with it. */

#define FILTER_DELAY_MS 120

typedef struct {
    kp_app *app;
    int id;
    kp_playlist *pl;
    char **hay; /* kp_search_haystack per entry */
    size_t n;
    GtkWidget *dlg, *entry, *view, *count;
    GtkListStore *store; /* one int: the entry's index */
    guint timer;
} jump;

enum { RESP_QUEUE = 1, RESP_PLAY };

static void show_row(GtkTreeViewColumn *col, GtkCellRenderer *r, GtkTreeModel *m,
                     GtkTreeIter *it, gpointer data)
{
    (void)col;
    jump *j = data;
    int i;
    gtk_tree_model_get(m, it, 0, &i, -1);
    const kp_entry *e = kp_playlist_get(j->pl, (size_t)i);
    char *title = e ? kp_entry_display_title(e, true) : g_strdup("");
    int q = e ? kp_player_queue_pos(j->app, j->id, e) : 0;
    char *text = q ? g_strdup_printf("%d. %s  (%d)", i + 1, title, q)
                   : g_strdup_printf("%d. %s", i + 1, title);
    g_object_set(r, "text", text, NULL);
    g_free(text);
    g_free(title);
}

static void filter(jump *j)
{
    char **words = kp_search_words(gtk_entry_get_text(GTK_ENTRY(j->entry)));
    /* Detached while filling: one model change instead of thousands. */
    g_object_ref(j->store);
    gtk_tree_view_set_model(GTK_TREE_VIEW(j->view), NULL);
    gtk_list_store_clear(j->store);
    size_t found = 0;
    for (size_t i = 0; i < j->n; i++)
        if (kp_search_match(j->hay[i], words)) {
            gtk_list_store_insert_with_values(j->store, NULL, -1, 0, (int)i, -1);
            found++;
        }
    gtk_tree_view_set_model(GTK_TREE_VIEW(j->view), GTK_TREE_MODEL(j->store));
    g_object_unref(j->store);
    g_strfreev(words);
    char *text = g_strdup_printf(ngettext("%zu file", "%zu files", found), found);
    gtk_label_set_text(GTK_LABEL(j->count), text);
    g_free(text);
    if (found) {
        GtkTreePath *p = gtk_tree_path_new_first();
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(j->view), p, NULL, FALSE);
        gtk_tree_path_free(p);
    }
}

static gboolean filter_now(gpointer data)
{
    jump *j = data;
    j->timer = 0;
    filter(j);
    return G_SOURCE_REMOVE;
}

static void on_changed(GtkEditable *e, gpointer data)
{
    (void)e;
    jump *j = data;
    if (j->timer)
        g_source_remove(j->timer);
    j->timer = g_timeout_add(FILTER_DELAY_MS, filter_now, j);
}

/* The entry under the cursor of the result list, -1 if none. */
static long chosen(jump *j)
{
    GtkTreeIter it;
    GtkTreeModel *m;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(j->view)), &m,
                                         &it))
        return -1;
    int i;
    gtk_tree_model_get(m, &it, 0, &i, -1);
    return i;
}

/* Up and Down in the text field move in the list. */
static gboolean on_entry_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    jump *j = data;
    if (ev->keyval != GDK_Up && ev->keyval != GDK_Down && ev->keyval != GDK_Page_Up &&
        ev->keyval != GDK_Page_Down)
        return FALSE;
    gtk_widget_grab_focus(j->view);
    gboolean handled;
    g_signal_emit_by_name(j->view, "key-press-event", ev, &handled);
    gtk_widget_grab_focus(j->entry);
    gtk_editable_set_position(GTK_EDITABLE(j->entry), -1);
    return TRUE;
}

static void on_activated(GtkTreeView *v, GtkTreePath *p, GtkTreeViewColumn *c, gpointer data)
{
    (void)v;
    (void)p;
    (void)c;
    jump *j = data;
    gtk_dialog_response(GTK_DIALOG(j->dlg), RESP_PLAY);
}

void kp_jump_dialog(kp_app *app)
{
    jump j = {.app = app};
    j.pl = kp_playlists_visible(app, &j.id);
    if (!j.pl)
        return;
    j.n = kp_playlist_count(j.pl);
    j.hay = g_new(char *, j.n + 1);
    for (size_t i = 0; i < j.n; i++)
        j.hay[i] = kp_search_haystack(kp_playlist_get(j.pl, i));
    j.hay[j.n] = NULL;

    j.dlg = gtk_dialog_new_with_buttons(_("Jump to file"), GTK_WINDOW(app->window),
                                        GTK_DIALOG_MODAL | GTK_DIALOG_NO_SEPARATOR,
                                        GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE, _("_Queue"),
                                        RESP_QUEUE, GTK_STOCK_MEDIA_PLAY, RESP_PLAY, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(j.dlg), RESP_PLAY);
    gtk_window_set_default_size(GTK_WINDOW(j.dlg), 560, 420);
    GtkWidget *box = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 6);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(j.dlg)->vbox), box, TRUE, TRUE, 0);

    j.entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(j.entry), TRUE);
    gtk_box_pack_start(GTK_BOX(box), j.entry, FALSE, FALSE, 0);

    j.store = gtk_list_store_new(1, G_TYPE_INT);
    j.view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(j.store));
    g_object_unref(j.store);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(j.view), FALSE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(j.view), FALSE);
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_column_pack_start(col, r, TRUE);
    gtk_tree_view_column_set_cell_data_func(col, r, show_row, &j, NULL);
    gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_append_column(GTK_TREE_VIEW(j.view), col);
    gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(j.view), TRUE);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), j.view);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    j.count = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(j.count), 0.0f, 0.5f);
    gtk_box_pack_start(GTK_BOX(box), j.count, FALSE, FALSE, 0);

    g_signal_connect(j.entry, "changed", G_CALLBACK(on_changed), &j);
    g_signal_connect(j.entry, "key-press-event", G_CALLBACK(on_entry_key), &j);
    g_signal_connect(j.view, "row-activated", G_CALLBACK(on_activated), &j);
    filter(&j);
    gtk_widget_show_all(j.dlg);
    gtk_widget_grab_focus(j.entry);

    for (;;) {
        int resp = gtk_dialog_run(GTK_DIALOG(j.dlg));
        if (j.timer) {
            /* Enter typed fast: what is in the field now counts. */
            g_source_remove(j.timer);
            j.timer = 0;
            filter(&j);
        }
        long i = chosen(&j);
        if (resp == RESP_QUEUE && i >= 0) {
            kp_player_queue_toggle(app, j.id, i);
            kp_playlists_queue_changed(app);
            gtk_widget_queue_draw(j.view);
            continue;
        }
        if (resp == RESP_PLAY && i >= 0) {
            gtk_widget_hide(j.dlg);
            kp_playlists_select(app, j.id, i);
            kp_player_play_at(app, j.id, i);
        }
        if (resp != RESP_PLAY || i >= 0)
            break;
    }
    gtk_widget_destroy(j.dlg);
    g_strfreev(j.hay);
}
