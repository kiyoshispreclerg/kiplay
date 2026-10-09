/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "srv3.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>

/* What we're inside of. Text directly in <p> belongs to the event's pen;
 * text in <s> to the segment's pen (or the event's, if <s> has none). */
typedef struct {
    kp_ytsub *sub;
    long event; /* index of the open <p>, -1 outside */
    int seg_pen;
    bool in_s;
} parse_state;

static bool parse_int(const char *v, int *out)
{
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v || n < -1000000 || n > 1000000)
        return false;
    *out = (int)n;
    return true;
}

static bool parse_i64(const char *v, int64_t *out)
{
    char *end;
    long long n = strtoll(v, &end, 10);
    if (end == v || n < 0)
        return false;
    *out = n;
    return true;
}

/* "#RRGGBB" */
static bool parse_color(const char *v, uint32_t *out)
{
    if (v[0] != '#' || strlen(v) != 7)
        return false;
    char *end;
    unsigned long n = strtoul(v + 1, &end, 16);
    if (*end)
        return false;
    *out = (uint32_t)n;
    return true;
}

static int clamp255(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

static void read_pen(kp_ytsub *sub, const char **names, const char **values)
{
    int id = -1;
    for (int k = 0; names[k]; k++)
        if (!strcmp(names[k], "id"))
            parse_int(values[k], &id);
    kp_ytpen *p = kp_ytsub_pen_slot(sub, id);
    if (!p)
        return;
    for (int k = 0; names[k]; k++) {
        const char *n = names[k], *v = values[k];
        int x;
        if (!strcmp(n, "fc"))
            parse_color(v, &p->fc);
        else if (!strcmp(n, "bc"))
            parse_color(v, &p->bc);
        else if (!strcmp(n, "ec"))
            parse_color(v, &p->ec);
        else if (!strcmp(n, "fo") && parse_int(v, &x))
            p->fo = clamp255(x);
        else if (!strcmp(n, "bo") && parse_int(v, &x))
            p->bo = clamp255(x);
        else if (!strcmp(n, "sz") && parse_int(v, &x))
            p->sz = x;
        else if (!strcmp(n, "fs") && parse_int(v, &x))
            p->fs = x;
        else if (!strcmp(n, "et") && parse_int(v, &x))
            p->et = x;
        else if (!strcmp(n, "of") && parse_int(v, &x))
            p->of = x;
        else if (!strcmp(n, "rb") && parse_int(v, &x))
            p->rb = x;
        else if (!strcmp(n, "b") && parse_int(v, &x))
            p->b = x != 0;
        else if (!strcmp(n, "i") && parse_int(v, &x))
            p->i = x != 0;
        else if (!strcmp(n, "u") && parse_int(v, &x))
            p->u = x != 0;
    }
}

static void read_pos(kp_ytsub *sub, const char **names, const char **values)
{
    int id = -1;
    for (int k = 0; names[k]; k++)
        if (!strcmp(names[k], "id"))
            parse_int(values[k], &id);
    kp_ytpos *p = kp_ytsub_pos_slot(sub, id);
    if (!p)
        return;
    for (int k = 0; names[k]; k++) {
        int x;
        if (!strcmp(names[k], "ap") && parse_int(values[k], &x) && x >= 0 && x <= 8)
            p->ap = x;
        else if (!strcmp(names[k], "ah"))
            p->ah = g_ascii_strtod(values[k], NULL);
        else if (!strcmp(names[k], "av"))
            p->av = g_ascii_strtod(values[k], NULL);
    }
}

static void read_style(kp_ytsub *sub, const char **names, const char **values)
{
    int id = -1;
    for (int k = 0; names[k]; k++)
        if (!strcmp(names[k], "id"))
            parse_int(values[k], &id);
    kp_ytstyle *s = kp_ytsub_style_slot(sub, id);
    if (!s)
        return;
    for (int k = 0; names[k]; k++) {
        int x;
        if (!parse_int(values[k], &x))
            continue;
        if (!strcmp(names[k], "ju"))
            s->ju = x;
        else if (!strcmp(names[k], "pd"))
            s->pd = x;
        else if (!strcmp(names[k], "sd"))
            s->sd = x;
    }
}

static void on_start(GMarkupParseContext *ctx, const char *element,
                     const char **names, const char **values, gpointer data,
                     GError **error)
{
    (void)ctx;
    (void)error;
    parse_state *st = data;
    if (!strcmp(element, "pen") && st->event < 0) {
        read_pen(st->sub, names, values);
    } else if (!strcmp(element, "wp")) {
        read_pos(st->sub, names, values);
    } else if (!strcmp(element, "ws")) {
        read_style(st->sub, names, values);
    } else if (!strcmp(element, "p")) {
        kp_ytevent *e = kp_ytsub_add_event(st->sub);
        st->event = (long)st->sub->nevents - 1;
        for (int k = 0; names[k]; k++) {
            const char *n = names[k], *v = values[k];
            if (!strcmp(n, "t"))
                parse_i64(v, &e->t);
            else if (!strcmp(n, "d"))
                parse_i64(v, &e->d);
            else if (!strcmp(n, "wp"))
                parse_int(v, &e->pos);
            else if (!strcmp(n, "ws"))
                parse_int(v, &e->style);
            else if (!strcmp(n, "p"))
                parse_int(v, &e->pen);
        }
    } else if (!strcmp(element, "s") && st->event >= 0) {
        st->in_s = true;
        st->seg_pen = -1;
        for (int k = 0; names[k]; k++)
            if (!strcmp(names[k], "p"))
                parse_int(values[k], &st->seg_pen);
    }
}

static void on_end(GMarkupParseContext *ctx, const char *element, gpointer data,
                   GError **error)
{
    (void)ctx;
    (void)error;
    parse_state *st = data;
    if (!strcmp(element, "p"))
        st->event = -1;
    else if (!strcmp(element, "s"))
        st->in_s = false;
}

static void on_text(GMarkupParseContext *ctx, const char *text, gsize len,
                    gpointer data, GError **error)
{
    (void)ctx;
    (void)error;
    parse_state *st = data;
    if (st->event < 0 || len == 0)
        return;
    kp_ytevent *e = &st->sub->events[st->event];
    kp_ytevent_add_seg(e, st->in_s ? st->seg_pen : -1, text, len);
}

kp_ytsub *kp_srv3_parse(const char *data, size_t len, char **err)
{
    static const GMarkupParser parser = {on_start, on_end, on_text, NULL, NULL};
    parse_state st = {kp_ytsub_new(), -1, -1, false};
    GMarkupParseContext *ctx = g_markup_parse_context_new(&parser, 0, &st, NULL);
    GError *gerr = NULL;
    bool ok = g_markup_parse_context_parse(ctx, data, (gssize)len, &gerr) &&
              g_markup_parse_context_end_parse(ctx, &gerr);
    g_markup_parse_context_free(ctx);
    if (!ok) {
        if (err)
            *err = g_strdup(gerr->message);
        g_error_free(gerr);
        kp_ytsub_free(st.sub);
        return NULL;
    }
    return st.sub;
}

kp_ytsub *kp_srv3_load(const char *path, char **err)
{
    char *data;
    gsize len;
    GError *gerr = NULL;
    if (!g_file_get_contents(path, &data, &len, &gerr)) {
        if (err)
            *err = g_strdup(gerr->message);
        g_error_free(gerr);
        return NULL;
    }
    kp_ytsub *s = kp_srv3_parse(data, len, err);
    g_free(data);
    return s;
}
