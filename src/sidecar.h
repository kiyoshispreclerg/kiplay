/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_SIDECAR_H
#define KP_SIDECAR_H

#include <glib.h>
#include <stdbool.h>

#include "prefs.h"
#include "ytsub_ass.h"

/* Files that sit next to a video and belong to it, found by name: for
 * `Title [ID].mp4` (yt-dlp's name), `Title [ID].<lang>.<ext>` and
 * `Title [ID].live_chat.json`. The part before the video's extension is
 * the base name, so it works without the [ID] too; the id is only for
 * whoever wants it (cache, fetching what is missing).
 *
 * Per language, YouTube's styled captions (srv3, then json3) win over the
 * plain ones (vtt, srt...) of the same language: the plain ones lost the
 * style. Styled captions are converted to ASS in the cache folder and that
 * file is what the player is given (kp_sidecar_play_path).
 *
 * Memory: kp_sidecar and its strings belong to the list (g_ptr_array_free
 * frees them: the list is made with kp_sidecar_free as the element
 * destructor). */

typedef enum {
    KP_SIDECAR_YT_SUB, /* srv3 / json3: converted to ASS */
    KP_SIDECAR_SUB,    /* a subtitle file the player reads itself */
    KP_SIDECAR_CHAT,   /* live_chat.json */
} kp_sidecar_kind;

typedef struct {
    kp_sidecar_kind kind;
    char *path;
    char *lang;   /* "ko", "es-419"; "" if the name has none */
    char *format; /* extension, lower case: "srv3", "vtt"... */
} kp_sidecar;

void kp_sidecar_free(kp_sidecar *sc);

/* The sidecars of video_path, sorted by language (chat last). Empty when
 * there are none or the folder can't be read. */
GPtrArray *kp_sidecars_find(const char *video_path);

/* The `ID` of "Title [ID].ext" (11 characters of A-Za-z0-9_-), or NULL.
 * g_free it. */
char *kp_sidecar_video_id(const char *path);

/* A srv3 or json3 file (by its extension, srv3 if unsure). */
kp_ytsub *kp_ytsub_load(const char *path, char **err);

/* Options for the conversion from [subtitles] in the preferences:
 * youtube_edge = youtube (thin, as on the site) | strong. */
void kp_sidecar_ass_opts(const kp_prefs *p, kp_ytass_opts *o);

/* What to give the player for a subtitle sidecar: the file itself, or for
 * a styled one the ASS made from it, in cache_dir (NULL = the user cache,
 * ~/.cache/kiplay/subs), reused while the source and the options are the
 * same. NULL with *err on failure. g_free it. */
char *kp_sidecar_play_path(const kp_sidecar *sc, const kp_ytass_opts *o, const char *cache_dir,
                           char **err);

/* Whether path is inside the cache folder of converted captions (for the
 * menus to call them "YouTube"). */
bool kp_sidecar_is_converted(const char *path);

#endif
