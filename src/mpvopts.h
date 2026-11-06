/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_MPVOPTS_H
#define KP_MPVOPTS_H

#include "prefs.h"

/* The preferences that become mpv options (Preferences > Video, Audio,
 * Subtitles), in kiplay.ini:
 *
 *   [video]      hwdec = no | auto-safe | ...    vo = gpu | xv | x11 | ...
 *   [audio]      ao = pulse | alsa | ...         languages = pt,en
 *   [subtitles]  languages = pt,en   font = Sans   font_size = 55
 *                auto = exact | fuzzy | no       (external subtitle files)
 *
 * Empty or absent keys leave mpv's own default (no option is passed), so
 * an mpv.conf read with [mpv] use_config still counts. */

/* NULL-terminated argv fragment; g_strfreev it. Never NULL. */
char **kp_mpv_pref_args(const kp_prefs *p);

/* The same preferences as MPlayer options ([video] hwdec and [subtitles]
 * auto have no MPlayer equivalent; [mplayer] vo and ao replace [video] vo
 * and [audio] ao, whose names are mpv's). NULL-terminated; g_strfreev it. */
char **kp_mplayer_pref_args(const kp_prefs *p);

/* Which program draws the video: "mpv" (default) or "mplayer"
 * ([video] backend). */
const char *kp_video_backend(const kp_prefs *p);

/* Everything that only takes effect when mpv starts (the options above,
 * [mpv] program, use_config and extra, or the MPlayer ones when it is
 * the video backend), as one string: when it changes,
 * a running mpv has to be restarted. g_free it. */
char *kp_mpv_pref_signature(const kp_prefs *p);

#endif
