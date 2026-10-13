/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <glib.h>

#include "../harness.h"
#include "media.h"
#include "search.h"

TEST(kinds_by_extension)
{
    CHECK_INT_EQ(kp_media_kind_of("/m/Song.MP3"), KP_MEDIA_AUDIO);
    CHECK_INT_EQ(kp_media_kind_of("file:///m/Jogo%20X.nsf?12"), KP_MEDIA_AUDIO);
    CHECK_INT_EQ(kp_media_kind_of("/m/clip.webm"), KP_MEDIA_VIDEO);
    CHECK_INT_EQ(kp_media_kind_of("/m/song.mid"), KP_MEDIA_VIDEO);
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

int main(void)
{
    RUN(kinds_by_extension);
    RUN(search_words_anywhere);
    return harness_report();
}
