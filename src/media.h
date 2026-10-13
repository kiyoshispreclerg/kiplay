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
/* Playlists that can be opened as tabs (m3u, m3u8, pls). */
bool kp_media_is_playlist(const char *location);

#endif
