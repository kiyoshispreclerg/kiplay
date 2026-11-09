/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_SPLITSPEC_H
#define KP_SPLITSPEC_H

#include <glib.h>
#include <stdbool.h>

/* "Split into tracks": the text the user types (or that comes from the
 * file's chapters, or from a YouTube description) turned into pieces.
 *
 * One time per line, the way video descriptions mark chapters:
 *
 *   0:00 Intro            [12:34] Title         01. Title - 3:45
 *   00:01:02 - Title      1:02:03.5 Title       Title (4:10)
 *
 * Lines without a time are ignored; a track number before the title
 * ("1.", "01)", "#3") is dropped. Times are where each piece starts, unless
 * they only make sense as lengths (an album's track list): they then add
 * up to the file's length, and are read as such.
 *
 * Whatever comes before the first time is a piece too (untitled).
 *
 * Memory: the pieces and their text belong to the GArray (made with a
 * clear function: g_array_unref frees everything). */

typedef struct {
    double start, end; /* seconds */
    char *text;        /* the line without its time, trimmed; "" for none */
    int line;          /* 1-based line in the text; 0 for the leading piece */
} kp_split_piece;

typedef enum {
    KP_SPLIT_OK,
    KP_SPLIT_EMPTY,     /* no time in the text */
    KP_SPLIT_UNORDERED, /* a time not after the one before */
    KP_SPLIT_PAST_END,  /* a time at or after the end of the file */
} kp_split_status;

typedef struct {
    kp_split_status status;
    int line;          /* where the problem is (KP_SPLIT_UNORDERED, _PAST_END) */
    bool as_durations; /* the times were read as lengths */
} kp_split_result;

/* Pieces covering 0..duration (seconds, > 0), never NULL: empty when
 * res->status isn't KP_SPLIT_OK. */
GArray *kp_split_parse(const char *text, double duration, kp_split_result *res);

/* "Artist - Title" (also with an en or em dash): true and both halves
 * (g_free them), trimmed; false when the text isn't like that. */
bool kp_split_artist_title(const char *text, char **artist, char **title);

/* A file name for piece n (1-based) of total: "03 - Title.ext", or
 * "03.ext" with no title. Slashes and control characters out, no leading
 * dot, at most ~200 bytes. ext without the dot. g_free it. */
char *kp_split_file_name(int n, int total, const char *title, const char *ext);

/* "1:23", "1:02:03", and ".5"-style decimals when not whole. */
char *kp_split_format_time(double seconds);

/* What the dialog starts with, for a local file: its chapters, given as
 * starts and titles (from the engine; n may be 0), else those of the
 * yt-dlp `.info.json` beside it, else its `.description`. NULL when none
 * has a time in it. g_free it. */
char *kp_split_text_for(const char *path, const double *starts, char *const *titles, int n);

#endif
