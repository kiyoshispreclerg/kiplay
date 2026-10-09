/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_YTSUB_ASS_H
#define KP_YTSUB_ASS_H

#include "ytsub.h"

/* Writes a kp_ytsub as ASS, for the backend's libass to draw. Every event
 * is positioned (\an + \pos) the way YouTube's player places its caption
 * windows, so the "dancing" captions (many short events at moving
 * positions) come out as on the site.
 *
 * Each event becomes up to two lines: layer 0 draws the background boxes
 * (BorderStyle 3, text invisible), layer 1 the text with its edge. libass
 * 0.15 has no BorderStyle 4, so a box and an outline can't share a line. */

#define KP_YT_FONT_STYLES 8

typedef struct {
    int width, height; /* PlayResX/Y: the video's size */
    /* ASS font size at sz=100, as a fraction of the video height. */
    double em_ratio;
    /* Edge thickness: 1.0 = YouTube's (a thin line); larger values give
     * the thick blurred glow some people prefer. */
    double edge_scale;
    /* Font family for each pen fs (0 = default). */
    const char *fonts[KP_YT_FONT_STYLES];
} kp_ytass_opts;

void kp_ytass_opts_default(kp_ytass_opts *o);

/* The whole .ass file; caller g_free()s. */
char *kp_ytsub_to_ass(const kp_ytsub *s, const kp_ytass_opts *o);

/* YouTube scales pen sizes by a quarter of the difference from 100%:
 * sz=200 is 1.25x, sz=2100 is 6x. Exposed for the tests. */
double kp_ytass_size_scale(int sz);

#endif
