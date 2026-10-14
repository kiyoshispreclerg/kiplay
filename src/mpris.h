/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_MPRIS_H
#define KP_MPRIS_H

#include <stdbool.h>

/* MPRIS 2 (https://specifications.freedesktop.org/mpris-spec/latest/):
 * the desktop, its media keys and applets (the kikarinhas among them)
 * see what plays and control it over the session bus. Kiplay is
 * org.mpris.MediaPlayer2.kiplay, or ...kiplay.instance<pid> when that name
 * is taken (another Kiplay with another settings folder).
 *
 * The caller tells what changes (kp_mpris_set_*); changes are gathered and
 * announced once per main loop turn with PropertiesChanged. Requests from
 * the bus come back through kp_mpris_ops, on the main loop. The position
 * is asked for (ops.position) only when someone reads it, per the spec.
 *
 * Memory: kp_mpris copies what it is given. Nothing blocks: the bus is
 * reached asynchronously, and without one MPRIS is just absent. */

typedef enum {
    KP_MPRIS_STOPPED,
    KP_MPRIS_PLAYING,
    KP_MPRIS_PAUSED,
} kp_mpris_status;

typedef struct {
    const char *title, *artist, *album;
    const char *url;    /* file:// URI or the stream's URL */
    double length;      /* seconds; <= 0 unknown */
} kp_mpris_track;

typedef struct {
    void (*play)(void *ud);
    void (*pause)(void *ud);
    void (*play_pause)(void *ud);
    void (*stop)(void *ud);
    void (*next)(void *ud);
    void (*previous)(void *ud);
    void (*seek)(void *ud, double offset); /* seconds, relative */
    void (*set_position)(void *ud, double seconds);
    void (*set_volume)(void *ud, double volume); /* 0..1 */
    void (*set_repeat)(void *ud, bool on);
    void (*set_shuffle)(void *ud, bool on);
    void (*raise)(void *ud);
    void (*quit)(void *ud);
    void (*open_uri)(void *ud, const char *uri);
    double (*position)(void *ud); /* seconds */
} kp_mpris_ops;

typedef struct kp_mpris kp_mpris;

/* Starts reaching the session bus; ops is copied. */
kp_mpris *kp_mpris_new(const kp_mpris_ops *ops, void *ud);
/* Leaves the bus. */
void kp_mpris_free(kp_mpris *m);
/* The name owned, NULL until then (tests, logs). */
const char *kp_mpris_bus_name(const kp_mpris *m);

void kp_mpris_set_status(kp_mpris *m, kp_mpris_status status);
/* The track playing (NULL: none). new_track: another entry started (it
 * gets a new track id); false: only what is known about it changed. */
void kp_mpris_set_track(kp_mpris *m, const kp_mpris_track *t, bool new_track);
void kp_mpris_set_volume(kp_mpris *m, double volume);
void kp_mpris_set_repeat(kp_mpris *m, bool on);
void kp_mpris_set_shuffle(kp_mpris *m, bool on);
void kp_mpris_set_seekable(kp_mpris *m, bool on);
/* The position jumped (a seek, or a file resumed): the Seeked signal. */
void kp_mpris_seeked(kp_mpris *m, double seconds);

#endif
