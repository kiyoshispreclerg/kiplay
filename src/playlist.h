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
 *   #EXTART:Artist
 *   #EXTALB:Album
 *   /home/me/Music/song.mp3
 *   file:///home/me/Music/game.nsf?3
 *
 * A location is an absolute path, or a URI when it needs one: a subtune of
 * a multi-song file (Audacious's "?N"), or a stream (http...). Relative
 * paths in a file are resolved against the file's folder. #EXTINF keeps
 * "Artist - Title" (what other players show); #EXTART/#EXTALB, when there,
 * split it back. PLS (File1=, Title1=, Length1=) is read and written too,
 * for import and export.
 *
 * Memory: the playlist owns its entries; every string lives in one
 * GStringChunk (no malloc per entry, which matters at tens of thousands of
 * entries). Removing entries leaves their text in the chunk until the next
 * load. */

typedef struct {
    const char *location;
    const char *title;  /* the song's title; NULL until known */
    const char *artist; /* NULL if unknown (title may then be "Artist - Title") */
    const char *album;
    int64_t length_ms; /* -1 until known */
    bool scanned;      /* the tags were looked for this run (not saved) */
} kp_entry;

/* What is known about a file, for adding and updating entries. NULL
 * strings and length -1 are "unknown". */
typedef struct {
    const char *title, *artist, *album;
    int64_t length_ms;
} kp_tags;

#define KP_TAGS_NONE ((kp_tags){NULL, NULL, NULL, -1})

typedef struct kp_playlist kp_playlist;

kp_playlist *kp_playlist_new(const char *name);
void kp_playlist_free(kp_playlist *pl);

const char *kp_playlist_name(const kp_playlist *pl);
void kp_playlist_set_name(kp_playlist *pl, const char *name);

size_t kp_playlist_count(const kp_playlist *pl);
/* NULL past the end. Valid until the playlist changes. */
const kp_entry *kp_playlist_get(const kp_playlist *pl, size_t i);

/* Appends; tags may be NULL. */
void kp_playlist_add(kp_playlist *pl, const char *location, const kp_tags *tags);
/* Inserts before entry at (clamped to the end). */
void kp_playlist_insert(kp_playlist *pl, size_t at, const char *location, const kp_tags *tags);
void kp_playlist_remove(kp_playlist *pl, size_t i);
/* Removes the n entries at rows (ascending, distinct) in one pass. */
void kp_playlist_remove_rows(kp_playlist *pl, const size_t *rows, size_t n);
/* Index of the entry whose location is that very string (the pointer the
 * entry holds, so it names one entry even with duplicates), or -1. Used
 * to follow an entry while the playlist changes around it. */
long kp_playlist_find_entry(const kp_playlist *pl, const char *location, long hint);
/* Moves the n entries at rows (ascending, distinct) so that they end up
 * together, in their order, where entry "to" was before the move (to = count:
 * at the end). Returns the new index of the first one. new_order, if not
 * NULL, gets count ints: new_order[new index] = old index (what GTK's
 * rows-reordered wants). */
size_t kp_playlist_move(kp_playlist *pl, const size_t *rows, size_t n, size_t to,
                        int *new_order);
/* What was found out about entry i. With a title, the title, artist and
 * album all replace the old ones (a NULL artist clears it); without one
 * they are kept. A length < 0 keeps the old one. Marks the playlist dirty
 * only if something changed, and returns whether it did. */
bool kp_playlist_set_tags(kp_playlist *pl, size_t i, const kp_tags *tags);
/* Flags entry i as looked at by the tag reader (in memory only). */
void kp_playlist_set_scanned(kp_playlist *pl, size_t i);
void kp_playlist_clear(kp_playlist *pl);

/* Sum of the known lengths, and how many are unknown. */
int64_t kp_playlist_total_ms(const kp_playlist *pl, size_t *unknown);

/* Parses M3U/M3U8 text. dir resolves relative paths (NULL: keep them).
 * Without a #PLAYLIST line the name is default_name. Never fails: lines it
 * doesn't understand are skipped. */
kp_playlist *kp_playlist_parse_m3u(const char *text, const char *dir,
                                   const char *default_name);
/* PLS ("[playlist]", FileN=, TitleN=, LengthN=), same rules. */
kp_playlist *kp_playlist_parse_pls(const char *text, const char *dir,
                                   const char *default_name);
/* M3U or PLS, by content. Text that isn't UTF-8 (old .m3u files) is read
 * as Windows-1252. NULL if the file can't be read (errno set). */
kp_playlist *kp_playlist_load(const char *path, const char *default_name);

/* The M3U8 / PLS text; caller g_free()s. */
char *kp_playlist_dump(const kp_playlist *pl);
char *kp_playlist_dump_pls(const kp_playlist *pl);
/* Atomic (temp file + rename), creating the folder; PLS if the name ends
 * in .pls, M3U8 otherwise. 0 or -1 (errno set). */
int kp_playlist_save(const kp_playlist *pl, const char *path);

/* Has it changed since load/save (kp_playlist_mark_saved clears it)? */
bool kp_playlist_dirty(const kp_playlist *pl);
void kp_playlist_mark_saved(kp_playlist *pl);

#endif
