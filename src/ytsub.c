/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ytsub.h"

#include <glib.h>
#include <string.h>

/* Ids seen so far stay under ~300; this only stops a broken file from
 * making us allocate gigabytes. */
#define KP_YT_MAX_ID 100000

kp_ytsub *kp_ytsub_new(void)
{
    return g_new0(kp_ytsub, 1);
}

void kp_ytsub_free(kp_ytsub *s)
{
    if (!s)
        return;
    for (size_t i = 0; i < s->nevents; i++) {
        for (size_t j = 0; j < s->events[i].nsegs; j++)
            g_free(s->events[i].segs[j].text);
        g_free(s->events[i].segs);
    }
    g_free(s->events);
    g_free(s->pens);
    g_free(s->pen_set);
    g_free(s->pos);
    g_free(s->pos_set);
    g_free(s->styles);
    g_free(s->style_set);
    g_free(s);
}

void kp_ytpen_default(kp_ytpen *p)
{
    memset(p, 0, sizeof(*p));
    p->fc = 0xFFFFFF;
    p->fo = 255;
    p->bc = 0x080808;
    p->bo = 191;
    p->ec = 0x000000;
    p->sz = 100;
    p->of = KP_YT_OFFSET_NORMAL;
}

void kp_ytpos_default(kp_ytpos *p)
{
    p->ap = 7;
    p->ah = 50;
    p->av = 100;
}

void kp_ytstyle_default(kp_ytstyle *s)
{
    s->ju = 2;
    s->pd = 0;
    s->sd = 0;
}

/* Grows a table (items of size elem plus its "set" flags) to hold id. */
static bool grow(void **items, bool **set, size_t *n, size_t elem, int id)
{
    if (id < 0 || id > KP_YT_MAX_ID)
        return false;
    size_t want = (size_t)id + 1;
    if (want <= *n)
        return true;
    *items = g_realloc(*items, want * elem);
    *set = g_realloc(*set, want * sizeof(bool));
    memset((char *)*items + *n * elem, 0, (want - *n) * elem);
    memset(*set + *n, 0, (want - *n) * sizeof(bool));
    *n = want;
    return true;
}

kp_ytpen *kp_ytsub_pen_slot(kp_ytsub *s, int id)
{
    if (!grow((void **)&s->pens, &s->pen_set, &s->npens, sizeof(kp_ytpen), id))
        return NULL;
    if (!s->pen_set[id]) {
        kp_ytpen_default(&s->pens[id]);
        s->pen_set[id] = true;
    }
    return &s->pens[id];
}

kp_ytpos *kp_ytsub_pos_slot(kp_ytsub *s, int id)
{
    if (!grow((void **)&s->pos, &s->pos_set, &s->npos, sizeof(kp_ytpos), id))
        return NULL;
    if (!s->pos_set[id]) {
        kp_ytpos_default(&s->pos[id]);
        s->pos_set[id] = true;
    }
    return &s->pos[id];
}

kp_ytstyle *kp_ytsub_style_slot(kp_ytsub *s, int id)
{
    if (!grow((void **)&s->styles, &s->style_set, &s->nstyles, sizeof(kp_ytstyle), id))
        return NULL;
    if (!s->style_set[id]) {
        kp_ytstyle_default(&s->styles[id]);
        s->style_set[id] = true;
    }
    return &s->styles[id];
}

const kp_ytpen *kp_ytsub_pen(const kp_ytsub *s, int id)
{
    static kp_ytpen def;
    static bool init;
    if (id >= 0 && (size_t)id < s->npens && s->pen_set[id])
        return &s->pens[id];
    if (!init) {
        kp_ytpen_default(&def);
        init = true;
    }
    return &def;
}

const kp_ytpos *kp_ytsub_pos(const kp_ytsub *s, int id)
{
    static const kp_ytpos def = {7, 50, 100};
    if (id >= 0 && (size_t)id < s->npos && s->pos_set[id])
        return &s->pos[id];
    return &def;
}

const kp_ytstyle *kp_ytsub_style(const kp_ytsub *s, int id)
{
    static const kp_ytstyle def = {2, 0, 0};
    if (id >= 0 && (size_t)id < s->nstyles && s->style_set[id])
        return &s->styles[id];
    return &def;
}

kp_ytevent *kp_ytsub_add_event(kp_ytsub *s)
{
    if (s->nevents == s->cap_events) {
        s->cap_events = s->cap_events ? s->cap_events * 2 : 64;
        s->events = g_renew(kp_ytevent, s->events, s->cap_events);
    }
    kp_ytevent *e = &s->events[s->nevents++];
    memset(e, 0, sizeof(*e));
    e->pos = e->style = e->pen = -1;
    return e;
}

void kp_ytevent_add_seg(kp_ytevent *e, int pen, const char *text, size_t len)
{
    e->segs = g_renew(kp_ytseg, e->segs, e->nsegs + 1);
    e->segs[e->nsegs].pen = pen;
    e->segs[e->nsegs].text = g_strndup(text, len);
    e->nsegs++;
}
