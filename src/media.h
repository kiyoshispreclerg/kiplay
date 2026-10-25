/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_MEDIA_H
#define KP_MEDIA_H

#include <stdbool.h>

/* What kind of file a location is, by its extension (no file is opened):
 * enough to pick the engine and to keep pictures and text out when a
 * folder is added. Locations may be paths, file:// URIs or "game.nsf?3". */

typedef enum {
    KP_MEDIA_NONE,  /* not something Kiplay plays (cover.jpg, notes.txt...) */
    KP_MEDIA_AUDIO, /* music: kiplay-audio's (game music too) */
    KP_MEDIA_VIDEO, /* or anything else mpv opens */
} kp_media_kind;

kp_media_kind kp_media_kind_of(const char *location);
/* How a playlist keeps a location it is given (a path or a URI): local
 * files as plain paths, but a subtune ("game.nsf?3", from a file:// URI or
 * a path, where "game.nsf" exists and "game.nsf?3" doesn't) as a file://
 * URI with the "?N", as Audacious writes it; anything else unchanged.
 * Returns a new string. */
char *kp_media_location(const char *location);
/* A video file whose sound kiplay-audio can play alone (it opens only the
 * audio stream): video, minus the tracker modules only mpv understands. */
bool kp_media_video_has_sound(const char *location);
/* Playlists that can be opened as tabs (m3u, m3u8, pls). */
bool kp_media_is_playlist(const char *location);

#endif
