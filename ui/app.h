/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_UI_APP_H
#define KP_UI_APP_H

#include <gtk/gtk.h>
#include <stdbool.h>

#include "backend.h"
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

/* What is playing. The playlist is named by id (it survives reordering
 * and other tabs closing); index -1 = nothing. */
typedef struct {
    kp_backend *backend; /* NULL until something first plays */
    int playlist_id;
    long index;
    bool loaded, paused, has_video;
    double position, duration; /* seconds; duration <= 0 = unknown */
    char *title;               /* from the player, may be NULL */
    int failures;              /* files in a row that wouldn't play */
    int crashes;               /* backend restarts in the last minute */
    gint64 first_crash_us;
} kp_playing;

typedef struct {
    GtkWidget *window;
    GtkWidget *menubar;
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

    /* Transport bar widgets the player updates. */
    GtkWidget *seek, *time_label, *volume, *play_btn, *pause_btn;
    bool seek_dragging; /* the user holds the seek bar: don't move it */

    kp_video_pos video_pos;
    kp_controls_pos controls_pos;
    bool video_forced; /* View > Video area: always shown */
    bool show_video;   /* what layout.c shows: forced, or a video plays */
    bool fullscreen;

    kp_playing playing;
} kp_app;

/* layout.c: (re)arranges video, playlists and controls from the fields
 * above. Call kp_layout_save before changing them, so the divider of the
 * layout being left is remembered under its own key. The video area keeps
 * its X window across changes (it is reparented, not recreated), so a
 * running backend keeps drawing. */
void kp_layout_apply(kp_app *app);
/* Remembers the paned divider in the prefs. */
void kp_layout_save(kp_app *app);
/* Shows or hides the video area from video_forced and playing.has_video;
 * does nothing if that doesn't change. */
void kp_layout_update_video(kp_app *app);
/* Only the video (and nothing else) on the whole screen, or back. */
void kp_layout_set_fullscreen(kp_app *app, bool on);

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
/* The visible playlist's id and its first selected row (-1 if none). */
int kp_playlists_selection(kp_app *app, long *index);
/* Bold for the playing row (index -1 = none), in the tab with that id. */
void kp_playlists_mark_playing(kp_app *app, int id, long index);
/* Redraws one row after its entry changed (title, length). */
void kp_playlists_refresh_row(kp_app *app, int id, long index);
/* Files from the command line or another kiplay: added to the visible
 * playlist (ones already there are found, not repeated). Returns its id;
 * *first = the index of the first file, -1 if none. */
int kp_playlists_open(kp_app *app, char **locations, long *first);
/* Something changed in a playlist: write it soon. */
void kp_playlists_changed(kp_app *app);

/* controls.c: the transport bar. */
GtkWidget *kp_controls_new(kp_app *app);
/* Time label and seek bar from app->playing. */
void kp_controls_update(kp_app *app);

/* player.c: plays the playlists through the backend. */
void kp_player_init(kp_app *app);
void kp_player_free(kp_app *app);
/* Plays entry index of the playlist with that id. */
void kp_player_play_at(kp_app *app, int id, long index);
/* The Play button: resume, or start the selection / where it stopped. */
void kp_player_play(kp_app *app);
void kp_player_pause(kp_app *app);
void kp_player_toggle_pause(kp_app *app);
void kp_player_stop(kp_app *app);
void kp_player_next(kp_app *app);
void kp_player_prev(kp_app *app);
void kp_player_seek(kp_app *app, double seconds);
void kp_player_seek_relative(kp_app *app, double seconds);
void kp_player_set_volume(kp_app *app, int volume);
/* Adds the files and, unless enqueue, plays the first; raises the window. */
void kp_player_open(kp_app *app, char **locations, bool enqueue);
/* A playlist was closed: stop if it was playing. */
void kp_player_playlist_closed(kp_app *app, int id);

#endif
