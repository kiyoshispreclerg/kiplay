/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <glib.h>
#include <math.h>
#include <string.h>

#include "../harness.h"
#include "json3.h"
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

/* The srv3 sample above, as YouTube's json3 writes it: pens, positions and
 * styles are arrays indexed by id, colors integers, bAttr a bit mask. */
static const char json3_sample[] =
    "{\"wireMagic\":\"pb3\","
    "\"pens\":[{},"
    "{\"szPenSize\":100,\"fcForeColor\":16711422,\"foForeAlpha\":0,\"boBackAlpha\":0},{},"
    "{\"bAttr\":1,\"szPenSize\":500,\"etEdgeType\":3,\"fcForeColor\":15679346,"
    "\"foForeAlpha\":254,\"boBackAlpha\":0,\"ecEdgeColor\":8969404}],"
    "\"wsWinStyles\":[{},{\"juJustifCode\":2,\"pdPrintDir\":0,\"sdScrollDir\":0}],"
    "\"wpWinPositions\":[{},{},{\"apPoint\":0,\"ahHorPos\":0.5,\"avVerPos\":0}],"
    "\"events\":["
    "{\"tStartMs\":0,\"dDurationMs\":5000,\"id\":1,\"wpWinPosId\":2,\"wsWinStyleId\":1},"
    "{\"tStartMs\":1,\"dDurationMs\":1999,\"wpWinPosId\":2,\"wsWinStyleId\":1,\"pPenId\":3,"
    "\"segs\":[{\"utf8\":\"Line one\\nline & two\"}]},"
    "{\"tStartMs\":36340,\"dDurationMs\":100,\"wpWinPosId\":2,"
    "\"segs\":[{\"utf8\":\"PA\",\"pPenId\":3},{\"utf8\":\"RTY\"}]}]}";

TEST(json3_fills_the_same_model)
{
    char *err = NULL;
    kp_ytsub *s = kp_json3_parse(json3_sample, strlen(json3_sample), &err);
    CHECK(s != NULL);
    /* The window definition (no segs) is not an event. */
    CHECK_INT_EQ(s->nevents, 2);
    const kp_ytpen *p = kp_ytsub_pen(s, 3);
    CHECK_INT_EQ(p->fc, 0xEF3F72);
    CHECK_INT_EQ(p->ec, 0x88DCBC);
    CHECK_INT_EQ(p->sz, 500);
    CHECK_INT_EQ(p->et, KP_YT_EDGE_GLOW);
    CHECK(p->b && !p->i && !p->u);
    CHECK_INT_EQ(kp_ytsub_pen(s, 1)->fo, 0);
    CHECK_INT_EQ(kp_ytsub_pen(s, 2)->bo, 191); /* {} = YouTube's defaults */
    CHECK_INT_EQ(kp_ytsub_style(s, 1)->ju, 2);
    /* Decimals read the same under a locale with a decimal comma. */
    CHECK(fabs(kp_ytsub_pos(s, 2)->ah - 0.5) < 1e-9);

    const kp_ytevent *e = &s->events[0];
    CHECK_INT_EQ(e->t, 1);
    CHECK_INT_EQ(e->d, 1999);
    CHECK_INT_EQ(e->pos, 2);
    CHECK_INT_EQ(e->pen, 3);
    CHECK_STR_EQ(e->segs[0].text, "Line one\nline & two");
    e = &s->events[1];
    CHECK_INT_EQ(e->pen, -1);
    CHECK_INT_EQ(e->nsegs, 2);
    CHECK_INT_EQ(e->segs[0].pen, 3);
    CHECK_INT_EQ(e->segs[1].pen, -1);
    kp_ytsub_free(s);
}

TEST(json3_rejects_what_is_not_json3)
{
    char *err = NULL;
    CHECK(kp_json3_parse("<timedtext/>", 12, &err) == NULL);
    CHECK(err != NULL);
    g_free(err);
    CHECK(kp_json3_parse("[1,2]", 5, NULL) == NULL);
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
    RUN(json3_fills_the_same_model);
    RUN(json3_rejects_what_is_not_json3);
    RUN(rejects_broken_xml);
    RUN(size_scale_is_a_quarter);
    RUN(writes_positioned_ass);
    return harness_report();
}
