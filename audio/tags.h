/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KA_TAGS_H
#define KA_TAGS_H

#include <stdbool.h>

/* Tags and length of a file without decoding it: what the playlist shows.
 * Same locations as dec.h ("?N" subtunes, file:// URIs). Game music gives
 * the song as the title, the game as the album and the composer as the
 * artist.
 *
 * Memory: the strings are malloc()ed and belong to the ka_tags. */

typedef struct {
    char *title, *artist, *album; /* NULL if the file doesn't say */
    double duration;              /* seconds, <= 0 unknown */
} ka_tags;

/* false with a message in err. */
bool ka_tags_read(const char *location, ka_tags *t, char *err, int errsize);
void ka_tags_clear(ka_tags *t);

/* "tags-mode": one location per line on stdin, one line per location on
 * stdout, in order:
 *
 *   ok <seconds>\t<title>\t<artist>\t<album>    (empty fields: unknown)
 *   error <message>
 *
 * Returns the exit status. */
int ka_tags_serve(void);

#endif
