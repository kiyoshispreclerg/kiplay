/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../harness.h"
#include "prefs.h"
#include "util.h"

static char dir[] = "/tmp/kp-test-XXXXXX";
static char path[KP_PATH_MAX];

static void write_file(const char *text)
{
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

TEST(missing_file_gives_defaults)
{
    remove(path);
    kp_prefs *p = kp_prefs_load(path);
    CHECK(p);
    CHECK_INT_EQ(kp_prefs_get_int(p, "window", "tab", 2, 0, 3), 2);
    /* Nothing changed: nothing written. */
    CHECK_INT_EQ(kp_prefs_save(p), 0);
    CHECK(!kp_file_exists(path));
    kp_prefs_free(p);
}

TEST(bad_values_fall_back_or_clamp)
{
    write_file("[window]\ntab = 9\nwidth = abc\nheight = -5\n");
    kp_prefs *p = kp_prefs_load(path);
    CHECK(p);
    CHECK_INT_EQ(kp_prefs_get_int(p, "window", "tab", 0, 0, 3), 3);
    CHECK_INT_EQ(kp_prefs_get_int(p, "window", "width", 860, 400, 10000), 860);
    CHECK_INT_EQ(kp_prefs_get_int(p, "window", "height", 600, 300, 10000), 300);
    kp_prefs_free(p);
}

TEST(save_keeps_comments_and_unknown_keys)
{
    write_file("# mine\n[window]\ntab = 1\n[future]\nx = y\n");
    kp_prefs *p = kp_prefs_load(path);
    CHECK(p);
    kp_prefs_set_int(p, "window", "tab", 1); /* same value: not dirty */
    struct stat before;
    CHECK(stat(path, &before) == 0);
    CHECK_INT_EQ(kp_prefs_save(p), 0);
    kp_prefs_set_int(p, "window", "tab", 3);
    kp_prefs_set_int(p, "window", "width", 1000);
    CHECK_INT_EQ(kp_prefs_save(p), 0);
    kp_prefs_free(p);

    char *text = kp_read_file(path, NULL);
    CHECK_STR_EQ(text, "# mine\n[window]\ntab = 3\nwidth = 1000\n[future]\nx = y\n");
    free(text);
}

int main(void)
{
    if (!mkdtemp(dir))
        return 1;
    snprintf(path, sizeof path, "%s/kiplay.ini", dir);
    RUN(missing_file_gives_defaults);
    RUN(bad_values_fall_back_or_clamp);
    RUN(save_keeps_comments_and_unknown_keys);
    remove(path);
    rmdir(dir);
    return harness_report();
}
