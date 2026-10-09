/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The INI reader/editor (from kikarinhas). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../harness.h"
#include "ini.h"
#include "util.h"

static int n_warnings;
static char last_warning[512];
static int last_line;

static void count_warning(void *ud, int line, const char *msg)
{
    (void)ud;
    n_warnings++;
    last_line = line;
    snprintf(last_warning, sizeof last_warning, "%s", msg);
}

static const char SAMPLE[] =
    "\xEF\xBB\xBF# topo\n"
    "[Window]\n"
    "size = 1920x1080   \r\n"
    "; comentário\n"
    "fps=60\n"
    "\n"
    "# antes do chat\n"
    "[chat]\n"
    "youtube = https://youtu.be/abc?t=1#x\n"
    "isto não é chave\n"
    "verbose = yes\n";

TEST(ini_reads_values_and_reports_bad_lines)
{
    n_warnings = 0;
    kp_ini *ini = kp_ini_parse(SAMPLE, count_warning, NULL);
    CHECK(ini);
    CHECK_INT_EQ(n_warnings, 1);
    CHECK_INT_EQ(last_line, 10);
    CHECK_STR_EQ(kp_ini_get(ini, "window", "SIZE"), "1920x1080");
    CHECK_STR_EQ(kp_ini_get(ini, "window", "fps"), "60");
    CHECK_STR_EQ(kp_ini_get(ini, "chat", "youtube"), "https://youtu.be/abc?t=1#x");
    CHECK_INT_EQ(kp_ini_line(ini, "chat", "verbose"), 11);
    CHECK(kp_ini_get(ini, "chat", "fps") == NULL);

    int it = 0, n = 0;
    const char *s, *k, *v;
    while (kp_ini_next(ini, &it, &s, &k, &v, NULL))
        n++;
    CHECK_INT_EQ(n, 4);
    kp_ini_free(ini);
}

TEST(ini_edits_keep_everything_else)
{
    kp_ini *ini = kp_ini_parse(SAMPLE, NULL, NULL);
    CHECK(ini);
    CHECK_INT_EQ(kp_ini_set(ini, "window", "fps", "30"), 0);   /* in place */
    CHECK_INT_EQ(kp_ini_set(ini, "window", "mode", "desktop"), 0); /* appended */
    CHECK_INT_EQ(kp_ini_set(ini, "command.buzina", "action", "sound"), 0);
    CHECK_INT_EQ(kp_ini_set(ini, "command.buzina", "data", "buzina"), 0);
    kp_ini_unset(ini, "chat", "verbose");
    char *out = kp_ini_dump(ini);
    CHECK_STR_EQ(out,
                 "# topo\n"
                 "[Window]\n"
                 "size = 1920x1080   \n"
                 "; comentário\n"
                 "fps = 30\n"
                 "mode = desktop\n"
                 "\n"
                 "# antes do chat\n"
                 "[chat]\n"
                 "youtube = https://youtu.be/abc?t=1#x\n"
                 "isto não é chave\n"
                 "\n"
                 "[command.buzina]\n"
                 "action = sound\n"
                 "data = buzina\n");
    free(out);

    /* Removing a section keeps the comment that introduces the next one. */
    kp_ini_remove_section(ini, "window");
    out = kp_ini_dump(ini);
    CHECK_STR_EQ(out,
                 "# topo\n"
                 "\n"
                 "# antes do chat\n"
                 "[chat]\n"
                 "youtube = https://youtu.be/abc?t=1#x\n"
                 "isto não é chave\n"
                 "\n"
                 "[command.buzina]\n"
                 "action = sound\n"
                 "data = buzina\n");
    free(out);

    int it = 0, n = 0;
    const char *sec;
    while (kp_ini_next_section(ini, &it, &sec))
        n++;
    CHECK_INT_EQ(n, 2);
    kp_ini_free(ini);
}

TEST(ini_save_is_atomic_and_rereadable)
{
    char dir[] = "/tmp/kd-test-XXXXXX";
    CHECK(mkdtemp(dir));
    char path[KP_PATH_MAX];
    snprintf(path, sizeof path, "%s/sub/kiplay.ini", dir);
    kp_ini *ini = kp_ini_new();
    kp_ini_set(ini, "avatars", "scale", "3");
    CHECK_INT_EQ(kp_ini_save(ini, path), 0);
    kp_ini_free(ini);

    char *text = kp_read_file(path, NULL);
    CHECK_STR_EQ(text, "[avatars]\nscale = 3\n");
    free(text);
    char sub[KP_PATH_MAX];
    snprintf(sub, sizeof sub, "%s/sub", dir);
    remove(path);
    rmdir(sub);
    rmdir(dir);
}

/* The cleaner definitions list one "delete =" per line: iteration has to
 * give every one, in file order. */
TEST(ini_repeated_keys_iterate_in_order)
{
    kp_ini *ini = kp_ini_parse("[option.cache]\n"
                               "delete = walk.files ~/.cache/a/\n"
                               "name = Cache\n"
                               "delete = file ~/b\n",
                               NULL, NULL);
    CHECK(ini);
    CHECK_STR_EQ(kp_ini_get(ini, "option.cache", "delete"), "file ~/b");
    int it = 0, n = 0;
    const char *s, *k, *v;
    const char *want[] = {"walk.files ~/.cache/a/", "file ~/b"};
    while (kp_ini_next(ini, &it, &s, &k, &v, NULL)) {
        if (strcmp(k, "delete") != 0)
            continue;
        CHECK(n < 2);
        CHECK_STR_EQ(v, want[n]);
        n++;
    }
    CHECK_INT_EQ(n, 2);
    kp_ini_free(ini);
}

int main(void)
{
    RUN(ini_reads_values_and_reports_bad_lines);
    RUN(ini_edits_keep_everything_else);
    RUN(ini_save_is_atomic_and_rereadable);
    RUN(ini_repeated_keys_iterate_in_order);
    return harness_report();
}
