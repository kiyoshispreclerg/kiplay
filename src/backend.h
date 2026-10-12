/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_BACKEND_H
#define KP_BACKEND_H

#include <stdbool.h>

/* The external player that decodes and draws: one process kept alive
 * between files (mpv --idle now; MPlayer and kiplay-audio later behind the
 * same calls). Everything runs on the GLib main loop and never blocks: the
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
    KP_EV_IDLE,     /* nothing loaded */
    KP_EV_CRASHED,  /* the process went away; text = why */
} kp_event_type;

typedef struct {
    kp_event_type type;
    double value;
    bool flag;
    const char *text;
} kp_event;

typedef void (*kp_event_cb)(void *ud, const kp_event *ev);

typedef struct {
    /* Program to run ("mpv", or a full path). */
    const char *program;
    /* X window to draw into (--wid); 0 = none (tests, audio only). */
    unsigned long wid;
    /* Read the user's own mpv.conf too (SMPlayer's "use config" option). */
    bool use_config;
    /* Initial volume, 0..100. */
    int volume;
    /* Extra arguments as typed by the user (shell quoting), may be NULL. */
    const char *extra;
    /* Arguments for tests (e.g. {"--ao=null", "--vo=null", NULL}), may be NULL. */
    const char *const *test_args;
} kp_backend_opts;

typedef struct kp_backend kp_backend;

/* Nothing starts until kp_backend_start. opts is copied. */
kp_backend *kp_backend_new_mpv(const kp_backend_opts *opts, kp_event_cb cb, void *ud);
/* Stops the process (asks it to quit, kills it if it lingers). */
void kp_backend_free(kp_backend *b);

/* Spawns the process; false with *err (g_free it) if it can't run. */
bool kp_backend_start(kp_backend *b, char **err);
bool kp_backend_running(const kp_backend *b);
/* Process id, 0 when not running (logs and tests). */
int kp_backend_pid(const kp_backend *b);

/* Plays path (a file or URL) from start seconds (0 = beginning). */
void kp_backend_load(kp_backend *b, const char *path, double start);
void kp_backend_stop(kp_backend *b);
void kp_backend_set_pause(kp_backend *b, bool paused);
void kp_backend_seek(kp_backend *b, double seconds);          /* absolute */
void kp_backend_seek_relative(kp_backend *b, double seconds);
void kp_backend_set_volume(kp_backend *b, int volume);        /* 0..100 */
/* Asks for the time 4x a second while true (only while playing and shown). */
void kp_backend_set_ticking(kp_backend *b, bool on);

/* For tests: one line of mpv's JSON IPC to events, as the backend would
 * deliver them. */
void kp_mpv_parse_line(const char *line, kp_event_cb cb, void *ud);

#endif
