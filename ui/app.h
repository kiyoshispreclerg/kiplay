/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_UI_APP_H
#define KP_UI_APP_H

#include <gtk/gtk.h>
#include <stdbool.h>

#include "plset.h"
#include "prefs.h"

/* What the parts of the window share. One per process, owned by window.c;
 * the widgets belong to the window, the prefs and the playlists to the
 * app (freed when the window goes). */

/* Where the video goes, relative to the playlists. Values go to
 * kiplay.ini by name (kp_video_pos_names): append only. */
typedef enum {
    KP_VIDEO_ABOVE,
    KP_VIDEO_LEFT,
    KP_VIDEO_RIGHT,
    KP_VIDEO_POS_COUNT
} kp_video_pos;

typedef enum {
    KP_CONTROLS_BOTTOM,
    KP_CONTROLS_TOP,
    KP_CONTROLS_POS_COUNT
} kp_controls_pos;

extern const char *const kp_video_pos_names[];
extern const char *const kp_controls_pos_names[];

typedef struct {
    GtkWidget *window;
    GtkAccelGroup *accel;
    kp_prefs *prefs;
    kp_plset *playlists;

    /* The pieces layout.c arranges inside body. */
    GtkWidget *body;     /* vbox under the menu bar */
    GtkWidget *video;    /* black drawing area; its X window is the backend's --wid */
    GtkWidget *notebook; /* one page per playlist */
    GtkWidget *controls; /* the transport bar */
    GtkWidget *paned;    /* holds video + notebook while the video shows */
    GtkWidget *status;   /* label at the bottom */

    kp_video_pos video_pos;
    kp_controls_pos controls_pos;
    bool show_video;
} kp_app;

/* layout.c: (re)arranges video, playlists and controls from the fields
 * above. Call kp_layout_save before changing them, so the divider of the
 * layout being left is remembered under its own key. The video area keeps
 * its X window across changes (it is reparented, not recreated), so a
 * running backend keeps drawing. */
void kp_layout_apply(kp_app *app);
/* Remembers the paned divider in the prefs. */
void kp_layout_save(kp_app *app);

/* playlists.c */
void kp_playlists_init(kp_app *app);
void kp_playlists_new(kp_app *app);
void kp_playlists_rename(kp_app *app);
void kp_playlists_close(kp_app *app);
void kp_playlists_add_files(kp_app *app);
void kp_playlists_remove_selected(kp_app *app);
/* Saves changed playlists and the tab order. */
void kp_playlists_save(kp_app *app);
/* Updates the status line for the visible playlist. */
void kp_playlists_update_status(kp_app *app);

/* controls.c: the transport bar (still without a backend). */
GtkWidget *kp_controls_new(kp_app *app);

#endif
