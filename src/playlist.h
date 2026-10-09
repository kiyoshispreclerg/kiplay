/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_PLAYLIST_H
#define KP_PLAYLIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One playlist (one tab), kept on disk as an extended M3U in UTF-8:
 *
 *   #EXTM3U
 *   #PLAYLIST:Name of the tab
 *   #EXTINF:198,Artist - Title
 *   /home/me/Music/song.mp3
 *   file:///home/me/Music/game.nsf?3
 *
 * A location is an absolute path, or a URI when it needs one: a subtune of
 * a multi-song file (Audacious's "?N"), or a stream (http...). Relative
 * paths in a file are resolved against the file's folder.
 *
 * Memory: the playlist owns its entries; every string lives in one
 * GStringChunk (no malloc per entry, which matters at tens of thousands of
 * entries). Removing entries leaves their text in the chunk until the next
 * load. */

typedef struct {
    const char *location;
    const char *title; /* NULL until known */
    int64_t length_ms; /* -1 until known */
} kp_entry;

typedef struct kp_playlist kp_playlist;

kp_playlist *kp_playlist_new(const char *name);
void kp_playlist_free(kp_playlist *pl);

const char *kp_playlist_name(const kp_playlist *pl);
void kp_playlist_set_name(kp_playlist *pl, const char *name);

size_t kp_playlist_count(const kp_playlist *pl);
/* NULL past the end. Valid until the playlist changes. */
const kp_entry *kp_playlist_get(const kp_playlist *pl, size_t i);

/* Appends; title may be NULL and length_ms -1 when unknown. */
void kp_playlist_add(kp_playlist *pl, const char *location, const char *title,
                     int64_t length_ms);
void kp_playlist_remove(kp_playlist *pl, size_t i);
void kp_playlist_clear(kp_playlist *pl);

/* Sum of the known lengths, and how many are unknown. */
int64_t kp_playlist_total_ms(const kp_playlist *pl, size_t *unknown);

/* Parses M3U/M3U8 text. dir resolves relative paths (NULL: keep them).
 * Without a #PLAYLIST line the name is default_name. Never fails: lines it
 * doesn't understand are skipped. */
kp_playlist *kp_playlist_parse_m3u(const char *text, const char *dir,
                                   const char *default_name);
/* NULL if the file can't be read (errno set). */
kp_playlist *kp_playlist_load(const char *path, const char *default_name);

/* The M3U8 text; caller g_free()s. */
char *kp_playlist_dump(const kp_playlist *pl);
/* Atomic (temp file + rename), creating the folder. 0 or -1 (errno set). */
int kp_playlist_save(const kp_playlist *pl, const char *path);

/* Has it changed since load/save (kp_playlist_mark_saved clears it)? */
bool kp_playlist_dirty(const kp_playlist *pl);
void kp_playlist_mark_saved(kp_playlist *pl);

#endif
