/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_SRV3_H
#define KP_SRV3_H

#include <stddef.h>

#include "ytsub.h"

/* Reader for YouTube's srv3 captions (<timedtext format="3">), what
 * `yt-dlp --sub-format srv3` saves. Unknown elements and attributes are
 * ignored, so newer files still load.
 *
 * Returns a new kp_ytsub (caller frees with kp_ytsub_free), or NULL with
 * *err set to a message the caller g_free()s. */
kp_ytsub *kp_srv3_parse(const char *data, size_t len, char **err);
kp_ytsub *kp_srv3_load(const char *path, char **err);

#endif
