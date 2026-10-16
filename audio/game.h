/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KA_GAME_H
#define KA_GAME_H

#include <stdbool.h>
#include <stdint.h>

/* Game music (NSF, SPC, GBS, VGM/VGZ, GYM, HES, KSS, AY, SAP) through our
 * own copy of Game_Music_Emu (vendor/gme), not ffmpeg's libgme demuxer:
 * the system libgme emulates the Mega Drive's YM2612 cycle by cycle (Nuked,
 * ~7% of a core; ours uses Gens' at ~0.3%), has no YM2413 (Master System
 * FM) and can't seek.
 *
 * Song N is the file's N-th, as in Audacious's playlists; "game.m3u" next
 * to "game.nsf" (the usual rips) gives them names and lengths, which
 * Audacious doesn't read.
 *
 * Memory: ka_game owns the emulator; the strings it returns live as long
 * as it does. */

typedef struct ka_game ka_game;

/* Game music without a length ends after this (with a fade), as in
 * Audacious; the tag reader reports the same length. */
#define KA_GAME_LENGTH_S 180

/* Whether the path's extension is one of ours (it isn't opened). */
bool ka_game_is(const char *path);

/* Song `subtune` (from 1; 0 is the first) rendered at rate frames a second;
 * rate 0 opens it for the information only (ka_game_read can't be used).
 * NULL with a message in err. */
ka_game *ka_game_open(const char *path, int subtune, int rate, char *err, int errsize);
void ka_game_close(ka_game *g);

/* Up to max stereo frames into out; 0 at the end (length and fade, or a
 * long silence). */
long ka_game_read(ka_game *g, int16_t *out, long max);
/* To that second; the emulator runs (fast, silently) up to it. */
bool ka_game_seek(ka_game *g, double seconds);

/* Seconds, with the fade. */
double ka_game_duration(const ka_game *g);
/* NULL when the file doesn't say. */
const char *ka_game_song(const ka_game *g);
const char *ka_game_name(const ka_game *g);
const char *ka_game_author(const ka_game *g);

#endif
