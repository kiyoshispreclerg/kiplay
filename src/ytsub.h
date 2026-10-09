/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_YTSUB_H
#define KP_YTSUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* YouTube's styled captions, as a neutral model that both readers (srv3,
 * later json3) fill and the ASS writer reads. The format is undocumented;
 * docs/FORMATOS.md records what was observed.
 *
 * Pens, window positions and window styles are referenced by id from the
 * events. Ids are small integers but sparse and out of order, so each table
 * is indexed by id directly and grows to the largest id seen; entries never
 * defined in the file keep YouTube's defaults (kp_ytsub_pen etc. return
 * those for unknown ids too).
 *
 * Memory: kp_ytsub owns everything inside it; free it with kp_ytsub_free. */

enum kp_yt_edge {
    KP_YT_EDGE_NONE = 0,
    KP_YT_EDGE_HARD_SHADOW = 1,
    KP_YT_EDGE_BEVEL = 2,
    KP_YT_EDGE_GLOW = 3,        /* the "uniform" outline */
    KP_YT_EDGE_SOFT_SHADOW = 4, /* the "drop shadow" */
};

enum kp_yt_offset {
    KP_YT_OFFSET_SUB = 0,
    KP_YT_OFFSET_NORMAL = 1,
    KP_YT_OFFSET_SUPER = 2,
};

typedef struct {
    uint32_t fc, bc, ec; /* text, background, edge colors: 0xRRGGBB */
    int fo, bo;          /* text and background opacity, 0..255 */
    int sz;              /* size in percent; 100 = normal */
    int fs;              /* font style 0..7 (default, mono serif, ...) */
    int et;              /* enum kp_yt_edge */
    int of;              /* enum kp_yt_offset */
    int rb;              /* ruby part, 0 = none */
    bool b, i, u;
} kp_ytpen;

typedef struct {
    int ap;         /* anchor point 0..8: top-left .. bottom-right */
    double ah, av;  /* anchor position, percent of the video, 0..100 */
} kp_ytpos;

typedef struct {
    int ju; /* justification: 0 left, 1 right, 2 center, 3 justify */
    int pd; /* print direction: 0 horizontal, 1 RTL, 2/3 vertical */
    int sd; /* scroll direction */
} kp_ytstyle;

typedef struct {
    int pen;    /* pen id, -1 = the event's pen */
    char *text; /* UTF-8, may contain '\n' */
} kp_ytseg;

typedef struct {
    int64_t t, d; /* start and duration, ms */
    int pos;      /* window position id, -1 = default */
    int style;    /* window style id, -1 = default */
    int pen;      /* pen id, -1 = default pen */
    kp_ytseg *segs;
    size_t nsegs;
} kp_ytevent;

typedef struct {
    kp_ytpen *pens;
    bool *pen_set;
    size_t npens;
    kp_ytpos *pos;
    bool *pos_set;
    size_t npos;
    kp_ytstyle *styles;
    bool *style_set;
    size_t nstyles;
    kp_ytevent *events;
    size_t nevents, cap_events;
} kp_ytsub;

kp_ytsub *kp_ytsub_new(void);
void kp_ytsub_free(kp_ytsub *s);

/* YouTube's defaults: white text, black background at 75%, bottom center. */
void kp_ytpen_default(kp_ytpen *p);
void kp_ytpos_default(kp_ytpos *p);
void kp_ytstyle_default(kp_ytstyle *s);

/* Slot for id, created with defaults (and the table grown) if needed.
 * NULL if id is negative or absurdly large. */
kp_ytpen *kp_ytsub_pen_slot(kp_ytsub *s, int id);
kp_ytpos *kp_ytsub_pos_slot(kp_ytsub *s, int id);
kp_ytstyle *kp_ytsub_style_slot(kp_ytsub *s, int id);

/* Lookups: the defaults for ids the file never defined. */
const kp_ytpen *kp_ytsub_pen(const kp_ytsub *s, int id);
const kp_ytpos *kp_ytsub_pos(const kp_ytsub *s, int id);
const kp_ytstyle *kp_ytsub_style(const kp_ytsub *s, int id);

/* Appends an empty event; the pointer is valid until the next append. */
kp_ytevent *kp_ytsub_add_event(kp_ytsub *s);
/* Appends a segment (text is copied). */
void kp_ytevent_add_seg(kp_ytevent *e, int pen, const char *text, size_t len);

#endif
