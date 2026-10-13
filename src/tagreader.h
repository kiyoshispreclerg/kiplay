/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_TAGREADER_H
#define KP_TAGREADER_H

#include <stddef.h>

#include "playlist.h"

/* Reads the tags and length of playlist entries in the background, in
 * another process ("kiplay-audio --tags", libavformat): a file that hangs
 * or crashes the reader costs that file, never the window. The process
 * starts with the first request and goes away a few seconds after the
 * last answer, so it costs nothing while there is nothing to read.
 *
 * Answers come on the main loop, in the order asked, with what the request
 * carried (playlist id and the index the entry had then): the caller finds
 * the entry again, since the playlist may have changed meanwhile.
 *
 * Memory: kp_tagreader copies what it is given; the tags passed to the
 * callback are only valid during it. */

typedef struct kp_tagreader kp_tagreader;

/* tags = NULL: the file couldn't be read (missing, not media, hung). */
typedef void (*kp_tags_cb)(void *ud, int list_id, long index, const char *location,
                           const kp_tags *tags);

/* program: the kiplay-audio to run. */
kp_tagreader *kp_tagreader_new(const char *program, kp_tags_cb cb, void *ud);
/* Stops the process; no more callbacks. */
void kp_tagreader_free(kp_tagreader *tr);

void kp_tagreader_add(kp_tagreader *tr, int list_id, long index, const char *location);
/* Drops the requests of that playlist not yet sent (it was closed). */
void kp_tagreader_forget(kp_tagreader *tr, int list_id);
/* Requests not yet answered. */
size_t kp_tagreader_pending(const kp_tagreader *tr);
/* The reader's pid, 0 when none runs (tests, measurements). */
int kp_tagreader_pid(const kp_tagreader *tr);

#endif
