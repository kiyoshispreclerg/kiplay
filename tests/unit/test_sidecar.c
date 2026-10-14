/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "../harness.h"
#include "sidecar.h"

static const char srv3_min[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\" ?><timedtext format=\"3\"><head>"
    "<wp id=\"2\" ap=\"0\" ah=\"0\" av=\"0\"/></head><body>"
    "<p t=\"1\" d=\"900\" wp=\"2\">hello</p></body></timedtext>";
static const char json3_min[] =
    "{\"wpWinPositions\":[{},{},{\"apPoint\":0,\"ahHorPos\":0,\"avVerPos\":0}],"
    "\"events\":[{\"tStartMs\":1,\"dDurationMs\":900,\"wpWinPosId\":2,"
    "\"segs\":[{\"utf8\":\"hola\"}]}]}";

static char *make_dir(void)
{
    GError *err = NULL;
    char *dir = g_dir_make_tmp("kp-sidecar-XXXXXX", &err);
    if (!dir)
        g_error("%s", err->message);
    return dir;
}

static void touch(const char *dir, const char *name, const char *content)
{
    char *p = g_build_filename(dir, name, NULL);
    CHECK(g_file_set_contents(p, content, -1, NULL));
    g_free(p);
}

static void rm_rf(const char *dir)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    const char *n;
    while (d && (n = g_dir_read_name(d))) {
        char *p = g_build_filename(dir, n, NULL);
        if (g_file_test(p, G_FILE_TEST_IS_DIR))
            rm_rf(p);
        else
            g_unlink(p);
        g_free(p);
    }
    if (d)
        g_dir_close(d);
    g_rmdir(dir);
}

static const kp_sidecar *find(GPtrArray *a, const char *lang)
{
    for (guint i = 0; i < a->len; i++) {
        const kp_sidecar *sc = g_ptr_array_index(a, i);
        if (sc->kind != KP_SIDECAR_CHAT && !strcmp(sc->lang, lang))
            return sc;
    }
    return NULL;
}

TEST(video_id_is_the_last_bracket)
{
    char *id = kp_sidecar_video_id("/x/Title [with] brackets [D00hlkW0u3U].webm");
    CHECK_STR_EQ(id, "D00hlkW0u3U");
    g_free(id);
    CHECK(kp_sidecar_video_id("/x/Title.mp4") == NULL);
    CHECK(kp_sidecar_video_id("/x/Title [short].mp4") == NULL);
}

TEST(styled_beats_plain_per_language)
{
    char *dir = make_dir();
    touch(dir, "T [abcdefghijk].mp4", "");
    touch(dir, "T [abcdefghijk].ko.srv3", srv3_min);
    touch(dir, "T [abcdefghijk].ko.json3", json3_min);
    touch(dir, "T [abcdefghijk].ko.vtt", "WEBVTT\n");
    touch(dir, "T [abcdefghijk].es-419.json3", json3_min);
    touch(dir, "T [abcdefghijk].es-419.vtt", "WEBVTT\n");
    touch(dir, "T [abcdefghijk].en.vtt", "WEBVTT\n");
    touch(dir, "T [abcdefghijk].en.srt", "1\n");
    touch(dir, "T [abcdefghijk].live_chat.json", "{}\n");
    touch(dir, "T [abcdefghijk].info.json", "{}");
    touch(dir, "T [abcdefghijk].webp", "");
    touch(dir, "T [abcdefghijk].en.mkv", ""); /* another video, not a language */
    touch(dir, "Other [abcdefghijk].ko.srv3", srv3_min);
    touch(dir, "T [abcdefghijkl].ko.srv3", srv3_min); /* a longer name is another video */

    char *video = g_build_filename(dir, "T [abcdefghijk].mp4", NULL);
    GPtrArray *a = kp_sidecars_find(video);
    CHECK_INT_EQ(a->len, 4); /* en, es-419, ko, chat */

    const kp_sidecar *ko = find(a, "ko");
    CHECK(ko && ko->kind == KP_SIDECAR_YT_SUB);
    CHECK_STR_EQ(ko->format, "srv3");
    const kp_sidecar *es = find(a, "es-419");
    CHECK(es && es->kind == KP_SIDECAR_YT_SUB);
    CHECK_STR_EQ(es->format, "json3");
    const kp_sidecar *en = find(a, "en");
    CHECK(en && en->kind == KP_SIDECAR_SUB);
    CHECK_STR_EQ(en->format, "srt");
    const kp_sidecar *chat = g_ptr_array_index(a, a->len - 1);
    CHECK(chat->kind == KP_SIDECAR_CHAT);
    CHECK_STR_HAS(chat->path, ".live_chat.json");

    g_ptr_array_free(a, TRUE);
    g_free(video);
    rm_rf(dir);
    g_free(dir);
}

TEST(plain_subtitle_without_language)
{
    char *dir = make_dir();
    touch(dir, "movie.mkv", "");
    touch(dir, "movie.srt", "1\n");
    touch(dir, "movie.notes.txt", "x");
    char *video = g_build_filename(dir, "movie.mkv", NULL);
    GPtrArray *a = kp_sidecars_find(video);
    CHECK_INT_EQ(a->len, 1);
    const kp_sidecar *sc = g_ptr_array_index(a, 0);
    CHECK_STR_EQ(sc->lang, "");
    CHECK_STR_EQ(sc->format, "srt");
    g_ptr_array_free(a, TRUE);
    g_free(video);

    /* No folder, no sidecars. */
    a = kp_sidecars_find("/nonexistent-kiplay-dir/x.mp4");
    CHECK_INT_EQ(a->len, 0);
    g_ptr_array_free(a, TRUE);
    rm_rf(dir);
    g_free(dir);
}

TEST(converts_to_a_cached_ass)
{
    char *dir = make_dir();
    char *cache = g_build_filename(dir, "cache", "subs", NULL);
    touch(dir, "T.ko.srv3", srv3_min);
    touch(dir, "T.es.json3", json3_min);
    touch(dir, "T.en.vtt", "WEBVTT\n");
    char *video = g_build_filename(dir, "T.mp4", NULL);
    GPtrArray *a = kp_sidecars_find(video);
    CHECK_INT_EQ(a->len, 3);

    kp_ytass_opts o;
    kp_ytass_opts_default(&o);
    for (guint i = 0; i < a->len; i++) {
        const kp_sidecar *sc = g_ptr_array_index(a, i);
        char *err = NULL;
        char *out = kp_sidecar_play_path(sc, &o, cache, &err);
        CHECK(out != NULL);
        if (sc->kind == KP_SIDECAR_SUB) {
            CHECK_STR_EQ(out, sc->path); /* plain: untouched */
        } else {
            CHECK(g_str_has_prefix(out, cache));
            char *want = g_strdup_printf(".%s.ass", sc->lang);
            CHECK(g_str_has_suffix(out, want));
            g_free(want);
            char *ass = NULL;
            CHECK(g_file_get_contents(out, &ass, NULL, NULL));
            CHECK_STR_HAS(ass, "Dialogue: 1,0:00:00.00,0:00:00.90,Text");
            g_free(ass);
            /* Same source and options: the same file, not regenerated. */
            char *again = kp_sidecar_play_path(sc, &o, cache, &err);
            CHECK_STR_EQ(again, out);
            g_free(again);
            /* Other options: another file. */
            kp_ytass_opts big = o;
            big.edge_scale = 3;
            char *other = kp_sidecar_play_path(sc, &big, cache, &err);
            CHECK(other && strcmp(other, out) != 0);
            g_free(other);
        }
        g_free(out);
    }
    g_ptr_array_free(a, TRUE);

    /* A broken file reports why and leaves nothing behind. */
    touch(dir, "B.xx.srv3", "<timedtext><body><p>");
    char *bad_video = g_build_filename(dir, "B.mp4", NULL);
    a = kp_sidecars_find(bad_video);
    CHECK_INT_EQ(a->len, 1);
    char *err = NULL;
    CHECK(kp_sidecar_play_path(g_ptr_array_index(a, 0), &o, cache, &err) == NULL);
    CHECK(err != NULL);
    g_free(err);
    g_ptr_array_free(a, TRUE);

    g_free(bad_video);
    g_free(video);
    g_free(cache);
    rm_rf(dir);
    g_free(dir);
}

int main(void)
{
    RUN(video_id_is_the_last_bracket);
    RUN(styled_beats_plain_per_language);
    RUN(plain_subtitle_without_language);
    RUN(converts_to_a_cached_ass);
    return harness_report();
}
