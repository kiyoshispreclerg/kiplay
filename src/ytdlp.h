/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_YTDLP_H
#define KP_YTDLP_H

#include <gio/gio.h>
#include <stdbool.h>

#include "prefs.h"

/* Fetching a YouTube video's subtitles with yt-dlp, for a local file named
 * `Title [ID].ext` (kp_sidecar_video_id). The program is [tools] ytdlp in
 * kiplay.ini (a path; "~/" works), else yt-dlp in PATH. Both steps run the
 * program without blocking and report on the main loop:
 *
 *   list     -J on the video: is it a valid, available video, and which
 *            subtitle languages does it have;
 *   download the chosen ones, as `Title [ID].<lang>.<ext>` next to the
 *            video, where the sidecar search finds them. YouTube's styled
 *            format (srv3) is asked first.
 *
 * Memory: the callbacks' arguments are only valid during the call. */

typedef struct {
    char *lang; /* the code yt-dlp takes in --sub-langs: "pt", "en-orig" */
    char *name; /* "Portuguese", or NULL */
    bool automatic; /* made by speech recognition */
} kp_ytdlp_sub;

void kp_ytdlp_sub_free(kp_ytdlp_sub *s);

/* The program to run, or NULL with *err (g_free it) saying what to set. */
char *kp_ytdlp_program(const kp_prefs *p, char **err);

/* The languages of "yt-dlp -J" output: the uploaded ones, then the
 * automatic captions in the video's own language only (YouTube offers
 * automatic translations to every language; they are not what one wants).
 * Live chat is not a subtitle. NULL with *err if the JSON is not a video.
 * The list's elements are kp_ytdlp_sub (free function set). */
GPtrArray *kp_ytdlp_parse_subs(const char *json, char **err);

/* The code to tell a player: "pt-orig" is "pt". g_free it. */
char *kp_ytdlp_player_lang(const char *lang);

/* done: subs (NULL on failure) and err, valid during the call. */
typedef void (*kp_ytdlp_list_cb)(void *ud, GPtrArray *subs, const char *err);
void kp_ytdlp_list(const char *program, const char *id, kp_ytdlp_list_cb done, void *ud);

/* base: the video's path without the extension. langs: NULL-terminated. */
typedef void (*kp_ytdlp_get_cb)(void *ud, const char *err);
void kp_ytdlp_download(const char *program, const char *id, const char *base,
                       const char *const *langs, kp_ytdlp_get_cb done, void *ud);

#endif
