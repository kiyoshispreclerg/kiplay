/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <glib.h>
#include <string.h>

#include "../harness.h"
#include "ytdlp.h"

static const char video_json[] =
    "{\"id\":\"abcdefghijk\",\"language\":\"ko\","
    "\"subtitles\":{\"en\":[{\"ext\":\"srv3\",\"name\":\"English\"}],"
    "\"live_chat\":[{\"ext\":\"json\"}]},"
    "\"automatic_captions\":{\"ko-orig\":[{\"ext\":\"srv3\",\"name\":\"Korean (Original)\"}],"
    "\"ko\":[{\"ext\":\"srv3\"}],\"pt\":[{\"ext\":\"srv3\"}]}}";

TEST(lists_uploaded_and_original_automatic)
{
    char *err = NULL;
    GPtrArray *subs = kp_ytdlp_parse_subs(video_json, &err);
    CHECK(subs != NULL);
    CHECK_INT_EQ((int)subs->len, 2);
    kp_ytdlp_sub *a = g_ptr_array_index(subs, 0), *b = g_ptr_array_index(subs, 1);
    CHECK_STR_EQ(a->lang, "en");
    CHECK_STR_EQ(a->name, "English");
    CHECK(!a->automatic);
    CHECK_STR_EQ(b->lang, "ko-orig");
    CHECK(b->automatic);
    g_ptr_array_free(subs, TRUE);
}

TEST(old_output_without_orig)
{
    GPtrArray *subs = kp_ytdlp_parse_subs(
        "{\"id\":\"x\",\"language\":\"pt\",\"automatic_captions\":{\"pt\":[{}],\"en\":[{}]}}",
        NULL);
    CHECK(subs != NULL);
    CHECK_INT_EQ((int)subs->len, 1);
    CHECK_STR_EQ(((kp_ytdlp_sub *)g_ptr_array_index(subs, 0))->lang, "pt");
    g_ptr_array_free(subs, TRUE);
}

TEST(rejects_what_is_not_a_video)
{
    char *err = NULL;
    CHECK(kp_ytdlp_parse_subs("not json", &err) == NULL);
    CHECK(err != NULL);
    g_free(err);
    CHECK(kp_ytdlp_parse_subs("{\"entries\":[]}", NULL) == NULL);
}

TEST(player_language_code)
{
    char *a = kp_ytdlp_player_lang("ko-orig"), *b = kp_ytdlp_player_lang("pt-BR");
    CHECK_STR_EQ(a, "ko");
    CHECK_STR_EQ(b, "pt-BR");
    g_free(a);
    g_free(b);
}

int main(void)
{
    RUN(lists_uploaded_and_original_automatic);
    RUN(old_output_without_orig);
    RUN(rejects_what_is_not_a_video);
    RUN(player_language_code);
    return harness_report();
}
