/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_YTDL_H
#define KP_YTDL_H

#include <gio/gio.h>
#include <stdbool.h>

/* The "Download" window's engine: what the person chose becomes a yt-dlp
 * command line (kp_ytdl_argv), the program runs without blocking with its
 * output read line by line (kp_ytdl_start), and the lines become a
 * progress (kp_ytdl_progress_feed). The program itself comes from
 * kp_ytdlp_program (ytdlp.h).
 *
 * Memory: kp_ytdl_opts only points at the caller's strings; the argv and
 * the progress own theirs. */

typedef struct {
    char *const *urls;        /* NULL-terminated; at least one */
    const char *dest;         /* -P: where the files go; empty: yt-dlp's cwd */
    const char *tmpl;         /* -o: output template; empty: yt-dlp's own */
    bool audio_only;          /* -x */
    const char *audio_format; /* --audio-format: "m4a", "mp3"...; empty: keep */
    int max_height;           /* video: best up to this height; 0: best */
    bool remux_mp4;           /* video: --remux-video mp4 */
    bool subs;                /* --write-subs, in srv3 when there is one */
    const char *sub_langs;    /* --sub-langs: "pt,en", "all"; empty: all */
    bool auto_subs;           /* --write-auto-subs */
    bool thumbnail;
    bool restrict_names;
    bool no_playlist;
    const char *items;        /* -I: "1:3", "1,2,5"; empty: all */
    bool skip_download;
    const char *extra;        /* more arguments, shell-quoted; may be NULL */
} kp_ytdl_opts;

/* Whitespace-separated links, one or many per line; lines starting with
 * '#' are comments. g_strfreev it. */
char **kp_ytdl_split_urls(const char *text);

/* The yt-dlp argv (g_free'd strings, NULL-terminated): g_strfreev it.
 * NULL with *err (g_free it) if there is nothing to download or an option
 * is not valid (the playlist items, the extra arguments). */
char **kp_ytdl_argv(const char *program, const kp_ytdl_opts *o, char **err);

/* The argv as a shell line, to show and to copy. g_free it. */
char *kp_ytdl_command_line(char *const *argv);

/* ---- progress ------------------------------------------------------------ */

typedef struct {
    double percent;  /* of the file being fetched, 0..100 */
    int item, items; /* "item 3 of 10" in a playlist; 0: unknown */
    char *file;      /* name of the file being written, or NULL */
    char *detail;    /* "of 5.00MiB at 1.20MiB/s ETA 00:03", or NULL */
} kp_ytdl_progress;

void kp_ytdl_progress_clear(kp_ytdl_progress *p);
/* Reads one line of output; true if something to show changed. */
bool kp_ytdl_progress_feed(kp_ytdl_progress *p, const char *line);
/* Of everything: the finished items plus this file's share, 0..1. */
double kp_ytdl_progress_fraction(const kp_ytdl_progress *p);

/* ---- running ------------------------------------------------------------- */

typedef struct kp_ytdl kp_ytdl;

/* One line of the program's output (stdout and stderr together). */
typedef void (*kp_ytdl_line_cb)(void *ud, const char *line);
/* The end: cancelled, or err (the program's last complaint) on failure.
 * Called once, on the main loop; the kp_ytdl is gone after it. */
typedef void (*kp_ytdl_done_cb)(void *ud, bool cancelled, const char *err);

/* NULL with *err (g_free it) if the program cannot start. */
kp_ytdl *kp_ytdl_start(char *const *argv, kp_ytdl_line_cb line, kp_ytdl_done_cb done, void *ud,
                       char **err);
/* Asks the program to stop; done follows with cancelled = true. */
void kp_ytdl_cancel(kp_ytdl *y);
/* Stops it and forgets the callbacks (the owner is going away). */
void kp_ytdl_abort(kp_ytdl *y);

#endif
