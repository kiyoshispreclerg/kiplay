/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "json3.h"

#include <glib.h>
#include <locale.h>
#include <string.h>

#include "json.h"

static int clamp255(double v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (int)v;
}

/* The key's value if present, so a missing key leaves the default alone. */
static bool has(const cJSON *o, const char *key, double *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v))
        return false;
    *out = v->valuedouble;
    return true;
}

static void read_pen(kp_ytsub *sub, int id, const cJSON *o)
{
    kp_ytpen *p = kp_ytsub_pen_slot(sub, id);
    double v;
    if (!p)
        return;
    if (has(o, "fcForeColor", &v))
        p->fc = (uint32_t)v & 0xFFFFFF;
    if (has(o, "bcBackColor", &v))
        p->bc = (uint32_t)v & 0xFFFFFF;
    if (has(o, "ecEdgeColor", &v))
        p->ec = (uint32_t)v & 0xFFFFFF;
    if (has(o, "foForeAlpha", &v))
        p->fo = clamp255(v);
    if (has(o, "boBackAlpha", &v))
        p->bo = clamp255(v);
    if (has(o, "szPenSize", &v))
        p->sz = (int)v;
    if (has(o, "fsFontStyle", &v))
        p->fs = (int)v;
    if (has(o, "etEdgeType", &v))
        p->et = (int)v;
    if (has(o, "ofOffset", &v))
        p->of = (int)v;
    if (has(o, "rbRubyPart", &v))
        p->rb = (int)v;
    if (has(o, "bAttr", &v)) { /* bit 0 bold, 1 italic, 2 underline */
        p->b = ((int)v & 1) != 0;
        p->i = ((int)v & 2) != 0;
        p->u = ((int)v & 4) != 0;
    }
}

static void read_pos(kp_ytsub *sub, int id, const cJSON *o)
{
    kp_ytpos *p = kp_ytsub_pos_slot(sub, id);
    double v;
    if (!p)
        return;
    if (has(o, "apPoint", &v) && v >= 0 && v <= 8)
        p->ap = (int)v;
    if (has(o, "ahHorPos", &v))
        p->ah = v;
    if (has(o, "avVerPos", &v))
        p->av = v;
}

static void read_style(kp_ytsub *sub, int id, const cJSON *o)
{
    kp_ytstyle *s = kp_ytsub_style_slot(sub, id);
    double v;
    if (!s)
        return;
    if (has(o, "juJustifCode", &v))
        s->ju = (int)v;
    if (has(o, "pdPrintDir", &v))
        s->pd = (int)v;
    if (has(o, "sdScrollDir", &v))
        s->sd = (int)v;
}

/* The tables are arrays indexed by id. */
static void read_table(kp_ytsub *sub, const cJSON *root, const char *key,
                       void (*one)(kp_ytsub *, int, const cJSON *))
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, key);
    int id = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr)
    {
        if (cJSON_IsObject(it))
            one(sub, id, it);
        id++;
    }
}

static void read_event(kp_ytsub *sub, const cJSON *o)
{
    const cJSON *segs = cJSON_GetObjectItemCaseSensitive(o, "segs");
    if (!cJSON_IsArray(segs)) /* a window definition, nothing to show */
        return;
    kp_ytevent *e = kp_ytsub_add_event(sub);
    double v;
    if (has(o, "tStartMs", &v) && v >= 0)
        e->t = (int64_t)v;
    if (has(o, "dDurationMs", &v) && v >= 0)
        e->d = (int64_t)v;
    if (has(o, "wpWinPosId", &v))
        e->pos = (int)v;
    if (has(o, "wsWinStyleId", &v))
        e->style = (int)v;
    if (has(o, "pPenId", &v))
        e->pen = (int)v;
    const cJSON *s;
    cJSON_ArrayForEach(s, segs)
    {
        const char *text = kp_json_str(s, "utf8");
        if (!text || !*text)
            continue;
        int pen = -1;
        if (has(s, "pPenId", &v))
            pen = (int)v;
        kp_ytevent_add_seg(e, pen, text, strlen(text));
    }
}

kp_ytsub *kp_json3_parse(const char *data, size_t len, char **err)
{
    /* cJSON reads numbers with strtod: decimals need the C locale. */
    locale_t c = newlocale(LC_ALL_MASK, "C", (locale_t)0);
    locale_t old = c ? uselocale(c) : (locale_t)0;
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (c) {
        uselocale(old);
        freelocale(c);
    }
    if (!cJSON_IsObject(root)) {
        if (err)
            *err = g_strdup("not a json3 file");
        cJSON_Delete(root);
        return NULL;
    }
    kp_ytsub *sub = kp_ytsub_new();
    read_table(sub, root, "pens", read_pen);
    read_table(sub, root, "wsWinStyles", read_style);
    read_table(sub, root, "wpWinPositions", read_pos);
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "events"))
    {
        if (cJSON_IsObject(e))
            read_event(sub, e);
    }
    cJSON_Delete(root);
    return sub;
}

kp_ytsub *kp_json3_load(const char *path, char **err)
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
    kp_ytsub *s = kp_json3_parse(data, len, err);
    g_free(data);
    return s;
}
