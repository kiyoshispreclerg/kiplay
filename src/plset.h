/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_PLSET_H
#define KP_PLSET_H

#include <stddef.h>

#include "playlist.h"

/* All the playlists (the tabs), each in <dir>/<id>.m3u8. Ids are small
 * numbers that never change while the playlist lives, so renaming or
 * moving a tab doesn't rename files; the tab order is kept apart, as a
 * list of ids ("3,1,2") the caller stores in the prefs.
 *
 * Memory: the set owns its playlists and its dir string. */

typedef struct kp_plset kp_plset;

/* Reads every <id>.m3u8 in dir (a missing dir is an empty set). order puts
 * those ids first, in that order; the rest follow by id. Unknown ids in
 * order are ignored. */
kp_plset *kp_plset_load(const char *dir, const char *order);
void kp_plset_free(kp_plset *s);

size_t kp_plset_count(const kp_plset *s);
kp_playlist *kp_plset_get(const kp_plset *s, size_t i);
int kp_plset_id(const kp_plset *s, size_t i);
/* Position of the playlist with that id, or -1. */
long kp_plset_find(const kp_plset *s, int id);

/* New empty playlist at position at (clamped to the end); returns its
 * position. Saved on the next kp_plset_save. */
size_t kp_plset_insert(kp_plset *s, size_t at, const char *name);
/* Removes it from the set and deletes its file. */
void kp_plset_remove(kp_plset *s, size_t i);
void kp_plset_move(kp_plset *s, size_t from, size_t to);

/* "3,1,2"; caller g_free()s. */
char *kp_plset_order(const kp_plset *s);

/* Writes the playlists that changed. 0, or -1 if any failed (errno set). */
int kp_plset_save(kp_plset *s);

#endif
