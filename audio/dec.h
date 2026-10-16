/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KA_DEC_H
#define KA_DEC_H

#include <stdbool.h>
#include <stdint.h>

/* One file decoded to 16-bit stereo, whatever it was: everything
 * libavformat opens, and game music (NSF, SPC, GBS, VGM...) through game.h.
 *
 * A location may end in "?N" (Audacious's notation for song N of a
 * multi-song file, from 1) or be a file:// URI.
 *
 * Memory: ka_dec owns its libav contexts; the strings it returns live as
 * long as it does. */

typedef struct ka_dec ka_dec;

/* Converted to rate frames a second; 0 keeps the file's own rate (44.1 kHz
 * for game music). NULL with a message in err. */
ka_dec *ka_dec_open(const char *location, int rate, char *err, int errsize);
void ka_dec_close(ka_dec *d);

/* Up to max frames into out (interleaved stereo); 0 at the end (a
 * decoding error that can't be skipped ends the file too). */
long ka_dec_read(ka_dec *d, int16_t *out, long max);
/* To that second (from 0); the next read starts there, to the sample. */
bool ka_dec_seek(ka_dec *d, double seconds);

/* What ka_dec_read gives, frames a second. */
int ka_dec_rate(const ka_dec *d);
/* Seconds, <= 0 unknown. */
double ka_dec_duration(const ka_dec *d);
/* "Artist - Title", the title alone, or NULL. */
const char *ka_dec_tags(const ka_dec *d);

/* Splits "path?N" into the path and N (0 when there is none); exposed for
 * the tests. Returns a new string (free it). */
char *ka_dec_split_subtune(const char *location, int *subtune);

#endif
