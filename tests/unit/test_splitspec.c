/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <glib.h>
#include <glib/gstdio.h>
#include <locale.h>
#include <math.h>

#include "../harness.h"
#include "splitspec.h"

#define PIECE(a, i) (&g_array_index((a), kp_split_piece, (i)))
#define NEAR(a, b) (fabs((a) - (b)) < 1e-6)

TEST(parses_description_style)
{
    const char *text = "Tracklist:\n"
                       "0:00 Intro\n"
                       "3:15 - Second Song\n"
                       "[7:02] Third | live\n"
                       "\n"
                       "Thanks for watching! 1.000.000 views\n"
                       "01:02:03.5 – The Long One\r\n";
    kp_split_result r;
    GArray *a = kp_split_parse(text, 4000, &r);
    CHECK_INT_EQ(r.status, KP_SPLIT_OK);
    CHECK(!r.as_durations);
    CHECK_INT_EQ(a->len, 4);
    CHECK(NEAR(PIECE(a, 0)->start, 0) && NEAR(PIECE(a, 0)->end, 195));
    CHECK_STR_EQ(PIECE(a, 0)->text, "Intro");
    CHECK_INT_EQ(PIECE(a, 0)->line, 2);
    CHECK_STR_EQ(PIECE(a, 1)->text, "Second Song");
    CHECK(NEAR(PIECE(a, 2)->start, 422));
    CHECK_STR_EQ(PIECE(a, 2)->text, "Third | live");
    CHECK(NEAR(PIECE(a, 3)->start, 3723.5) && NEAR(PIECE(a, 3)->end, 4000));
    CHECK_STR_EQ(PIECE(a, 3)->text, "The Long One");
    g_array_unref(a);
}

TEST(title_before_time_and_numbers)
{
    kp_split_result r;
    GArray *a = kp_split_parse("01. First - 0:00\n2) Second (2:30)\n#3 Third 5:00\n"
                               "04 - Fourth 7:00\n99 Luftballons 9:00\n",
                               600, &r);
    CHECK_INT_EQ(r.status, KP_SPLIT_OK);
    CHECK(!r.as_durations);
    CHECK_INT_EQ(a->len, 5);
    CHECK_STR_EQ(PIECE(a, 0)->text, "First");
    CHECK_STR_EQ(PIECE(a, 1)->text, "Second");
    CHECK_STR_EQ(PIECE(a, 2)->text, "Third");
    CHECK_STR_EQ(PIECE(a, 3)->text, "Fourth");
    CHECK_STR_EQ(PIECE(a, 4)->text, "99 Luftballons");
    g_array_unref(a);
}

TEST(ranges_keep_the_start)
{
    kp_split_result r;
    GArray *a = kp_split_parse("0:00 - 2:00 A\n2:00 - 4:30 B\n", 300, &r);
    CHECK_INT_EQ(r.status, KP_SPLIT_OK);
    CHECK_INT_EQ(a->len, 2);
    CHECK_STR_EQ(PIECE(a, 0)->text, "A");
    CHECK(NEAR(PIECE(a, 1)->start, 120) && NEAR(PIECE(a, 1)->end, 300));
    CHECK_STR_EQ(PIECE(a, 1)->text, "B");
    g_array_unref(a);
}

TEST(audio_before_the_first_time_is_a_piece)
{
    kp_split_result r;
    GArray *a = kp_split_parse("0:30 Song\n2:00 Other\n", 300, &r);
    CHECK_INT_EQ(r.status, KP_SPLIT_OK);
    CHECK_INT_EQ(a->len, 3);
    CHECK(NEAR(PIECE(a, 0)->start, 0) && NEAR(PIECE(a, 0)->end, 30));
    CHECK_STR_EQ(PIECE(a, 0)->text, "");
    CHECK_INT_EQ(PIECE(a, 0)->line, 0);
    CHECK_STR_EQ(PIECE(a, 1)->text, "Song");
    g_array_unref(a);
    /* Under a second: that is the start. */
    a = kp_split_parse("0:00.5 Song\n2:00 Other\n", 300, &r);
    CHECK_INT_EQ(a->len, 2);
    CHECK(NEAR(PIECE(a, 0)->start, 0));
    g_array_unref(a);
}

TEST(track_list_of_lengths)
{
    /* 3:45 + 4:10 + 2:50 = 645 s; the file has 646. */
    kp_split_result r;
    GArray *a = kp_split_parse("1. One (3:45)\n2. Two (4:10)\n3. Three (2:50)\n", 646, &r);
    CHECK_INT_EQ(r.status, KP_SPLIT_OK);
    CHECK(r.as_durations);
    CHECK_INT_EQ(a->len, 3);
    CHECK(NEAR(PIECE(a, 0)->start, 0) && NEAR(PIECE(a, 0)->end, 225));
    CHECK(NEAR(PIECE(a, 1)->start, 225) && NEAR(PIECE(a, 1)->end, 475));
    CHECK(NEAR(PIECE(a, 2)->start, 475) && NEAR(PIECE(a, 2)->end, 646));
    CHECK_STR_EQ(PIECE(a, 2)->text, "Three");
    g_array_unref(a);
    /* The same times in a long file are starts. */
    a = kp_split_parse("One 3:45\nTwo 4:10\nThree 2:50\n", 3000, &r);
    CHECK_INT_EQ(r.status, KP_SPLIT_UNORDERED);
    CHECK_INT_EQ(r.line, 3);
    CHECK_INT_EQ(a->len, 0);
    g_array_unref(a);
}

TEST(reports_problems)
{
    kp_split_result r;
    GArray *a = kp_split_parse("no times here\n", 100, &r);
    CHECK_INT_EQ(r.status, KP_SPLIT_EMPTY);
    g_array_unref(a);
    a = kp_split_parse("0:00 A\n0:00 B\n", 100, &r);
    CHECK_INT_EQ(r.status, KP_SPLIT_UNORDERED);
    CHECK_INT_EQ(r.line, 2);
    g_array_unref(a);
    a = kp_split_parse("0:00 A\n\n2:00 B\n", 100, &r);
    CHECK_INT_EQ(r.status, KP_SPLIT_PAST_END);
    CHECK_INT_EQ(r.line, 3);
    g_array_unref(a);
    /* Not times: seconds over 59, four parts, glued to digits. */
    a = kp_split_parse("1:75 x\n1:02:03:04 y\n12:345 z\nv1:30 w\n", 100, &r);
    CHECK_INT_EQ(r.status, KP_SPLIT_EMPTY);
    g_array_unref(a);
    /* Unknown length: the last piece goes to the end. */
    a = kp_split_parse("0:00 A\n10:00 B\n", 0, &r);
    CHECK_INT_EQ(r.status, KP_SPLIT_OK);
    CHECK(PIECE(a, 1)->end < 0);
    g_array_unref(a);
}

TEST(splits_artist_and_title)
{
    char *a, *t;
    CHECK(kp_split_artist_title("Band - Song - Live", &a, &t));
    CHECK_STR_EQ(a, "Band");
    CHECK_STR_EQ(t, "Song - Live");
    g_free(a);
    g_free(t);
    CHECK(kp_split_artist_title("Banda \xE2\x80\x93 Canção", &a, &t));
    CHECK_STR_EQ(t, "Canção");
    g_free(a);
    g_free(t);
    CHECK(!kp_split_artist_title("Spider-Man Theme", &a, &t));
    CHECK(!kp_split_artist_title(" - Only", &a, &t));
}

TEST(makes_file_names)
{
    char *n = kp_split_file_name(3, 12, "AC/DC: Live", "mp3");
    CHECK_STR_EQ(n, "03 - AC-DC: Live.mp3");
    g_free(n);
    n = kp_split_file_name(7, 150, "", "flac");
    CHECK_STR_EQ(n, "007.flac");
    g_free(n);
    n = kp_split_file_name(1, 2, "...hidden\x01", "ogg");
    CHECK_STR_EQ(n, "01 - hidden.ogg");
    g_free(n);
    GString *longt = g_string_new(NULL);
    for (int i = 0; i < 120; i++)
        g_string_append(longt, "é");
    n = kp_split_file_name(1, 2, longt->str, "opus");
    CHECK(strlen(n) <= 210);
    CHECK(g_utf8_validate(n, -1, NULL));
    CHECK(g_str_has_suffix(n, "é.opus"));
    g_free(n);
    g_string_free(longt, TRUE);
}

TEST(formats_times)
{
    char *t = kp_split_format_time(75);
    CHECK_STR_EQ(t, "1:15");
    g_free(t);
    t = kp_split_format_time(3723.5);
    CHECK_STR_EQ(t, "1:02:03.5");
    g_free(t);
    t = kp_split_format_time(0.25);
    CHECK_STR_EQ(t, "0:00.25");
    g_free(t);
}

TEST(text_from_chapters_and_sidecars)
{
    double starts[] = {0, 61.5};
    char *titles[] = {"Um", "Dois"};
    char *t = kp_split_text_for("/x/a.m4a", starts, titles, 2);
    CHECK_STR_EQ(t, "0:00 Um\n1:01.5 Dois\n");
    g_free(t);

    char *dir = g_dir_make_tmp("kp-test-XXXXXX", NULL);
    char *media = g_build_filename(dir, "Live [abc].opus", NULL);
    char *info = g_build_filename(dir, "Live [abc].info.json", NULL);
    char *desc = g_build_filename(dir, "Live [abc].description", NULL);
    CHECK(kp_split_text_for(media, NULL, NULL, 0) == NULL);
    g_file_set_contents(desc, "Setlist\n0:00 A\n4:00 B\n", -1, NULL);
    t = kp_split_text_for(media, NULL, NULL, 0);
    CHECK_STR_EQ(t, "Setlist\n0:00 A\n4:00 B\n");
    g_free(t);
    g_file_set_contents(info, "{\"description\": \"no times\", \"chapters\": "
                              "[{\"start_time\": 0.0, \"title\": \"Abertura\"},"
                              " {\"start_time\": 90.5, \"title\": \"Fim\"}]}", -1, NULL);
    t = kp_split_text_for(media, NULL, NULL, 0);
    CHECK_STR_EQ(t, "0:00 Abertura\n1:30.5 Fim\n");
    g_free(t);
    g_file_set_contents(info, "{\"description\": \"no times\"}", -1, NULL);
    t = kp_split_text_for(media, NULL, NULL, 0);
    CHECK_STR_EQ(t, "Setlist\n0:00 A\n4:00 B\n");
    g_free(t);
    g_remove(info);
    g_remove(desc);
    g_rmdir(dir);
    g_free(media);
    g_free(info);
    g_free(desc);
    g_free(dir);
}

int main(void)
{
    /* Decimals with a comma, where the system has such a locale: the
     * info.json's numbers must still read. */
    if (!setlocale(LC_ALL, "pt_BR.UTF-8"))
        setlocale(LC_ALL, "de_DE.UTF-8");
    RUN(parses_description_style);
    RUN(title_before_time_and_numbers);
    RUN(ranges_keep_the_start);
    RUN(audio_before_the_first_time_is_a_piece);
    RUN(track_list_of_lengths);
    RUN(reports_problems);
    RUN(splits_artist_and_title);
    RUN(makes_file_names);
    RUN(formats_times);
    RUN(text_from_chapters_and_sidecars);
    return harness_report();
}
