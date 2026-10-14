/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_JSON3_H
#define KP_JSON3_H

#include <stddef.h>

#include "ytsub.h"

/* Reader for YouTube's json3 captions (`yt-dlp --sub-format json3`): the
 * same content as srv3 in JSON, with long attribute names and colors as
 * plain integers. Fills the same kp_ytsub, so the ASS writer doesn't care
 * which format a file came in. Unknown keys are ignored.
 *
 * Returns a new kp_ytsub (caller frees with kp_ytsub_free), or NULL with
 * *err set to a message the caller g_free()s. */
kp_ytsub *kp_json3_parse(const char *data, size_t len, char **err);
kp_ytsub *kp_json3_load(const char *path, char **err);

#endif
