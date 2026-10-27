/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_UI_APP_H
#define KP_UI_APP_H

#include <gtk/gtk.h>
#include <stdbool.h>

#include "backend.h"
#include "filesettings.h"
#include "instance.h"
#include "plset.h"
#include "prefs.h"
#include "tagreader.h"

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

/* Optional playlist columns (the title is always there). Kept in
 * kiplay.ini by name (kp_column_names, in bit order): append only. */
enum {
    KP_COLUMN_NUM = 1 << 0,
    KP_COLUMN_ARTIST = 1 << 1,
    KP_COLUMN_ALBUM = 1 << 2,
    KP_COLUMN_LENGTH = 1 << 3,
    KP_COLUMN_PATH = 1 << 4,
    KP_COLUMN_QUEUE = 1 << 5,
    KP_COLUMN_COUNT = 6,
};
#define KP_COLUMNS_DEFAULT (KP_COLUMN_NUM | KP_COLUMN_LENGTH)
extern const char *const kp_column_names[];

/* An entry followed while its playlist changes: the playlist's id and the
 * entry's own location string (kp_playlist_find_entry), plus where it was
 * last seen, to find it fast. */
typedef struct {
    int id;
    const char *location;
    long hint;
} kp_entry_ref;

/* What is playing. The playlist is named by id (it survives reordering
 * and other tabs closing); index -1 = nothing. */
typedef struct {
    /* Two engines, each started the first time it is needed: mpv for
     * video (and everything else), kiplay-audio for music. backend is the
     * one playing now. */
    kp_backend *backend, *mpv, *audio;
    guint mpv_idle_timer; /* lets an unused mpv go (it holds ~20 MB) */
    int queued_id;        /* the entry handed to the engine for gapless: */
    long queued;          /* playlist id and index; -1 none */
    int playlist_id;
    long index;
    const char *location; /* the entry's own string (kp_entry_ref), NULL none */
    bool loaded, paused, has_video;
    bool starting;             /* handed to the engine, not loaded yet */
    double position, duration; /* seconds; duration <= 0 = unknown */
    char *title;               /* from the player, may be NULL */
    int failures;              /* files in a row that wouldn't play */
    int crashes;               /* backend restarts in the last minute */
    gint64 first_crash_us;
    bool stop_after;           /* stop when the current file ends */
    bool muted;                /* volume 0 for the engines, the setting kept */
    /* What is remembered of the playing file (kp_file_settings): the
     * choices made in the Video/Audio/Subtitles menus land here and are
     * written when the file stops. */
    kp_file_settings file;
    bool file_local;           /* a local file: it can have settings */
    bool file_stored;          /* settings are on disk for it */
    char *mpv_signature;       /* kp_mpv_pref_signature when mpv started */
    char *audio_device;        /* the device kiplay-audio started with */
    char *audio_soundfont;     /* and its SoundFont */
    /* Played by "next" (for "previous"), and, with shuffle, the entries of
     * the playing playlist already heard this round. */
    GArray *history;           /* kp_entry_ref, oldest first */
    GHashTable *heard;         /* location strings */
    int heard_id;              /* the playlist heard is about */
} kp_playing;

typedef struct {
    GtkWidget *window;
    GtkWidget *menubar;
    kp_prefs *prefs;
    kp_plset *playlists;

    /* The pieces layout.c arranges inside body. */
    GtkWidget *body;     /* vbox under the menu bar */
    GtkWidget *video;    /* black drawing area; its X window is the backend's --wid */
    GtkWidget *notebook; /* one page per playlist */
    GtkWidget *side;     /* what layout.c places for the playlists: the notebook,
                          * and the search panel beside it while open */
    GtkWidget *controls; /* the transport bar */
    GtkWidget *paned;    /* holds video + notebook while the video shows */
    GtkWidget *status;   /* label at the bottom */
    GtkWidget *chat;     /* the chat panel (chatpanel.c); beside the video when wanted */
    GtkWidget *chat_item; /* View > Chat */
    bool chat_available; /* the playing video has a chat file, indexed */
    bool chat_shown;     /* what View > Chat says */

    /* Transport bar widgets the player updates. */
    GtkWidget *seek, *time_label, *volume, *play_btn, *pause_btn;
    GtkWidget *stop_after_item; /* Playback > Stop after this track */
    GtkWidget *repeat_btn, *shuffle_btn, *playlists_btn;
    GtkWidget *playlists_item; /* View > Playlists */
    /* View menu items the preferences dialog drives (their handlers do
     * the work). */
    GtkWidget *show_video_item, *compact_item;
    GtkWidget *video_pos_items[KP_VIDEO_POS_COUNT];
    GtkWidget *controls_pos_items[KP_CONTROLS_POS_COUNT];
    GtkWidget *column_items[KP_COLUMN_COUNT];
    bool seek_dragging; /* the user holds the seek bar: don't move it */

    /* Played before anything else, in order (Q). */
    GArray *queue; /* kp_entry_ref */
    unsigned columns; /* KP_COLUMN_* bits */
    kp_tagreader *tags;

    kp_video_pos video_pos;
    kp_controls_pos controls_pos;
    bool video_forced; /* View > Video area: always shown */
    bool show_video;   /* what layout.c shows: forced, or a video plays */
    bool fullscreen;
    bool compact;      /* only the video and the controls */
    bool playlists_shown; /* the tabs are in the layout (View > Playlists) */

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
/* The chat panel appeared or went away (kp_chat_wanted changed). */
void kp_layout_update_chat(kp_app *app);
/* Only the video (and nothing else) on the whole screen, or back. */
void kp_layout_set_fullscreen(kp_app *app, bool on);
/* Shows or hides the playlists (the tabs); with no video the window
 * shrinks to the rest. Not for fullscreen and compact mode, which hide
 * them anyway. */
void kp_layout_set_playlists(kp_app *app, bool on);
/* Only the video and the controls (no menus, playlists, status), or back;
 * without a video the window shrinks to the controls. */
void kp_layout_set_compact(kp_app *app, bool on);

/* fsbar.c: the controls floating over the fullscreen video, shown while
 * the mouse moves. init once, with the window; attach moves app->controls
 * into the floating bar (and shows it), detach takes it out. */
void kp_fsbar_init(kp_app *app);
void kp_fsbar_attach(kp_app *app);
void kp_fsbar_detach(kp_app *app);
/* Windowed or not, the mouse cursor hides at rest over a playing video;
 * call when whether one plays may have changed. */
void kp_fsbar_video_changed(kp_app *app);

/* playlists.c */
void kp_playlists_init(kp_app *app);
void kp_playlists_free(kp_app *app);
void kp_playlists_new(kp_app *app);
void kp_playlists_properties(kp_app *app);
void kp_playlists_close(kp_app *app);
void kp_playlists_add_files(kp_app *app);
void kp_playlists_add_folder(kp_app *app);
/* A .m3u/.m3u8/.pls file as a new tab; the visible tab to one. */
void kp_playlists_import(kp_app *app);
void kp_playlists_export(kp_app *app);
/* Q: the selected rows into the queue, or out if all are already in. */
void kp_playlists_toggle_queue(kp_app *app);
/* The selected rows' files to the clipboard (cut: and out of the playlist);
 * paste puts the clipboard's files (ours, or a file manager's) after the
 * selection. */
void kp_playlists_copy(kp_app *app);
void kp_playlists_cut(kp_app *app);
void kp_playlists_paste(kp_app *app);
void kp_playlists_play_selected(kp_app *app);
/* The first selected file's folder, in the file manager. */
void kp_playlists_open_folder(kp_app *app);
/* Shows these columns (KP_COLUMN_* bits) in every tab. */
void kp_playlists_set_columns(kp_app *app, unsigned columns);
/* The visible playlist and its id (NULL if none). */
kp_playlist *kp_playlists_visible(kp_app *app, int *id);
/* The visible playlist gets the keyboard focus. */
void kp_playlists_focus(kp_app *app);
/* Shows the tab with that id and selects the row, scrolled into view. */
void kp_playlists_select(kp_app *app, int id, long index);
/* The queue changed: redraw the numbers. */
void kp_playlists_queue_changed(kp_app *app);
void kp_playlists_remove_selected(kp_app *app);
/* Saves changed playlists and the tab order. */
void kp_playlists_save(kp_app *app);
/* Updates the status line for the visible playlist. */
void kp_playlists_update_status(kp_app *app);
/* The visible playlist's id and its first selected row (-1 if none). */
int kp_playlists_selection(kp_app *app, long *index);
/* Bold for the playing row (index -1 = none), in the tab with that id. */
void kp_playlists_mark_playing(kp_app *app, int id, long index);
/* Scrolls that tab so the playing row is in the middle (a new file began). */
void kp_playlists_scroll_to_playing(kp_app *app, int id, long index);
/* Redraws one row after its entry changed (title, length). */
void kp_playlists_refresh_row(kp_app *app, int id, long index);
/* The kp_playlist with that id, or NULL. */
kp_playlist *kp_playlists_get(kp_app *app, int id);
/* Files from the command line or another kiplay: added to the visible
 * playlist (ones already there are found, not repeated; folders are
 * walked, in the background) and, with play, the first one played. */
void kp_playlists_open(kp_app *app, char **locations, bool play, bool new_playlist);
/* Something changed in a playlist: write it soon. */
void kp_playlists_changed(kp_app *app);
/* Alt+Up/Down: the selected rows one row up (delta < 0) or down. */
void kp_playlists_move_selected(kp_app *app, int delta);

/* controls.c: the transport bar. */
GtkWidget *kp_controls_new(kp_app *app);
/* Time label and seek bar from app->playing. */
void kp_controls_update(kp_app *app);
/* The volume button moves by delta (percent); the player follows. */
void kp_controls_change_volume(kp_app *app, int delta);
/* Sets the volume button (0..100); the player follows. */
void kp_controls_set_volume(kp_app *app, int volume);

/* player.c: plays the playlists through the backend. */
/* kiplay-audio next to the kiplay binary (a build tree), else in PATH. */
char *kp_engine_path(void);
void kp_player_init(kp_app *app);
void kp_player_free(kp_app *app);
/* Plays entry index of the playlist with that id. */
void kp_player_play_at(kp_app *app, int id, long index);
/* Plays a playlist from the track it was last at (the first if none). */
void kp_player_play_playlist(kp_app *app, int id);
/* The Play button: resume, or start the selection / where it stopped. */
void kp_player_play(kp_app *app);
void kp_player_pause(kp_app *app);
void kp_player_toggle_pause(kp_app *app);
void kp_player_stop(kp_app *app);
void kp_player_next(kp_app *app);
void kp_player_prev(kp_app *app);
void kp_player_seek(kp_app *app, double seconds);
void kp_player_seek_relative(kp_app *app, double seconds);
/* One frame on (or back), paused; only for a playing video. */
void kp_player_frame_forward(kp_app *app);
void kp_player_frame_back(kp_app *app);
void kp_player_set_volume(kp_app *app, int volume);
/* Adds the files (KP_OPEN_* flags: to a new playlist? without playing?) and,
 * unless enqueued, plays the first; raises the window. */
void kp_player_open(kp_app *app, char **locations, unsigned flags);
/* The playlist or repeat/shuffle changed: hand the engine the right next
 * file for gapless. */
void kp_player_requeue(kp_app *app);
/* A playlist was closed: stop if it was playing. */
void kp_player_playlist_closed(kp_app *app, int id);
/* Entries of playlist id moved or went away: the playing index follows
 * its entry (or "next" goes on from where it was). */
void kp_player_playlist_changed(kp_app *app, int id);
/* Q on an entry: into the queue, or out. */
void kp_player_queue_toggle(kp_app *app, int id, long index);
/* Its place in the queue (1...), 0 if not queued. */
int kp_player_queue_pos(kp_app *app, int id, const kp_entry *e);
void kp_player_set_stop_after(kp_app *app, bool on);
/* The playing entry's tags arrived: the window title follows. */
void kp_player_info_changed(kp_app *app);
void kp_player_toggle_mute(kp_app *app);
/* A short message in the status line, and over the video when it shows. */
void kp_player_message(kp_app *app, const char *text);
/* The preferences changed: engines whose options changed start again
 * (the file playing goes on where it was). */
void kp_player_prefs_changed(kp_app *app);

/* tracks.c: the Video, Audio and Subtitles menus. */
void kp_tracks_menus(kp_app *app, GtkWidget *bar);
void kp_tracks_free(void);
/* The engine's track list (n 0: none). */
void kp_tracks_update(kp_app *app, const kp_track *tracks, int n);
/* Appends the Audio track and Subtitles submenus (a snapshot of the
 * playing file's tracks) to a context menu. */
void kp_tracks_context_items(kp_app *app, GtkWidget *menu);
/* The engine or its state changed: the menus follow (what is greyed). */
void kp_tracks_refresh(kp_app *app);
/* Another file (or none): no tracks yet; the aspect from app->playing.file. */
void kp_tracks_file_changed(kp_app *app);
void kp_tracks_mute_changed(kp_app *app);
void kp_tracks_cycle(kp_app *app, kp_track_type type);
void kp_tracks_toggle_subs(kp_app *app);
/* delta seconds later (0: no delay). */
void kp_tracks_delay(kp_app *app, kp_track_type type, double delta);
void kp_tracks_cycle_aspect(kp_app *app);

/* subdl.c: subtitles of a YouTube video (named "Title [ID].ext") fetched
 * with yt-dlp, which is set in Preferences > Advanced. */
void kp_subdl_start(kp_app *app);
/* The playing file is a local video with an ID in its name. */
bool kp_subdl_possible(kp_app *app);

/* download.c: File > Download...: links to yt-dlp (set in Preferences > Advanced),
 * with options and a progress bar, in a modal window. */
void kp_download_dialog(kp_app *app);

/* shortcuts.c: the keyboard commands (Preferences > Shortcuts). */
void kp_shortcuts_load(kp_app *app);
/* Runs the command bound to that key; false if none. */
bool kp_shortcuts_key(kp_app *app, const GdkEventKey *ev);
/* The menu item shows the key of that command (and follows changes). */
void kp_shortcuts_attach(GtkWidget *item, const char *action);
GtkWidget *kp_shortcuts_editor_new(kp_app *app);
/* Saves what the editor holds and uses it. */
void kp_shortcuts_editor_apply(kp_app *app, GtkWidget *editor);

/* imgcache.c: pictures of the chat (emoji, avatars) by URL, downloaded in
 * the background and kept on disk. */
typedef void (*kp_img_cb)(void *ud);
/* changed runs (main loop) whenever a picture arrived or failed. */
void kp_img_init(kp_img_cb changed, void *ud);
/* The picture, or NULL while it loads (or if it can't): ask again after
 * the change callback. Owned by the cache; valid until the next call. */
GdkPixbuf *kp_img_get(const char *url);
void kp_img_reset(void);
/* The cache folder's size in bytes; clearing it returns what it freed. The
 * panel downloads again what it still shows. */
gint64 kp_img_cache_size(void);
gint64 kp_img_cache_clear(void);

/* chatpanel.c: the recorded live chat beside the video. */
void kp_chat_init(kp_app *app);
/* The video that starts (NULL: nothing, or music): looks for its chat file. */
void kp_chat_file_changed(kp_app *app, const char *path);
void kp_chat_set_time(kp_app *app, double seconds);
void kp_chat_prefs_changed(kp_app *app);
void kp_chat_toggle(kp_app *app);
/* A chat is loaded and shown: layout.c gives it room beside the video. */
bool kp_chat_wanted(const kp_app *app);
/* In fullscreen the chat is drawn by mpv over its own window (and the
 * panel leaves the layout); call after fullscreen, the chat or the
 * engine changed. */
void kp_chat_overlay_update(kp_app *app);

/* prefsdlg.c: Options > Preferences. */
void kp_prefs_dialog(kp_app *app);

/* desktop.c: MPRIS and the tray icon. */
void kp_desktop_init(kp_app *app);
void kp_desktop_free(kp_app *app);
/* What plays, its state or the volume changed. */
void kp_desktop_update(kp_app *app);
/* The position jumped to seconds. */
void kp_desktop_seeked(kp_app *app, double seconds);
/* [general] mpris / tray changed. */
void kp_desktop_prefs_changed(kp_app *app);
/* The window's close button: true if it went to the tray instead. */
bool kp_desktop_close_to_tray(kp_app *app);
/* Shows the window if hidden in the tray (and raises it). */
void kp_desktop_present(kp_app *app);

/* window.c */
/* Saves everything and closes the window (Quit). */
void kp_window_quit(kp_app *app);

/* searchpanel.c: the search panel beside the playlists (Ctrl+F). It
 * exists only while open. */
void kp_search_panel_toggle(kp_app *app);
/* A playlist changed: the open panel searches again (no-op if closed). */
void kp_search_panel_changed(kp_app *app);
/* The window is going: drops the panel without touching the layout. */
void kp_search_panel_close(kp_app *app);

/* jump.c: Ctrl+J, find a file in the visible playlist and play or queue it. */
void kp_jump_dialog(kp_app *app);

#endif
