/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "plmodel.h"

#include <string.h>

#include "util.h"

struct _KpPlModel {
    GObject parent;
    kp_playlist *pl;
    int id;
    gint stamp;
    long playing;
    bool split_artist;
    kp_queue_pos_fn queue_pos;
    kp_need_tags_fn need_tags;
    void *ud;
};

struct _KpPlModelClass {
    GObjectClass parent;
};

static void kp_plmodel_iface_init(GtkTreeModelIface *iface);
static void kp_plmodel_drag_init(GtkTreeDragSourceIface *iface);

/* A drag source too: GTK 2's tree view only starts dragging rows of a
 * model that says they can be dragged. What the drag carries is just
 * "rows" (the moved rows are the selection; see playlists.c). */
G_DEFINE_TYPE_WITH_CODE(KpPlModel, kp_plmodel, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GTK_TYPE_TREE_MODEL, kp_plmodel_iface_init)
                            G_IMPLEMENT_INTERFACE(GTK_TYPE_TREE_DRAG_SOURCE,
                                                  kp_plmodel_drag_init))

static void kp_plmodel_init(KpPlModel *m)
{
    m->stamp = g_random_int();
    m->playing = -1;
}

static void kp_plmodel_class_init(KpPlModelClass *klass)
{
    (void)klass;
}

/* ---- the GtkTreeModel interface ----------------------------------------- */

static GtkTreeModelFlags get_flags(GtkTreeModel *model)
{
    (void)model;
    return GTK_TREE_MODEL_LIST_ONLY;
}

static gint get_n_columns(GtkTreeModel *model)
{
    (void)model;
    return KP_N_COLS;
}

static GType get_column_type(GtkTreeModel *model, gint col)
{
    (void)model;
    return col == KP_COL_WEIGHT ? G_TYPE_INT : G_TYPE_STRING;
}

static gboolean set_iter(KpPlModel *m, GtkTreeIter *it, long i)
{
    if (i < 0 || (size_t)i >= kp_playlist_count(m->pl)) {
        it->stamp = 0;
        return FALSE;
    }
    it->stamp = m->stamp;
    it->user_data = GINT_TO_POINTER((gint)i);
    return TRUE;
}

long kp_plmodel_index(GtkTreeIter *it)
{
    return GPOINTER_TO_INT(it->user_data);
}

static gboolean get_iter(GtkTreeModel *model, GtkTreeIter *it, GtkTreePath *path)
{
    if (gtk_tree_path_get_depth(path) != 1)
        return FALSE;
    return set_iter(KP_PLMODEL(model), it, gtk_tree_path_get_indices(path)[0]);
}

static GtkTreePath *get_path(GtkTreeModel *model, GtkTreeIter *it)
{
    g_return_val_if_fail(it->stamp == KP_PLMODEL(model)->stamp, NULL);
    return gtk_tree_path_new_from_indices((gint)kp_plmodel_index(it), -1);
}

char *kp_entry_display_title(const kp_entry *e, bool with_artist)
{
    if (e->title)
        return with_artist && e->artist ? g_strdup_printf("%s - %s", e->artist, e->title)
                                        : g_strdup(e->title);
    /* Not known yet: the file name, without the folder. */
    char *path = g_str_has_prefix(e->location, "file://")
                     ? g_filename_from_uri(e->location, NULL, NULL) : NULL;
    if (!path && g_str_has_prefix(e->location, "file://")) {
        /* "file:///a/b.nsf?3": the query makes g_filename_from_uri fail. */
        char *copy = g_strdup(e->location);
        char *q = strrchr(copy, '?');
        if (q)
            *q = '\0';
        path = g_filename_from_uri(copy, NULL, NULL);
        g_free(copy);
        const char *sub = strrchr(e->location, '?');
        if (path && sub) {
            char *base = g_path_get_basename(path);
            char *shown = g_filename_display_name(base);
            char *with = g_strdup_printf("%s (%s)", shown, sub + 1);
            g_free(shown);
            g_free(base);
            g_free(path);
            return with;
        }
    }
    const char *src = path ? path : e->location;
    char *base = strchr(src, '/') && src[0] == '/' ? g_path_get_basename(src) : g_strdup(src);
    char *shown = g_filename_display_name(base);
    g_free(base);
    g_free(path);
    return shown;
}

static char *display_path(const kp_entry *e)
{
    if (e->location[0] == '/')
        return g_filename_display_name(e->location);
    if (g_str_has_prefix(e->location, "file://")) {
        char *s = g_uri_unescape_string(e->location + strlen("file://"), NULL);
        if (s && g_utf8_validate(s, -1, NULL))
            return s;
        g_free(s);
    }
    return g_strdup(e->location);
}

static void get_value(GtkTreeModel *model, GtkTreeIter *it, gint col, GValue *value)
{
    KpPlModel *m = KP_PLMODEL(model);
    long i = kp_plmodel_index(it);
    const kp_entry *e = kp_playlist_get(m->pl, (size_t)i);
    g_value_init(value, get_column_type(model, col));
    if (!e)
        return;
    /* Something on screen that nobody has looked at yet. */
    if (!e->scanned && (!e->title || e->length_ms < 0) && m->need_tags &&
        (col == KP_COL_TITLE || col == KP_COL_LENGTH))
        m->need_tags(m->ud, m, i);
    char buf[32];
    switch (col) {
    case KP_COL_NUM: {
        int q = m->queue_pos ? m->queue_pos(m->ud, e) : 0;
        if (q > 0)
            g_value_take_string(value, g_strdup_printf("%ld (%d)", i + 1, q));
        else {
            g_snprintf(buf, sizeof buf, "%ld", i + 1);
            g_value_set_string(value, buf);
        }
        break;
    }
    case KP_COL_TITLE:
        g_value_take_string(value, kp_entry_display_title(e, !m->split_artist));
        break;
    case KP_COL_ARTIST:
        g_value_set_static_string(value, e->artist);
        break;
    case KP_COL_ALBUM:
        g_value_set_static_string(value, e->album);
        break;
    case KP_COL_LENGTH:
        buf[0] = '\0';
        if (e->length_ms >= 0)
            kp_format_duration(buf, sizeof buf, e->length_ms);
        g_value_set_string(value, buf);
        break;
    case KP_COL_PATH:
        g_value_take_string(value, display_path(e));
        break;
    case KP_COL_WEIGHT:
        g_value_set_int(value, i == m->playing ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
        break;
    default:
        break;
    }
}

static gboolean iter_next(GtkTreeModel *model, GtkTreeIter *it)
{
    return set_iter(KP_PLMODEL(model), it, kp_plmodel_index(it) + 1);
}

static gboolean iter_nth_child(GtkTreeModel *model, GtkTreeIter *it, GtkTreeIter *parent, gint n)
{
    if (parent) {
        it->stamp = 0;
        return FALSE;
    }
    return set_iter(KP_PLMODEL(model), it, n);
}

static gboolean iter_children(GtkTreeModel *model, GtkTreeIter *it, GtkTreeIter *parent)
{
    return iter_nth_child(model, it, parent, 0);
}

static gboolean iter_has_child(GtkTreeModel *model, GtkTreeIter *it)
{
    (void)model;
    (void)it;
    return FALSE;
}

static gint iter_n_children(GtkTreeModel *model, GtkTreeIter *it)
{
    return it ? 0 : (gint)kp_playlist_count(KP_PLMODEL(model)->pl);
}

static gboolean iter_parent(GtkTreeModel *model, GtkTreeIter *it, GtkTreeIter *child)
{
    (void)model;
    (void)child;
    it->stamp = 0;
    return FALSE;
}

static void kp_plmodel_iface_init(GtkTreeModelIface *iface)
{
    iface->get_flags = get_flags;
    iface->get_n_columns = get_n_columns;
    iface->get_column_type = get_column_type;
    iface->get_iter = get_iter;
    iface->get_path = get_path;
    iface->get_value = get_value;
    iface->iter_next = iter_next;
    iface->iter_children = iter_children;
    iface->iter_has_child = iter_has_child;
    iface->iter_n_children = iter_n_children;
    iface->iter_nth_child = iter_nth_child;
    iface->iter_parent = iter_parent;
}

static gboolean row_draggable(GtkTreeDragSource *src, GtkTreePath *path)
{
    (void)src;
    (void)path;
    return TRUE;
}

static gboolean drag_data_get(GtkTreeDragSource *src, GtkTreePath *path, GtkSelectionData *sel)
{
    (void)src;
    (void)path;
    gtk_selection_data_set(sel, sel->target, 8, (const guchar *)"rows", 4);
    return TRUE;
}

static gboolean drag_data_delete(GtkTreeDragSource *src, GtkTreePath *path)
{
    (void)src;
    (void)path;
    return TRUE; /* the drop moved them already */
}

static void kp_plmodel_drag_init(GtkTreeDragSourceIface *iface)
{
    iface->row_draggable = row_draggable;
    iface->drag_data_get = drag_data_get;
    iface->drag_data_delete = drag_data_delete;
}

/* ---- ours ---------------------------------------------------------------- */

KpPlModel *kp_plmodel_new(kp_playlist *pl, int id)
{
    KpPlModel *m = g_object_new(KP_TYPE_PLMODEL, NULL);
    m->pl = pl;
    m->id = id;
    return m;
}

kp_playlist *kp_plmodel_playlist(KpPlModel *m)
{
    return m->pl;
}

int kp_plmodel_id(KpPlModel *m)
{
    return m->id;
}

void kp_plmodel_set_hooks(KpPlModel *m, kp_queue_pos_fn queue_pos, kp_need_tags_fn need_tags,
                          void *ud)
{
    m->queue_pos = queue_pos;
    m->need_tags = need_tags;
    m->ud = ud;
}

void kp_plmodel_set_split_artist(KpPlModel *m, bool split)
{
    m->split_artist = split;
}

void kp_plmodel_changed(KpPlModel *m, long index)
{
    GtkTreeIter it;
    if (!set_iter(m, &it, index))
        return;
    GtkTreePath *path = gtk_tree_path_new_from_indices((gint)index, -1);
    gtk_tree_model_row_changed(GTK_TREE_MODEL(m), path, &it);
    gtk_tree_path_free(path);
}

void kp_plmodel_set_playing(KpPlModel *m, long index)
{
    long old = m->playing;
    m->playing = index;
    if (old != index) {
        kp_plmodel_changed(m, old);
        kp_plmodel_changed(m, index);
    }
}

long kp_plmodel_playing(KpPlModel *m)
{
    return m->playing;
}

void kp_plmodel_inserted(KpPlModel *m, long first, long n)
{
    if (m->playing >= first)
        m->playing += n;
    for (long i = first; i < first + n; i++) {
        GtkTreeIter it;
        if (!set_iter(m, &it, i))
            break;
        GtkTreePath *path = gtk_tree_path_new_from_indices((gint)i, -1);
        gtk_tree_model_row_inserted(GTK_TREE_MODEL(m), path, &it);
        gtk_tree_path_free(path);
    }
}

void kp_plmodel_deleted(KpPlModel *m, long index)
{
    if (m->playing == index)
        m->playing = -1;
    else if (m->playing > index)
        m->playing--;
    GtkTreePath *path = gtk_tree_path_new_from_indices((gint)index, -1);
    gtk_tree_model_row_deleted(GTK_TREE_MODEL(m), path);
    gtk_tree_path_free(path);
}

void kp_plmodel_reordered(KpPlModel *m, int *new_order)
{
    long n = (long)kp_playlist_count(m->pl);
    for (long i = 0; i < n && m->playing >= 0; i++)
        if (new_order[i] == m->playing) {
            m->playing = i;
            break;
        }
    GtkTreePath *path = gtk_tree_path_new();
    gtk_tree_model_rows_reordered(GTK_TREE_MODEL(m), path, NULL, new_order);
    gtk_tree_path_free(path);
}
