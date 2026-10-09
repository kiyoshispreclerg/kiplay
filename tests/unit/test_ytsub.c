/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <glib.h>
#include <math.h>
#include <string.h>

#include "../harness.h"
#include "srv3.h"
#include "ytsub_ass.h"

/* Synthetic, in the shape of the files yt-dlp saves. */
static const char srv3_sample[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\" ?><timedtext format=\"3\">\n"
    "<head>\n"
    "<pen id=\"1\" fc=\"#FEFEFE\" fo=\"0\" bo=\"0\" sz=\"100\"/>\n"
    "<pen id=\"3\" b=\"1\" fc=\"#EF3F72\" fo=\"254\" bo=\"0\" ec=\"#88DCBC\" sz=\"500\" et=\"3\"/>\n"
    "<pen id=\"8\" b=\"1\" fc=\"#EF3F72\" fo=\"254\" bc=\"#000000\" bo=\"130\" sz=\"100\"/>\n"
    "<ws id=\"1\" ju=\"2\" pd=\"0\" sd=\"0\"/>\n"
    "<wp id=\"0\"/>\n"
    "<wp id=\"86\" ap=\"7\" ah=\"11\" av=\"28\"/>\n"
    "<wp id=\"2\" ap=\"0\" ah=\"0\" av=\"0\"/>\n"
    "</head>\n"
    "<body>\n"
    "<p t=\"1\" d=\"1999\" wp=\"86\" ws=\"1\" p=\"3\">Line one\nline &amp; two</p>\n"
    "<p t=\"36340\" d=\"100\" wp=\"2\" ws=\"1\"><s p=\"8\">PA</s><s>RTY {x}</s></p>\n"
    "<p t=\"40000\" d=\"500\" wp=\"0\" p=\"1\">invisible</p>\n"
    "</body>\n"
    "</timedtext>\n";

TEST(parses_heads_and_events)
{
    char *err = NULL;
    kp_ytsub *s = kp_srv3_parse(srv3_sample, strlen(srv3_sample), &err);
    CHECK(s != NULL);
    CHECK_INT_EQ(s->nevents, 3);

    const kp_ytpen *p = kp_ytsub_pen(s, 3);
    CHECK_INT_EQ(p->fc, 0xEF3F72);
    CHECK_INT_EQ(p->ec, 0x88DCBC);
    CHECK_INT_EQ(p->sz, 500);
    CHECK_INT_EQ(p->et, KP_YT_EDGE_GLOW);
    CHECK(p->b);
    /* Undefined pens and positions get YouTube's defaults. */
    CHECK_INT_EQ(kp_ytsub_pen(s, 99)->bo, 191);
    CHECK_INT_EQ(kp_ytsub_pos(s, 0)->ap, 7);
    CHECK(fabs(kp_ytsub_pos(s, 86)->ah - 11) < 1e-9);

    const kp_ytevent *e = &s->events[0];
    CHECK_INT_EQ(e->t, 1);
    CHECK_INT_EQ(e->d, 1999);
    CHECK_INT_EQ(e->pos, 86);
    CHECK_INT_EQ(e->pen, 3);
    CHECK_INT_EQ(e->nsegs, 1);
    CHECK_STR_EQ(e->segs[0].text, "Line one\nline & two");

    e = &s->events[1];
    CHECK_INT_EQ(e->nsegs, 2);
    CHECK_INT_EQ(e->segs[0].pen, 8);
    CHECK_INT_EQ(e->segs[1].pen, -1);
    kp_ytsub_free(s);
}

TEST(rejects_broken_xml)
{
    char *err = NULL;
    kp_ytsub *s = kp_srv3_parse("<timedtext><body><p>", 20, &err);
    CHECK(s == NULL);
    CHECK(err != NULL);
    g_free(err);
}

TEST(size_scale_is_a_quarter)
{
    CHECK(fabs(kp_ytass_size_scale(100) - 1.0) < 1e-9);
    CHECK(fabs(kp_ytass_size_scale(500) - 2.0) < 1e-9);
    CHECK(fabs(kp_ytass_size_scale(2100) - 6.0) < 1e-9);
    CHECK(kp_ytass_size_scale(-500) >= 0.25);
}

TEST(writes_positioned_ass)
{
    kp_ytsub *s = kp_srv3_parse(srv3_sample, strlen(srv3_sample), NULL);
    kp_ytass_opts o;
    kp_ytass_opts_default(&o);
    char *ass = kp_ytsub_to_ass(s, &o);

    CHECK_STR_HAS(ass, "PlayResX: 1920");
    /* wp 86: ap 7 -> \an2, (2 + 11*0.96)% of 1920, (2 + 28*0.96)% of 1080. */
    CHECK_STR_HAS(ass, "{\\an2\\pos(241.2,311.9)}");
    CHECK_STR_HAS(ass, "\\1c&H723FEF&");
    CHECK_STR_HAS(ass, "\\3c&HBCDC88&");
    CHECK_STR_HAS(ass, "Line one\\Nline & two");
    /* Only pen 8 has a background: one box line, then the text line. */
    CHECK_STR_HAS(ass, "Dialogue: 0,0:00:36.34,0:00:36.44,Box");
    CHECK_STR_HAS(ass, "Dialogue: 1,0:00:36.34,0:00:36.44,Text");
    /* Braces can't open an override block. */
    CHECK(strstr(ass, "{x}") == NULL);
    /* fo=0 and no background: nothing to draw. */
    CHECK(strstr(ass, "invisible") == NULL);
    g_free(ass);
    kp_ytsub_free(s);
}

int main(void)
{
    RUN(parses_heads_and_events);
    RUN(rejects_broken_xml);
    RUN(size_scale_is_a_quarter);
    RUN(writes_positioned_ass);
    return harness_report();
}
