/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <glib.h>
#include <glib/gstdio.h>

#include "../harness.h"
#include "media.h"
#include "search.h"

TEST(kinds_by_extension)
{
    CHECK_INT_EQ(kp_media_kind_of("/m/Song.MP3"), KP_MEDIA_AUDIO);
    CHECK_INT_EQ(kp_media_kind_of("file:///m/Jogo%20X.nsf?12"), KP_MEDIA_AUDIO);
    CHECK_INT_EQ(kp_media_kind_of("/m/clip.webm"), KP_MEDIA_VIDEO);
    CHECK_INT_EQ(kp_media_kind_of("/m/song.mid"), KP_MEDIA_AUDIO);
    CHECK_INT_EQ(kp_media_kind_of("/m/song.mod"), KP_MEDIA_VIDEO);
    CHECK_INT_EQ(kp_media_kind_of("/m/cover.jpg"), KP_MEDIA_NONE);
    CHECK_INT_EQ(kp_media_kind_of("/m.mp3/README"), KP_MEDIA_NONE);
    CHECK_INT_EQ(kp_media_kind_of("http://radio/stream"), KP_MEDIA_NONE);
    CHECK(kp_media_is_playlist("/a/list.M3U8"));
    CHECK(kp_media_is_playlist("/a/radio.pls"));
    CHECK(!kp_media_is_playlist("/a/list.m3u8.mp3"));
}

TEST(search_words_anywhere)
{
    kp_entry e = {.location = "file:///m/The%20Beatles/08%20-%20Help.mp3",
                  .title = "Help!",
                  .artist = "The Beatles",
                  .album = "Help",
                  .length_ms = -1};
    char *hay = kp_search_haystack(&e);
    char **w = kp_search_words("  beat   08 ");
    CHECK(w[0] && w[1] && !w[2]);
    CHECK(kp_search_match(hay, w));
    g_strfreev(w);
    w = kp_search_words("HELP stones");
    CHECK(!kp_search_match(hay, w));
    g_strfreev(w);
    w = kp_search_words("");
    CHECK(w[0] == NULL);
    CHECK(kp_search_match(hay, w));
    g_strfreev(w);
    g_free(hay);

    /* Accents typed either way, any case. */
    kp_entry m = {.location = "/m/Ma\xcc\x81sica.ogg", .length_ms = -1};
    hay = kp_search_haystack(&m);
    w = kp_search_words("M\xc3\x81SICA");
    CHECK(kp_search_match(hay, w));
    g_strfreev(w);
    g_free(hay);
}

TEST(subtunes_become_uris)
{
    char *dir = g_dir_make_tmp("kp-test-XXXXXX", NULL);
    char *game = g_build_filename(dir, "Jogo X.nsf", NULL);
    char *odd = g_build_filename(dir, "x?3", NULL);
    g_file_set_contents(game, "", 0, NULL);
    g_file_set_contents(odd, "", 0, NULL);
    char *uri = g_filename_to_uri(game, NULL, NULL);
    char *want = g_strdup_printf("%s?55", uri);
    /* From a URI (what file managers and kiplay ARG send) and a path. */
    char *in = g_strdup_printf("%s?55", uri);
    char *got = kp_media_location(in);
    CHECK_STR_EQ(got, want);
    g_free(got);
    g_free(in);
    in = g_strdup_printf("%s?55", game);
    got = kp_media_location(in);
    CHECK_STR_EQ(got, want);
    g_free(got);
    g_free(in);
    /* Plain files stay paths; a file really named "x?3" too. */
    got = kp_media_location(uri);
    CHECK_STR_EQ(got, game);
    g_free(got);
    got = kp_media_location(odd);
    CHECK_STR_EQ(got, odd);
    g_free(got);
    got = kp_media_location("http://radio/x?1");
    CHECK_STR_EQ(got, "http://radio/x?1");
    g_free(got);
    g_unlink(game);
    g_unlink(odd);
    g_rmdir(dir);
    g_free(want);
    g_free(uri);
    g_free(game);
    g_free(odd);
    g_free(dir);
}

int main(void)
{
    RUN(kinds_by_extension);
    RUN(search_words_anywhere);
    RUN(subtunes_become_uris);
    return harness_report();
}
