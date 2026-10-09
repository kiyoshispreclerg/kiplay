/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ytsub_ass.h"

#include <glib.h>
#include <math.h>
#include <string.h>

void kp_ytass_opts_default(kp_ytass_opts *o)
{
    o->width = 1920;
    o->height = 1080;
    /* Calibrated against a recording of YouTube's player (docs/FORMATOS.md). */
    o->em_ratio = 0.0625;
    o->edge_scale = 1.0;
    /* YouTube's families, with the substitutes fontconfig doesn't pick by
     * itself (Monotype Corsiva would fall back to a sans). */
    o->fonts[0] = "Roboto";
    o->fonts[1] = "Courier New";
    o->fonts[2] = "Times New Roman";
    o->fonts[3] = "Lucida Console";
    o->fonts[4] = "Roboto";
    o->fonts[5] = "Comic Sans MS";
    o->fonts[6] = "Z003";
    o->fonts[7] = "Carrois Gothic SC";
}

double kp_ytass_size_scale(int sz)
{
    double s = 1.0 + (sz - 100) / 400.0;
    return s < 0.25 ? 0.25 : s;
}

/* ASS anchors (\an, numpad layout) for YouTube's ap 0..8 (reading order). */
static const int an_for_ap[9] = {7, 8, 9, 4, 5, 6, 1, 2, 3};

static void put_color(GString *out, const char *tag, uint32_t rgb)
{
    g_string_append_printf(out, "\\%s&H%02X%02X%02X&", tag, rgb & 0xFF,
                           (rgb >> 8) & 0xFF, (rgb >> 16) & 0xFF);
}

static void put_alpha(GString *out, const char *tag, int opacity)
{
    /* YouTube writes 254 for opaque; ASS alpha is transparency. */
    int a = opacity >= 254 ? 0 : 255 - opacity;
    g_string_append_printf(out, "\\%s&H%02X&", tag, a);
}

static void put_num(GString *out, const char *tag, double v)
{
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(buf, sizeof(buf), "%.2f", v);
    g_string_append_printf(out, "\\%s%s", tag, buf);
}

/* Text with ASS's specials neutralized: '{' would open an override block,
 * a backslash could form \N or \h; '\n' becomes a hard line break. */
static void put_text(GString *out, const char *t)
{
    for (const char *c = t; *c; c++) {
        switch (*c) {
        case '\n':
            g_string_append(out, "\\N");
            break;
        case '\r':
            break;
        case '{':
            g_string_append(out, "\xEF\xBD\x9B"); /* U+FF5B */
            break;
        case '}':
            g_string_append(out, "\xEF\xBD\x9D"); /* U+FF5D */
            break;
        case '\\':
            g_string_append(out, "\\\xE2\x81\xA0"); /* + U+2060 word joiner */
            break;
        default:
            g_string_append_c(out, *c);
        }
    }
}

static void put_time(GString *out, int64_t ms)
{
    /* Centiseconds, truncated: consecutive events (t2 = t1 + d1) keep
     * touching exactly, with no gap or overlap frame between them. */
    int64_t cs = ms / 10;
    g_string_append_printf(out, "%d:%02d:%02d.%02d", (int)(cs / 360000),
                           (int)(cs / 6000 % 60), (int)(cs / 100 % 60),
                           (int)(cs % 100));
}

static const char *font_for(const kp_ytass_opts *o, int fs)
{
    if (fs < 0 || fs >= KP_YT_FONT_STYLES || !o->fonts[fs])
        return o->fonts[0];
    return o->fonts[fs];
}

static void put_font(GString *out, const kp_ytass_opts *o, const kp_ytpen *p, double em)
{
    g_string_append(out, "\\fn");
    g_string_append(out, font_for(o, p->fs));
    put_num(out, "fs", em);
    g_string_append_printf(out, "\\b%d\\i%d\\u%d", p->b, p->i, p->u);
}

/* The background box of one segment (layer 0). */
static void put_box_seg(GString *out, const kp_ytass_opts *o, const kp_ytpen *p, double em)
{
    g_string_append_c(out, '{');
    put_font(out, o, p, em);
    g_string_append(out, "\\1a&HFF&\\shad0");
    put_color(out, "3c", p->bc);
    put_alpha(out, "3a", p->bo);
    put_num(out, "bord", em * 0.12);
    g_string_append_c(out, '}');
}

/* Text and edge of one segment (layer 1). */
static void put_text_seg(GString *out, const kp_ytass_opts *o, const kp_ytpen *p, double em,
                         double base)
{
    /* YouTube draws edges at a fixed width from the base size, not the
     * pen's: a sz=2100 "YEAH!!" gets the same thin line as normal text. */
    double edge = base * 0.03 * o->edge_scale;
    g_string_append_c(out, '{');
    put_font(out, o, p, em);
    put_color(out, "1c", p->fc);
    put_alpha(out, "1a", p->fo);
    switch (p->et) {
    case KP_YT_EDGE_GLOW:
        put_color(out, "3c", p->ec);
        put_alpha(out, "3a", p->fo);
        put_num(out, "bord", fmax(1.0, o->edge_scale > 1.0 ? em * 0.03 * o->edge_scale : edge));
        if (o->edge_scale > 1.0)
            put_num(out, "blur", em * 0.01 * o->edge_scale);
        g_string_append(out, "\\shad0");
        break;
    case KP_YT_EDGE_SOFT_SHADOW:
    case KP_YT_EDGE_HARD_SHADOW:
        put_color(out, "4c", p->ec);
        put_alpha(out, "4a", p->fo);
        g_string_append(out, "\\bord0");
        put_num(out, "shad", fmax(1.0, edge * (p->et == KP_YT_EDGE_SOFT_SHADOW ? 1.5 : 2.0)));
        break;
    case KP_YT_EDGE_BEVEL:
        put_color(out, "4c", p->ec);
        put_alpha(out, "4a", p->fo);
        g_string_append(out, "\\bord0");
        put_num(out, "xshad", fmax(1.0, edge));
        put_num(out, "yshad", fmax(1.0, edge));
        break;
    default:
        g_string_append(out, "\\bord0\\shad0");
    }
    g_string_append_c(out, '}');
}

static void put_event(GString *out, const kp_ytsub *s, const kp_ytevent *e,
                      const kp_ytass_opts *o)
{
    if (e->nsegs == 0)
        return;
    bool visible = false, boxed = false;
    for (size_t i = 0; i < e->nsegs; i++) {
        const kp_ytpen *p = kp_ytsub_pen(s, e->segs[i].pen >= 0 ? e->segs[i].pen : e->pen);
        const char *t = e->segs[i].text;
        bool blank = t[strspn(t, " \n\r\t")] == '\0';
        if (!blank && p->fo > 0)
            visible = true;
        if (!blank && p->bo > 0)
            boxed = true;
    }
    if (!visible && !boxed)
        return;

    const kp_ytpos *pos = kp_ytsub_pos(s, e->pos);
    /* The caption area is the video minus a 2% margin on each side. */
    double x = (2.0 + pos->ah * 0.96) / 100.0 * o->width;
    double y = (2.0 + pos->av * 0.96) / 100.0 * o->height;
    double base = o->height * o->em_ratio;

    for (int layer = boxed ? 0 : 1; layer <= 1; layer++) {
        if (layer == 1 && !visible)
            break;
        g_string_append_printf(out, "Dialogue: %d,", layer);
        put_time(out, e->t);
        g_string_append_c(out, ',');
        put_time(out, e->t + (e->d > 10 ? e->d : 10));
        g_string_append_printf(out, ",%s,,0,0,0,,{\\an%d", layer ? "Text" : "Box",
                               an_for_ap[pos->ap]);
        char bx[G_ASCII_DTOSTR_BUF_SIZE], by[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd(bx, sizeof(bx), "%.1f", x);
        g_ascii_formatd(by, sizeof(by), "%.1f", y);
        g_string_append_printf(out, "\\pos(%s,%s)}", bx, by);
        for (size_t i = 0; i < e->nsegs; i++) {
            const kp_ytpen *p = kp_ytsub_pen(s, e->segs[i].pen >= 0 ? e->segs[i].pen : e->pen);
            double em = base * kp_ytass_size_scale(p->sz);
            if (layer == 0)
                put_box_seg(out, o, p, em);
            else
                put_text_seg(out, o, p, em, base);
            put_text(out, e->segs[i].text);
        }
        g_string_append_c(out, '\n');
    }
}

char *kp_ytsub_to_ass(const kp_ytsub *s, const kp_ytass_opts *o)
{
    GString *out = g_string_new(NULL);
    g_string_append_printf(out,
        "[Script Info]\n"
        "; Converted from YouTube captions by kiplay " KP_VERSION "\n"
        "ScriptType: v4.00+\n"
        "PlayResX: %d\n"
        "PlayResY: %d\n"
        "WrapStyle: 2\n"
        "ScaledBorderAndShadow: yes\n"
        "YCbCr Matrix: None\n"
        "\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
        "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, "
        "ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, "
        "MarginL, MarginR, MarginV, Encoding\n"
        "Style: Text,%s,%d,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,0,0,0,0,"
        "100,100,0,0,1,0,0,2,0,0,0,1\n"
        "Style: Box,%s,%d,&HFF000000,&HFF000000,&H40080808,&HFF000000,0,0,0,0,"
        "100,100,0,0,3,0,0,2,0,0,0,1\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
        "Effect, Text\n",
        o->width, o->height, o->fonts[0], (int)lround(o->height * o->em_ratio),
        o->fonts[0], (int)lround(o->height * o->em_ratio));
    for (size_t i = 0; i < s->nevents; i++)
        put_event(out, s, &s->events[i], o);
    return g_string_free(out, FALSE);
}
