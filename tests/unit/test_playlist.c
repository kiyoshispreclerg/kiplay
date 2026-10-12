/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "../harness.h"
#include "playlist.h"
#include "plset.h"

TEST(parses_extended_m3u)
{
    const char *text = "\xEF\xBB\xBF#EXTM3U\r\n"
                       "#PLAYLIST:Jogos\r\n"
                       "#EXTINF:198,Artista - Música\r\n"
                       "/music/a.mp3\r\n"
                       "\r\n"
                       "# a comment\n"
                       "b.ogg\n"
                       "file:///music/game.nsf?3\n"
                       "#EXTINF:-1,Rádio\n"
                       "http://example.com/stream\n";
    kp_playlist *pl = kp_playlist_parse_m3u(text, "/lists", "fallback");
    CHECK_STR_EQ(kp_playlist_name(pl), "Jogos");
    CHECK_INT_EQ(kp_playlist_count(pl), 4);
    const kp_entry *e = kp_playlist_get(pl, 0);
    CHECK_STR_EQ(e->location, "/music/a.mp3");
    CHECK_STR_EQ(e->title, "Artista - Música");
    CHECK_INT_EQ(e->length_ms, 198000);
    e = kp_playlist_get(pl, 1);
    CHECK_STR_EQ(e->location, "/lists/b.ogg");
    CHECK(e->title == NULL);
    CHECK_INT_EQ(e->length_ms, -1);
    /* Subtunes keep Audacious's "?N". */
    CHECK_STR_EQ(kp_playlist_get(pl, 2)->location, "file:///music/game.nsf?3");
    e = kp_playlist_get(pl, 3);
    CHECK_STR_EQ(e->location, "http://example.com/stream");
    CHECK_INT_EQ(e->length_ms, -1);
    CHECK(kp_playlist_get(pl, 4) == NULL);
    /* Freshly loaded: nothing to save. */
    CHECK(!kp_playlist_dirty(pl));
    kp_playlist_free(pl);
}

TEST(name_defaults_without_header)
{
    kp_playlist *pl = kp_playlist_parse_m3u("/a.mp3\n", NULL, "3.m3u8");
    CHECK_STR_EQ(kp_playlist_name(pl), "3.m3u8");
    kp_playlist_free(pl);
}

TEST(dump_round_trips)
{
    kp_playlist *pl = kp_playlist_new("Two\nlines");
    kp_playlist_add(pl, "/a.mp3", "A", 61500);
    kp_playlist_add(pl, "/b.mp3", NULL, -1);
    CHECK(kp_playlist_dirty(pl));
    char *text = kp_playlist_dump(pl);
    CHECK_STR_EQ(text, "#EXTM3U\n#PLAYLIST:Two lines\n#EXTINF:61,A\n/a.mp3\n/b.mp3\n");
    kp_playlist *back = kp_playlist_parse_m3u(text, NULL, "x");
    CHECK_INT_EQ(kp_playlist_count(back), 2);
    CHECK_STR_EQ(kp_playlist_get(back, 0)->title, "A");
    g_free(text);
    kp_playlist_free(back);
    kp_playlist_free(pl);
}

TEST(remove_and_totals)
{
    kp_playlist *pl = kp_playlist_new("x");
    kp_playlist_add(pl, "/a", NULL, 1000);
    kp_playlist_add(pl, "/b", NULL, -1);
    kp_playlist_add(pl, "/c", NULL, 2000);
    size_t unknown;
    CHECK_INT_EQ(kp_playlist_total_ms(pl, &unknown), 3000);
    CHECK_INT_EQ(unknown, 1);
    kp_playlist_remove(pl, 0);
    CHECK_STR_EQ(kp_playlist_get(pl, 0)->location, "/b");
    CHECK_INT_EQ(kp_playlist_count(pl), 2);
    kp_playlist_clear(pl);
    CHECK_INT_EQ(kp_playlist_count(pl), 0);
    kp_playlist_free(pl);
}

TEST(info_from_the_player)
{
    kp_playlist *pl = kp_playlist_parse_m3u("#EXTINF:61,Old\n/a.mp3\n/b.mp3\n", NULL, "x");
    CHECK(!kp_playlist_dirty(pl));
    /* Same title, same second: nothing to save. */
    CHECK(!kp_playlist_set_info(pl, 0, "Old", 61400));
    CHECK(!kp_playlist_dirty(pl));
    CHECK(kp_playlist_set_info(pl, 1, NULL, 198556));
    CHECK_INT_EQ(kp_playlist_get(pl, 1)->length_ms, 198556);
    CHECK(kp_playlist_get(pl, 1)->title == NULL);
    CHECK(kp_playlist_set_info(pl, 0, "New", -1));
    CHECK_STR_EQ(kp_playlist_get(pl, 0)->title, "New");
    CHECK_INT_EQ(kp_playlist_get(pl, 0)->length_ms, 61000);
    CHECK(kp_playlist_dirty(pl));
    CHECK(!kp_playlist_set_info(pl, 9, "x", 1));
    kp_playlist_free(pl);
}

TEST(set_keeps_ids_order_and_files)
{
    char *dir = g_dir_make_tmp("kp-test-XXXXXX", NULL);
    CHECK(dir != NULL);

    kp_plset *s = kp_plset_load(dir, NULL);
    CHECK_INT_EQ(kp_plset_count(s), 0);
    kp_plset_insert(s, 0, "Um");
    kp_plset_insert(s, 1, "Dois");
    kp_plset_insert(s, 0, "Zero");
    CHECK_STR_EQ(kp_playlist_name(kp_plset_get(s, 0)), "Zero");
    char *order = kp_plset_order(s);
    CHECK_STR_EQ(order, "3,1,2");
    g_free(order);
    kp_playlist_add(kp_plset_get(s, 1), "/a.mp3", NULL, -1);
    CHECK_INT_EQ(kp_plset_save(s), 0);
    kp_plset_free(s);

    /* Empty playlists are written too, so the tab comes back. */
    s = kp_plset_load(dir, "3,1,2");
    CHECK_INT_EQ(kp_plset_count(s), 3);
    CHECK_INT_EQ(kp_plset_id(s, 0), 3);
    CHECK_STR_EQ(kp_playlist_name(kp_plset_get(s, 0)), "Zero");
    CHECK_INT_EQ(kp_playlist_count(kp_plset_get(s, 1)), 1);

    kp_plset_move(s, 0, 2);
    order = kp_plset_order(s);
    CHECK_STR_EQ(order, "1,2,3");
    g_free(order);

    kp_plset_remove(s, kp_plset_find(s, 2));
    CHECK_INT_EQ(kp_plset_count(s), 2);
    char *gone = g_build_filename(dir, "2.m3u8", NULL);
    CHECK(!g_file_test(gone, G_FILE_TEST_EXISTS));
    g_free(gone);
    /* A new id never reuses a live one. */
    size_t at = kp_plset_insert(s, 99, "Novo");
    CHECK_INT_EQ(at, 2);
    CHECK_INT_EQ(kp_plset_id(s, 2), 4);
    CHECK_INT_EQ(kp_plset_save(s), 0);
    kp_plset_free(s);

    /* Unknown ids in the order are ignored; the rest follow by id. */
    s = kp_plset_load(dir, "9,3");
    order = kp_plset_order(s);
    CHECK_STR_EQ(order, "3,1,4");
    g_free(order);
    for (size_t i = kp_plset_count(s); i-- > 0;)
        kp_plset_remove(s, i);
    kp_plset_free(s);
    g_rmdir(dir);
    g_free(dir);
}

int main(void)
{
    RUN(parses_extended_m3u);
    RUN(name_defaults_without_header);
    RUN(dump_round_trips);
    RUN(remove_and_totals);
    RUN(info_from_the_player);
    RUN(set_keeps_ids_order_and_files);
    return harness_report();
}
