/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_BACKEND_H
#define KP_BACKEND_H

#include <stdbool.h>

/* The external player that decodes and draws: one process kept alive
 * between files, behind the same calls whatever it is: mpv --idle (audio
 * and video) or kiplay-audio (Kiplay's own small music engine). Everything runs on the GLib main loop and never blocks: the
 * process is spawned asynchronously, commands are queued until it listens
 * and written without blocking, and what it says comes back as events.
 *
 * If the process dies, KP_EV_CRASHED is sent and the backend is stopped;
 * kp_backend_start brings it back (the caller then reloads the file).
 *
 * Memory: kp_backend owns its process, socket and queues; event texts are
 * only valid during the callback. */

typedef enum {
    KP_EV_LOADED,   /* a file started; has_video and duration may follow */
    KP_EV_DURATION, /* value = seconds (<= 0 unknown, e.g. a stream) */
    KP_EV_TIME,     /* value = seconds played */
    KP_EV_PAUSED,   /* flag = paused */
    KP_EV_VIDEO,    /* flag = the file has a real video track (not cover art) */
    KP_EV_TITLE,    /* text = what to call it now (tags, stream title, file name) */
    KP_EV_TAGS,     /* text = "Artist - Title" from the file's tags (NULL: none) */
    KP_EV_ENDED,    /* the file ended by itself (flag = with an error) */
    KP_EV_NEXT,     /* gapless: the queued file took over (LOADED etc. follow) */
    KP_EV_IDLE,     /* nothing loaded */
    KP_EV_CRASHED,  /* the process went away; text = why */
    KP_EV_TRACKS,   /* tracks/n_tracks = the file's tracks (KP_CAP_TRACKS) */
} kp_event_type;

typedef enum {
    KP_TRACK_VIDEO,
    KP_TRACK_AUDIO,
    KP_TRACK_SUB,
} kp_track_type;

/* One track of the playing file, as the backend numbers it (ids start at 1
 * per type; 0 means "none" when choosing). */
typedef struct {
    kp_track_type type;
    int id;
    const char *title, *lang, *codec; /* NULL if unknown */
    const char *file;                 /* external track: its file; else NULL */
    bool selected, external, albumart;
} kp_track;

typedef struct {
    kp_event_type type;
    double value;
    bool flag;
    const char *text;
    const kp_track *tracks;
    int n_tracks;
} kp_event;

typedef void (*kp_event_cb)(void *ud, const kp_event *ev);

/* What a backend can do beyond the basics. */
enum {
    KP_CAP_VIDEO = 1 << 0,   /* draws video into opts.wid */
    KP_CAP_GAPLESS = 1 << 1, /* kp_backend_queue: the next file starts without a gap */
    KP_CAP_TRACKS = 1 << 2,  /* KP_EV_TRACKS, tracks, delays and aspect can be chosen */
};

typedef struct {
    /* Program to run ("mpv", "kiplay-audio", or a full path). */
    const char *program;
    /* X window to draw into (--wid); 0 = none (tests, audio only). */
    unsigned long wid;
    /* Read the user's own mpv.conf too (SMPlayer's "use config" option). */
    bool use_config;
    /* Initial volume, 0..100. */
    int volume;
    /* kiplay-audio: ALSA device ("default", "hw:1"...); NULL = default. */
    const char *device;
    /* Options from the preferences (kp_mpv_pref_args), may be NULL. */
    const char *const *args;
    /* Extra arguments as typed by the user (shell quoting), may be NULL. */
    const char *extra;
    /* Arguments for tests (e.g. {"--ao=null", "--vo=null", NULL}), may be NULL. */
    const char *const *test_args;
} kp_backend_opts;

typedef struct kp_backend kp_backend;

/* Nothing starts until kp_backend_start. opts is copied. */
kp_backend *kp_backend_new_mpv(const kp_backend_opts *opts, kp_event_cb cb, void *ud);
kp_backend *kp_backend_new_audio(const kp_backend_opts *opts, kp_event_cb cb, void *ud);
const char *kp_backend_name(const kp_backend *b);
unsigned kp_backend_caps(const kp_backend *b);
/* Stops the process (asks it to quit, kills it if it lingers). */
void kp_backend_free(kp_backend *b);

/* Spawns the process; false with *err (g_free it) if it can't run. */
bool kp_backend_start(kp_backend *b, char **err);
bool kp_backend_running(const kp_backend *b);
/* Process id, 0 when not running (logs and tests). */
int kp_backend_pid(const kp_backend *b);

/* A subtitle file that goes with the video: title and lang (a code such as
 * "ko" or "es-419", which the language preference matches) may be NULL. */
typedef struct {
    const char *path, *title, *lang;
} kp_sub_file;

/* How a file starts: what Kiplay remembers of it (kp_file_settings).
 * Backends without KP_CAP_TRACKS only look at start. */
typedef struct {
    double start;                   /* seconds; 0 = beginning */
    int aid, sid;                   /* track ids; -1 = the backend's choice, sid 0 = none */
    double audio_delay, sub_delay;  /* seconds */
    const char *aspect;             /* "16:9", "1.85"...; NULL or "" = the file's */
    /* Subtitle files to load with it. When there are any, the backend's own
     * search for subtitles next to the file is off: the list is everything. */
    const kp_sub_file *sub_files;
    int n_sub_files;
} kp_load_opts;

#define KP_LOAD_OPTS_INIT {.start = 0, .aid = -1, .sid = -1}

/* Plays path (a file or URL); lo NULL = from the beginning, as it comes. */
void kp_backend_load(kp_backend *b, const char *path, const kp_load_opts *lo);
/* What to play when the current file ends, without a gap (KP_CAP_GAPLESS;
 * NULL clears it). KP_EV_NEXT tells when it took over. */
void kp_backend_queue(kp_backend *b, const char *path);
void kp_backend_stop(kp_backend *b);
void kp_backend_set_pause(kp_backend *b, bool paused);
void kp_backend_seek(kp_backend *b, double seconds);          /* absolute */
void kp_backend_seek_relative(kp_backend *b, double seconds);
void kp_backend_set_volume(kp_backend *b, int volume);        /* 0..100 */
/* One video frame forward or back, leaving it paused (KP_CAP_VIDEO; no-op
 * otherwise). */
void kp_backend_step_frame(kp_backend *b, bool back);
/* Asks for the time 4x a second while true (only while playing and shown). */
void kp_backend_set_ticking(kp_backend *b, bool on);
/* KP_CAP_TRACKS (no-ops otherwise). id 0 = none (subtitles). */
void kp_backend_set_track(kp_backend *b, kp_track_type type, int id);
/* Loads a subtitle file into the playing file (KP_CAP_TRACKS); select: and
 * shows it. */
void kp_backend_add_sub(kp_backend *b, const kp_sub_file *file, bool select);
/* Audio or subtitle delay, seconds (positive: later). */
void kp_backend_set_delay(kp_backend *b, kp_track_type type, double seconds);
/* "16:9", "4:3"...; NULL or "" = the file's own. */
void kp_backend_set_aspect(kp_backend *b, const char *aspect);
/* A short message drawn over the video (fullscreen feedback), if it can. */
void kp_backend_show_text(kp_backend *b, const char *text);

/* A BGRA picture (premultiplied, Cairo's ARGB32 on little-endian) drawn
 * over the video at x, y in window pixels, read from the file at path,
 * which must stay as it is until replaced: hand a different file for the
 * next picture. path NULL removes it. One overlay per backend. */
void kp_backend_overlay(kp_backend *b, const char *path, int x, int y, int w, int h, int stride);
/* Keeps this fraction of the window's width at the right free of video
 * (0 = none), for something drawn there. */
void kp_backend_set_margin_right(kp_backend *b, double ratio);

/* For tests: one line of mpv's JSON IPC to events, as the backend would
 * deliver them. */
void kp_mpv_parse_line(const char *line, kp_event_cb cb, void *ud);
/* For tests: loadfile's per-file options, each after a comma
 * (",start=12.000,sid=no"); "" for none. g_free it. */
char *kp_mpv_file_options(const kp_load_opts *lo);

#endif
