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
    kp_tags a = {"A", NULL, NULL, 61500};
    kp_playlist_add(pl, "/a.mp3", &a);
    kp_playlist_add(pl, "/b.mp3", NULL);
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

TEST(type_round_trips)
{
    kp_playlist *pl = kp_playlist_new("x");
    CHECK_INT_EQ(kp_playlist_get_type(pl), KP_PLAYLIST_MIXED);
    kp_playlist_mark_saved(pl);
    kp_playlist_set_type(pl, KP_PLAYLIST_AUDIO);
    CHECK(kp_playlist_dirty(pl));
    kp_playlist_add(pl, "/a.mkv", NULL);
    char *text = kp_playlist_dump(pl);
    CHECK_STR_EQ(text, "#EXTM3U\n#PLAYLIST:x\n#KIPLAY-TYPE:audio\n/a.mkv\n");
    kp_playlist *back = kp_playlist_parse_m3u(text, NULL, "y");
    CHECK_INT_EQ(kp_playlist_get_type(back), KP_PLAYLIST_AUDIO);
    CHECK(!kp_playlist_dirty(back));
    g_free(text);
    kp_playlist_free(back);
    /* Without the line (every older file) it is mixed. */
    back = kp_playlist_parse_m3u("#EXTM3U\n/a.mp3\n", NULL, "y");
    CHECK_INT_EQ(kp_playlist_get_type(back), KP_PLAYLIST_MIXED);
    kp_playlist_free(back);
    kp_playlist_free(pl);
}

TEST(remove_and_totals)
{
    kp_playlist *pl = kp_playlist_new("x");
    kp_playlist_add(pl, "/a", &(kp_tags){NULL, NULL, NULL, 1000});
    kp_playlist_add(pl, "/b", NULL);
    kp_playlist_add(pl, "/c", &(kp_tags){NULL, NULL, NULL, 2000});
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

TEST(tags_from_the_reader)
{
    kp_playlist *pl = kp_playlist_parse_m3u("#EXTINF:61,Old\n/a.mp3\n/b.mp3\n", NULL, "x");
    CHECK(!kp_playlist_dirty(pl));
    /* Same title, same second: nothing to save. */
    CHECK(!kp_playlist_set_tags(pl, 0, &(kp_tags){"Old", NULL, NULL, 61400}));
    CHECK(!kp_playlist_dirty(pl));
    CHECK(kp_playlist_set_tags(pl, 1, &(kp_tags){NULL, "ignored", NULL, 198556}));
    const kp_entry *e = kp_playlist_get(pl, 1);
    CHECK_INT_EQ(e->length_ms, 198556);
    CHECK(e->title == NULL && e->artist == NULL);
    CHECK(kp_playlist_set_tags(pl, 0, &(kp_tags){"New", "Artist", "", -1}));
    e = kp_playlist_get(pl, 0);
    CHECK_STR_EQ(e->title, "New");
    CHECK_STR_EQ(e->artist, "Artist");
    CHECK(e->album == NULL);
    CHECK_INT_EQ(e->length_ms, 61000);
    CHECK(kp_playlist_dirty(pl));
    /* A new title without an artist clears the old artist. */
    CHECK(kp_playlist_set_tags(pl, 0, &(kp_tags){"New", NULL, NULL, -1}));
    CHECK(kp_playlist_get(pl, 0)->artist == NULL);
    CHECK(!kp_playlist_set_tags(pl, 9, &(kp_tags){"x", NULL, NULL, 1}));
    CHECK(!kp_playlist_get(pl, 1)->scanned);
    kp_playlist_set_scanned(pl, 1);
    CHECK(kp_playlist_get(pl, 1)->scanned);
    kp_playlist_free(pl);
}

TEST(artist_and_album_round_trip)
{
    kp_playlist *pl = kp_playlist_new("x");
    kp_playlist_add(pl, "/a.mp3", &(kp_tags){"Song", "Band", "Record", 200000});
    kp_playlist_add(pl, "/b.mp3", &(kp_tags){"Only - title", NULL, NULL, -1});
    char *text = kp_playlist_dump(pl);
    CHECK_STR_EQ(text, "#EXTM3U\n#PLAYLIST:x\n"
                       "#EXTINF:200,Band - Song\n#EXTART:Band\n#EXTALB:Record\n/a.mp3\n"
                       "#EXTINF:-1,Only - title\n/b.mp3\n");
    kp_playlist *back = kp_playlist_parse_m3u(text, NULL, "x");
    const kp_entry *e = kp_playlist_get(back, 0);
    CHECK_STR_EQ(e->title, "Song");
    CHECK_STR_EQ(e->artist, "Band");
    CHECK_STR_EQ(e->album, "Record");
    e = kp_playlist_get(back, 1);
    CHECK_STR_EQ(e->title, "Only - title");
    CHECK(e->artist == NULL);
    g_free(text);
    kp_playlist_free(back);
    kp_playlist_free(pl);

    /* #EXTART before #EXTINF, and an EXTINF that doesn't start with it. */
    pl = kp_playlist_parse_m3u("#EXTART:A\n#EXTINF:1,Other - B\n/x\n", NULL, "x");
    CHECK_STR_EQ(kp_playlist_get(pl, 0)->title, "Other - B");
    CHECK_STR_EQ(kp_playlist_get(pl, 0)->artist, "A");
    kp_playlist_free(pl);
}

TEST(insert_and_move)
{
    kp_playlist *pl = kp_playlist_new("x");
    const char *names[] = {"/0", "/1", "/2", "/3", "/4"};
    for (int i = 0; i < 5; i++)
        kp_playlist_add(pl, names[i], NULL);
    kp_playlist_insert(pl, 1, "/new", NULL);
    CHECK_STR_EQ(kp_playlist_get(pl, 1)->location, "/new");
    CHECK_STR_EQ(kp_playlist_get(pl, 2)->location, "/1");
    kp_playlist_remove(pl, 1);
    kp_playlist_insert(pl, 99, "/end", NULL);
    CHECK_STR_EQ(kp_playlist_get(pl, 5)->location, "/end");
    kp_playlist_remove(pl, 5);

    /* /1 and /3 dropped on /0: 1 3 0 2 4. */
    int order[5];
    size_t rows[] = {1, 3};
    CHECK_INT_EQ(kp_playlist_move(pl, rows, 2, 0, order), 0);
    const char *want[] = {"/1", "/3", "/0", "/2", "/4"};
    for (int i = 0; i < 5; i++)
        CHECK_STR_EQ(kp_playlist_get(pl, (size_t)i)->location, want[i]);
    CHECK(order[0] == 1 && order[1] == 3 && order[2] == 0 && order[3] == 2 && order[4] == 4);
    /* The first two to the end: 0 2 4 1 3. */
    size_t first[] = {0, 1};
    CHECK_INT_EQ(kp_playlist_move(pl, first, 2, 5, NULL), 3);
    const char *want2[] = {"/0", "/2", "/4", "/1", "/3"};
    for (int i = 0; i < 5; i++)
        CHECK_STR_EQ(kp_playlist_get(pl, (size_t)i)->location, want2[i]);
    /* Dropped among themselves: nothing moves. */
    kp_playlist_mark_saved(pl);
    size_t mid[] = {1, 2};
    CHECK_INT_EQ(kp_playlist_move(pl, mid, 2, 2, NULL), 1);
    CHECK(!kp_playlist_dirty(pl));
    kp_playlist_free(pl);
}

TEST(remove_rows_and_follow_entries)
{
    kp_playlist *pl = kp_playlist_new("x");
    const char *names[] = {"/0", "/1", "/2", "/1", "/4"};
    for (int i = 0; i < 5; i++)
        kp_playlist_add(pl, names[i], NULL);
    /* Same text, different entries: each one is found by its own string. */
    const char *second = kp_playlist_get(pl, 3)->location;
    CHECK_INT_EQ(kp_playlist_find_entry(pl, second, 1), 3);
    CHECK_INT_EQ(kp_playlist_find_entry(pl, second, 3), 3);
    CHECK_INT_EQ(kp_playlist_find_entry(pl, "/1", -1), -1);
    size_t rows[] = {0, 2, 4};
    kp_playlist_remove_rows(pl, rows, 3);
    CHECK_INT_EQ(kp_playlist_count(pl), 2);
    CHECK_INT_EQ(kp_playlist_find_entry(pl, second, 3), 1);
    CHECK_STR_EQ(kp_playlist_get(pl, 0)->location, "/1");
    kp_playlist_free(pl);
}

TEST(pls_round_trip)
{
    const char *text = "[playlist]\r\n"
                       "File2=http://radio/stream\r\n"
                       "Title2=Radio\r\n"
                       "Length2=-1\r\n"
                       "File1=a.mp3\r\n"
                       "Title1=Band - Song\r\n"
                       "Length1=200\r\n"
                       "NumberOfEntries=2\r\nVersion=2\r\n";
    kp_playlist *pl = kp_playlist_parse_pls(text, "/music", "list.pls");
    CHECK_STR_EQ(kp_playlist_name(pl), "list.pls");
    CHECK_INT_EQ(kp_playlist_count(pl), 2);
    CHECK_STR_EQ(kp_playlist_get(pl, 0)->location, "/music/a.mp3");
    CHECK_STR_EQ(kp_playlist_get(pl, 0)->title, "Band - Song");
    CHECK_INT_EQ(kp_playlist_get(pl, 0)->length_ms, 200000);
    CHECK_STR_EQ(kp_playlist_get(pl, 1)->location, "http://radio/stream");
    CHECK_INT_EQ(kp_playlist_get(pl, 1)->length_ms, -1);
    char *out = kp_playlist_dump_pls(pl);
    CHECK_STR_EQ(out, "[playlist]\nX-GNOME-Title=list.pls\n"
                      "File1=/music/a.mp3\nTitle1=Band - Song\nLength1=200\n"
                      "File2=http://radio/stream\nTitle2=Radio\nLength2=-1\n"
                      "NumberOfEntries=2\nVersion=2\n");
    kp_playlist *back = kp_playlist_parse_pls(out, NULL, "x");
    CHECK_STR_EQ(kp_playlist_name(back), "list.pls");
    CHECK_INT_EQ(kp_playlist_count(back), 2);
    g_free(out);
    kp_playlist_free(back);
    kp_playlist_free(pl);
}

TEST(load_finds_the_format_and_encoding)
{
    char *dir = g_dir_make_tmp("kp-test-XXXXXX", NULL);
    CHECK(dir != NULL);
    char *pls = g_build_filename(dir, "x.m3u", NULL);
    /* PLS behind a misleading name; and "Música" in Windows-1252. */
    CHECK(g_file_set_contents(pls, "\n[Playlist]\nFile1=/a.mp3\n", -1, NULL));
    kp_playlist *pl = kp_playlist_load(pls, "x");
    CHECK_INT_EQ(kp_playlist_count(pl), 1);
    kp_playlist_free(pl);
    CHECK(g_file_set_contents(pls, "/M\xfasica/a.mp3\n", -1, NULL));
    pl = kp_playlist_load(pls, "x");
    CHECK_STR_EQ(kp_playlist_get(pl, 0)->location, "/M\xc3\xbasica/a.mp3");
    kp_playlist_free(pl);
    /* Saving by extension. */
    char *out = g_build_filename(dir, "y.pls", NULL);
    kp_playlist *y = kp_playlist_new("y");
    CHECK_INT_EQ(kp_playlist_save(y, out), 0);
    kp_playlist_free(y);
    char *text = NULL;
    CHECK(g_file_get_contents(out, &text, NULL, NULL));
    CHECK(g_str_has_prefix(text, "[playlist]\n"));
    g_free(text);
    g_unlink(pls);
    g_unlink(out);
    g_rmdir(dir);
    g_free(pls);
    g_free(out);
    g_free(dir);
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
    kp_playlist_add(kp_plset_get(s, 1), "/a.mp3", NULL);
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
    RUN(type_round_trips);
    RUN(remove_and_totals);
    RUN(tags_from_the_reader);
    RUN(artist_and_album_round_trip);
    RUN(insert_and_move);
    RUN(remove_rows_and_follow_entries);
    RUN(pls_round_trip);
    RUN(load_finds_the_format_and_encoding);
    RUN(set_keeps_ids_order_and_files);
    return harness_report();
}
