/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../harness.h"
#include "ini.h"
#include "util.h"

TEST(pathf_fails_instead_of_truncating)
{
    char buf[8];
    CHECK(kp_pathf(buf, sizeof buf, "%s", "1234567"));
    CHECK_STR_EQ(buf, "1234567");
    CHECK(!kp_pathf(buf, sizeof buf, "%s", "12345678"));
    CHECK_STR_EQ(buf, "");
}

TEST(durations_like_players)
{
    char s[32];
    kp_format_duration(s, sizeof s, 0);
    CHECK_STR_EQ(s, "0:00");
    kp_format_duration(s, sizeof s, 198556);
    CHECK_STR_EQ(s, "3:18");
    kp_format_duration(s, sizeof s, 3723000);
    CHECK_STR_EQ(s, "1:02:03");
    kp_format_duration(s, sizeof s, -1);
    CHECK_STR_EQ(s, "--:--");
}

TEST(config_path_follows_xdg)
{
    char p[KP_PATH_MAX];
    const char *old_xdg = getenv("XDG_CONFIG_HOME");
    char *saved = old_xdg ? strdup(old_xdg) : NULL;

    setenv("XDG_CONFIG_HOME", "/x/cfg", 1);
    CHECK(kp_config_path(p, sizeof p, "kiplay.ini"));
    CHECK_STR_EQ(p, "/x/cfg/kiplay/kiplay.ini");
    /* Relative XDG paths are invalid per the spec: fall back to $HOME. */
    setenv("XDG_CONFIG_HOME", "rel", 1);
    setenv("HOME", "/h", 1);
    CHECK(kp_config_path(p, sizeof p, "kiplay.ini"));
    CHECK_STR_EQ(p, "/h/.config/kiplay/kiplay.ini");

    if (saved)
        setenv("XDG_CONFIG_HOME", saved, 1);
    else
        unsetenv("XDG_CONFIG_HOME");
    free(saved);
}

/* ---- saving without losing the file to a power cut ---------------------- */

static char *scratch(char *dir_out)
{
    kp_pathf(dir_out, KP_PATH_MAX, "/tmp/kp-util-XXXXXX");
    return mkdtemp(dir_out);
}

static void put(const char *path, const char *text, size_t len)
{
    FILE *f = fopen(path, "wb");
    CHECK(f);
    if (f) {
        fwrite(text, 1, len, f);
        fclose(f);
    }
}

static void drop(const char *dir, const char *const *names)
{
    char p[KP_PATH_MAX];
    for (; *names; names++) {
        kp_pathf(p, sizeof p, "%s/%s", dir, *names);
        remove(p);
    }
    rmdir(dir);
}

static const char *const NAMES[] = {"f", "f.backup", "f.tmp", "f.backup.tmp", NULL};

TEST(save_first_time_makes_no_backup)
{
    char dir[KP_PATH_MAX], path[KP_PATH_MAX], bak[KP_PATH_MAX];
    CHECK(scratch(dir));
    kp_pathf(path, sizeof path, "%s/f", dir);
    kp_pathf(bak, sizeof bak, "%s/f.backup", dir);
    CHECK_INT_EQ(kp_save_file(path, "a\n", 2, kp_check_lines), 0);
    char *t = kp_read_file(path, NULL);
    CHECK_STR_EQ(t, "a\n");
    free(t);
    CHECK(!kp_file_exists(bak));
    drop(dir, NAMES);
}

TEST(save_keeps_previous_as_backup_with_same_mode)
{
    char dir[KP_PATH_MAX], path[KP_PATH_MAX], bak[KP_PATH_MAX];
    CHECK(scratch(dir));
    kp_pathf(path, sizeof path, "%s/f", dir);
    kp_pathf(bak, sizeof bak, "%s/f.backup", dir);
    CHECK_INT_EQ(kp_save_file(path, "one\n", 4, kp_check_lines), 0);
    CHECK_INT_EQ(chmod(path, 0600), 0);
    CHECK_INT_EQ(kp_save_file(path, "two\n", 4, kp_check_lines), 0);
    char *t = kp_read_file(path, NULL);
    CHECK_STR_EQ(t, "two\n");
    free(t);
    t = kp_read_file(bak, NULL);
    CHECK_STR_EQ(t, "one\n");
    free(t);
    struct stat st;
    CHECK_INT_EQ(stat(bak, &st), 0);
    CHECK_INT_EQ((int)(st.st_mode & 07777), 0600);
    CHECK_INT_EQ(stat(path, &st), 0);
    CHECK_INT_EQ((int)(st.st_mode & 07777), 0600);
    drop(dir, NAMES);
}

TEST(damaged_file_does_not_replace_a_good_backup)
{
    char dir[KP_PATH_MAX], path[KP_PATH_MAX], bak[KP_PATH_MAX];
    CHECK(scratch(dir));
    kp_pathf(path, sizeof path, "%s/f", dir);
    kp_pathf(bak, sizeof bak, "%s/f.backup", dir);
    put(bak, "good\n", 5);
    put(path, "", 0);
    CHECK_INT_EQ(kp_save_file(path, "new\n", 4, kp_check_lines), 0);
    char *t = kp_read_file(bak, NULL);
    CHECK_STR_EQ(t, "good\n");
    free(t);
    t = kp_read_file(path, NULL);
    CHECK_STR_EQ(t, "new\n");
    free(t);

    /* Zeroed, and cut in the middle of a line, are damaged too. */
    put(path, "\0\0\0\0", 4);
    CHECK_INT_EQ(kp_save_file(path, "newer\n", 6, kp_check_lines), 0);
    put(path, "half a li", 9);
    CHECK_INT_EQ(kp_save_file(path, "newest\n", 7, kp_check_lines), 0);
    t = kp_read_file(bak, NULL);
    CHECK_STR_EQ(t, "good\n");
    free(t);
    drop(dir, NAMES);
}

TEST(ini_backup_wants_no_warning_but_reading_does_not)
{
    CHECK(kp_ini_check_clean("[a]\nk = v\n", 10));
    CHECK(!kp_ini_check_clean("[a]\nthis is a typo\n", 19));
    CHECK(!kp_ini_check_clean("", 0));

    char dir[KP_PATH_MAX], path[KP_PATH_MAX], bak[KP_PATH_MAX];
    CHECK(scratch(dir));
    kp_pathf(path, sizeof path, "%s/f", dir);
    kp_pathf(bak, sizeof bak, "%s/f.backup", dir);
    /* The user's typo stays: the file is read, not swapped for the backup. */
    put(bak, "[a]\nk = old\n", 12);
    put(path, "[a]\nk = v\ntypo\n", 15);
    char *t = kp_read_file_or_backup(path, NULL, NULL);
    CHECK_STR_HAS(t, "typo");
    free(t);

    kp_ini *ini = kp_ini_new();
    kp_ini_set(ini, "a", "k", "new");
    CHECK_INT_EQ(kp_ini_save_backed_up(ini, path), 0);
    kp_ini_free(ini);
    t = kp_read_file(bak, NULL);
    CHECK_STR_EQ(t, "[a]\nk = old\n"); /* the typo file was not good enough */
    free(t);
    drop(dir, NAMES);
}

TEST(read_prefers_the_file_and_falls_back_to_backup)
{
    char dir[KP_PATH_MAX], path[KP_PATH_MAX], bak[KP_PATH_MAX];
    CHECK(scratch(dir));
    kp_pathf(path, sizeof path, "%s/f", dir);
    kp_pathf(bak, sizeof bak, "%s/f.backup", dir);
    size_t len = 0;

    /* Neither: as kp_read_file says. */
    CHECK(!kp_read_file_or_backup(path, NULL, &len));
    CHECK_INT_EQ(errno, ENOENT);

    put(bak, "backup\n", 7);
    char *t = kp_read_file_or_backup(path, NULL, &len);
    CHECK_STR_EQ(t, "backup\n");
    CHECK_INT_EQ((int)len, 7);
    free(t);

    put(path, "", 0);
    t = kp_read_file_or_backup(path, NULL, &len);
    CHECK_STR_EQ(t, "backup\n");
    free(t);
    put(path, "\0\0\0", 3);
    t = kp_read_file_or_backup(path, NULL, &len);
    CHECK_STR_EQ(t, "backup\n");
    free(t);

    put(path, "main\n", 5);
    t = kp_read_file_or_backup(path, NULL, &len);
    CHECK_STR_EQ(t, "main\n");
    free(t);

    /* A bad backup too: what the plain read gives (an empty file). */
    put(path, "", 0);
    put(bak, "", 0);
    t = kp_read_file_or_backup(path, NULL, &len);
    CHECK(t && len == 0);
    free(t);
    drop(dir, NAMES);
}

TEST(lines_without_final_newline_are_not_sound)
{
    CHECK(kp_check_lines("a\nb\n", 4));
    CHECK(!kp_check_lines("a\nb", 3));
    CHECK(!kp_check_usable("a\0b", 3));
}

int main(void)
{
    RUN(pathf_fails_instead_of_truncating);
    RUN(durations_like_players);
    RUN(config_path_follows_xdg);
    RUN(save_first_time_makes_no_backup);
    RUN(save_keeps_previous_as_backup_with_same_mode);
    RUN(damaged_file_does_not_replace_a_good_backup);
    RUN(ini_backup_wants_no_warning_but_reading_does_not);
    RUN(read_prefers_the_file_and_falls_back_to_backup);
    RUN(lines_without_final_newline_are_not_sound);
    return harness_report();
}
