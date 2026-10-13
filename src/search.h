/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_SEARCH_H
#define KP_SEARCH_H

#include <stdbool.h>

#include "playlist.h"

/* "Jump to file" matching: every word of the query, in any order and any
 * case, somewhere in the title, artist, album or file path ("beat 08"
 * finds "Beatles/08 - Help.mp3").
 *
 * Memory: both functions return new strings (g_strfreev / g_free). */

/* The query's words, case-folded; an empty query has none. */
char **kp_search_words(const char *query);
/* What an entry is searched in, case-folded, made once per entry. */
char *kp_search_haystack(const kp_entry *e);
bool kp_search_match(const char *haystack, char *const *words);

#endif
