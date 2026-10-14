/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

#include "../harness.h"
#include "filesettings.h"
#include "mpvopts.h"
#include "util.h"

static char *tmp_dir(void)
{
    return g_dir_make_tmp("kp-fs-XXXXXX", NULL);
}

/* rm -r, and frees the name. */
static void remove_tree(char *path)
{
    GDir *d = g_dir_open(path, 0, NULL);
    const char *name;
    while (d && (name = g_dir_read_name(d)))
        remove_tree(g_build_filename(path, name, NULL));
    if (d) {
        g_dir_close(d);
        g_rmdir(path);
    } else {
        g_remove(path);
    }
    g_free(path);
}

static char *write_file(const char *dir, const char *name, const char *text)
{
    char *path = g_build_filename(dir, name, NULL);
    g_file_set_contents(path, text, -1, NULL);
    return path;
}

TEST(round_trip)
{
    char *dir = tmp_dir();
    char *store = g_build_filename(dir, "files", NULL);
    char *media = write_file(dir, "vídeo [abc].mkv", "not really a video");

    kp_file_settings fs;
    CHECK(!kp_file_settings_load(store, media, &fs));
    CHECK(kp_file_settings_empty(&fs));
    CHECK_INT_EQ(fs.aid, -1);

    fs.position = 754.25;
    fs.aid = 2;
    fs.sid = 0;
    fs.audio_delay = -0.1;
    fs.sub_delay = 0.3;
    g_strlcpy(fs.aspect, "16:9", sizeof fs.aspect);
    CHECK_INT_EQ(kp_file_settings_save(store, media, &fs), 0);

    kp_file_settings back;
    CHECK(kp_file_settings_load(store, media, &back));
    CHECK(fabs(back.position - 754.25) < 1e-6);
    CHECK_INT_EQ(back.aid, 2);
    CHECK_INT_EQ(back.sid, 0);
    CHECK(fabs(back.audio_delay + 0.1) < 1e-6);
    CHECK(fabs(back.sub_delay - 0.3) < 1e-6);
    CHECK_STR_EQ(back.aspect, "16:9");

    /* Under <2 hex>/<sha1>.ini, with the location for whoever looks. */
    char *path = kp_file_settings_path(store, media);
    CHECK(path && g_str_has_prefix(path, store));
    char *base = g_path_get_basename(path);
    CHECK_INT_EQ(strlen(base), 40 + 4);
    char *text = NULL;
    CHECK(g_file_get_contents(path, &text, NULL, NULL));
    CHECK(strstr(text, "location = ") && strstr(text, "vídeo [abc].mkv"));

    /* Saving nothing removes the file. */
    kp_file_settings empty = KP_FILE_SETTINGS_INIT;
    CHECK_INT_EQ(kp_file_settings_save(store, media, &empty), 0);
    CHECK(!g_file_test(path, G_FILE_TEST_EXISTS));
    CHECK(!kp_file_settings_load(store, media, &back));

    g_free(text);
    g_free(base);
    g_free(path);
    g_free(media);
    g_free(store);
    remove_tree(dir);
}

TEST(another_file_at_the_same_path_starts_fresh)
{
    char *dir = tmp_dir();
    char *media = write_file(dir, "a.mp4", "12345");
    kp_file_settings fs = KP_FILE_SETTINGS_INIT;
    fs.position = 30;
    CHECK_INT_EQ(kp_file_settings_save(dir, media, &fs), 0);
    char *before = kp_file_settings_path(dir, media);
    /* Re-downloaded: another size, another key. */
    g_file_set_contents(media, "1234567890", -1, NULL);
    char *after = kp_file_settings_path(dir, media);
    CHECK(strcmp(before, after) != 0);
    kp_file_settings back;
    CHECK(!kp_file_settings_load(dir, media, &back));
    g_free(before);
    g_free(after);
    g_free(media);
    remove_tree(dir);
}

TEST(only_local_files)
{
    char *dir = tmp_dir();
    char *media = write_file(dir, "jogo.nsf", "NESM");
    kp_file_settings fs = KP_FILE_SETTINGS_INIT;
    CHECK(kp_file_settings_path(dir, "https://example.com/v.mp4") == NULL);
    CHECK(kp_file_settings_path(dir, "/no/such/file.mkv") == NULL);
    CHECK(kp_file_settings_save(dir, "https://example.com/v.mp4", &fs) < 0);
    /* A subtune has its own settings, from the file's size. */
    char *uri = g_filename_to_uri(media, NULL, NULL);
    char *sub3 = g_strconcat(uri, "?3", NULL), *sub4 = g_strconcat(uri, "?4", NULL);
    char *p3 = kp_file_settings_path(dir, sub3), *p4 = kp_file_settings_path(dir, sub4);
    CHECK(p3 && p4 && strcmp(p3, p4) != 0);
    g_free(p3);
    g_free(p4);
    g_free(sub3);
    g_free(sub4);
    g_free(uri);
    g_free(media);
    remove_tree(dir);
}

TEST(broken_values_are_ignored)
{
    char *dir = tmp_dir();
    char *media = write_file(dir, "b.mkv", "x");
    char *path = kp_file_settings_path(dir, media);
    kp_make_parent_dirs(path);
    g_file_set_contents(path, "[file]\nposition = nan\naid = -7\nsid = abc\nsub_delay = 1e999\n",
                        -1, NULL);
    kp_file_settings fs;
    CHECK(kp_file_settings_load(dir, media, &fs));
    CHECK(fs.position == 0);
    CHECK_INT_EQ(fs.aid, -1);
    CHECK_INT_EQ(fs.sid, -1);
    CHECK(fs.sub_delay == 0);
    g_free(path);
    g_free(media);
    remove_tree(dir);
}

TEST(mpv_options_from_the_prefs)
{
    char *dir = tmp_dir();
    char *ini = write_file(dir, "kiplay.ini",
                           "[video]\nhwdec = auto-safe\nvo =\n"
                           "[audio]\nlanguages = ja, en\n"
                           "[subtitles]\nlanguages = pt-BR,pt\nfont = DejaVu Sans\n"
                           "font_size = 48\nauto = fuzzy\n"
                           "[mpv]\nextra = --deband\n");
    kp_prefs *p = kp_prefs_load(ini);
    char **args = kp_mpv_pref_args(p);
    char *joined = g_strjoinv(" | ", args);
    CHECK_STR_EQ(joined, "--hwdec=auto-safe | --alang=ja,en | --slang=pt-BR,pt | "
                         "--sub-font=DejaVu Sans | --sub-font-size=48 | --sub-auto=fuzzy");
    char *sig = kp_mpv_pref_signature(p);
    kp_prefs_set_str(p, "mpv", "extra", "--deband --dither");
    char *sig2 = kp_mpv_pref_signature(p);
    CHECK(strcmp(sig, sig2) != 0);
    /* Nothing set: nothing passed (mpv's own defaults, or mpv.conf's). */
    kp_prefs *empty = kp_prefs_load("/nonexistent/kiplay.ini");
    char **none = kp_mpv_pref_args(empty);
    CHECK(none[0] == NULL);
    g_strfreev(none);
    kp_prefs_free(empty);
    g_free(sig);
    g_free(sig2);
    g_free(joined);
    g_strfreev(args);
    kp_prefs_free(p);
    g_free(ini);
    remove_tree(dir);
}

int main(void)
{
    RUN(round_trip);
    RUN(another_file_at_the_same_path_starts_fresh);
    RUN(only_local_files);
    RUN(broken_values_are_ignored);
    RUN(mpv_options_from_the_prefs);
    return harness_report();
}
