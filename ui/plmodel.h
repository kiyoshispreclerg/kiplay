/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_UI_PLMODEL_H
#define KP_UI_PLMODEL_H

#include <gtk/gtk.h>
#include <stdbool.h>

#include "playlist.h"

/* A GtkTreeModel that shows a kp_playlist as it is, without copying it:
 * a GtkListStore would duplicate every string of every row (megabytes at
 * tens of thousands of entries) and fill them all before the first draw.
 * The text of a row is made only when the view asks for it, which in
 * fixed-height mode is only for the rows on screen.
 *
 * The playlist changes through kp_playlist_* and then the model is told
 * (kp_plmodel_inserted and the others), which tells the views.
 *
 * Memory: the model doesn't own the playlist; it must outlive the model's
 * last use. */

enum {
    KP_COL_NUM,    /* "12", or "12 (1)" when queued */
    KP_COL_TITLE,  /* "Artist - Title" (or the title alone, see below), else the file name */
    KP_COL_ARTIST,
    KP_COL_ALBUM,
    KP_COL_LENGTH, /* "3:25" */
    KP_COL_PATH,   /* where the file is, readable */
    KP_COL_WEIGHT, /* bold for the playing row */
    KP_N_COLS
};

#define KP_TYPE_PLMODEL (kp_plmodel_get_type())
#define KP_PLMODEL(o) (G_TYPE_CHECK_INSTANCE_CAST((o), KP_TYPE_PLMODEL, KpPlModel))

typedef struct _KpPlModel KpPlModel;
typedef struct _KpPlModelClass KpPlModelClass;

GType kp_plmodel_get_type(void);

/* Position in the play queue (1, 2...), 0 if not queued. */
typedef int (*kp_queue_pos_fn)(void *ud, const kp_entry *e);
/* A row shown on screen has nothing known yet: time to read its tags. */
typedef void (*kp_need_tags_fn)(void *ud, KpPlModel *m, long index);

KpPlModel *kp_plmodel_new(kp_playlist *pl, int id);
kp_playlist *kp_plmodel_playlist(KpPlModel *m);
int kp_plmodel_id(KpPlModel *m);

void kp_plmodel_set_hooks(KpPlModel *m, kp_queue_pos_fn queue_pos, kp_need_tags_fn need_tags,
                          void *ud);
/* With an artist column the title column shows the title alone. The
 * views must be redrawn after (every row changes; signalling each one
 * would cost tens of thousands of signals for a redraw). */
void kp_plmodel_set_split_artist(KpPlModel *m, bool split);
/* Bold row; -1 none. */
void kp_plmodel_set_playing(KpPlModel *m, long index);
long kp_plmodel_playing(KpPlModel *m);

/* After the playlist changed: n entries inserted at first; entry index
 * removed; the order changed (new_order as kp_playlist_move gives it);
 * one row's text changed. */
void kp_plmodel_inserted(KpPlModel *m, long first, long n);
void kp_plmodel_deleted(KpPlModel *m, long index);
void kp_plmodel_reordered(KpPlModel *m, int *new_order);
void kp_plmodel_changed(KpPlModel *m, long index);

/* Index of the row an iter points at. */
long kp_plmodel_index(GtkTreeIter *it);

/* The text the title column shows for e. g_free() it. */
char *kp_entry_display_title(const kp_entry *e, bool with_artist);

#endif
