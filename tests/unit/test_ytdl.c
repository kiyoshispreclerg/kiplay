/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <glib.h>
#include <string.h>

#include "../harness.h"
#include "ytdl.h"

TEST(splits_links_by_space_and_line)
{
    char **u = kp_ytdl_split_urls("  https://a/1 https://a/2\n\n# comment\n\thttps://a/3\r\n");
    CHECK_INT_EQ((int)g_strv_length(u), 3);
    CHECK_STR_EQ(u[0], "https://a/1");
    CHECK_STR_EQ(u[2], "https://a/3");
    g_strfreev(u);
}

TEST(builds_the_command)
{
    char *urls[] = {"https://y/1", "https://y/2", NULL};
    kp_ytdl_opts o = {.urls = urls, .dest = "/tmp/d", .tmpl = "%(title).80s.%(ext)s",
                      .max_height = 720, .remux_mp4 = true, .subs = true, .sub_langs = "pt,en",
                      .thumbnail = true, .items = "1:3", .extra = "--limit-rate '1 M'"};
    char *err = NULL;
    char **a = kp_ytdl_argv("yt-dlp", &o, &err);
    CHECK(a != NULL);
    char *line = kp_ytdl_command_line(a);
    CHECK_STR_EQ(line, "yt-dlp --newline -P /tmp/d -o '%(title).80s.%(ext)s' "
                       "-f 'bestvideo[height<=720]+bestaudio/best[height<=720]' "
                       "--remux-video mp4 --write-subs --sub-langs pt,en --sub-format srv3/best "
                       "--write-thumbnail -I 1:3 --limit-rate '1 M' -- https://y/1 https://y/2");
    g_free(line);
    g_strfreev(a);
}

TEST(audio_only_ignores_video_options)
{
    char *urls[] = {"u", NULL};
    kp_ytdl_opts o = {.urls = urls, .audio_only = true, .audio_format = "m4a", .max_height = 720,
                      .remux_mp4 = true};
    char **a = kp_ytdl_argv("yt-dlp", &o, NULL);
    char *line = kp_ytdl_command_line(a);
    CHECK_STR_EQ(line, "yt-dlp --newline -x --audio-format m4a -- u");
    g_free(line);
    g_strfreev(a);
}

TEST(rejects_bad_input)
{
    char *none[] = {NULL};
    char *urls[] = {"u", NULL};
    char *err = NULL;
    kp_ytdl_opts o = {.urls = none};
    CHECK(kp_ytdl_argv("y", &o, &err) == NULL);
    CHECK(err != NULL);
    g_free(err);
    err = NULL;
    o = (kp_ytdl_opts){.urls = urls, .items = "1;rm"};
    CHECK(kp_ytdl_argv("y", &o, &err) == NULL);
    g_free(err);
    err = NULL;
    o = (kp_ytdl_opts){.urls = urls, .extra = "'unclosed"};
    CHECK(kp_ytdl_argv("y", &o, &err) == NULL);
    g_free(err);
}

TEST(reads_progress)
{
    kp_ytdl_progress p = {0};
    CHECK(kp_ytdl_progress_feed(&p, "[download] Downloading item 2 of 4"));
    CHECK_INT_EQ(p.item, 2);
    CHECK_INT_EQ(p.items, 4);
    CHECK(kp_ytdl_progress_feed(&p, "[download] Destination: /x/y/Song [abc].webm"));
    CHECK_STR_EQ(p.file, "Song [abc].webm");
    CHECK(kp_ytdl_progress_feed(&p, "[download]  50.0% of ~  5.00MiB at  1.20MiB/s ETA 00:03"));
    CHECK(p.percent == 50.0);
    CHECK_STR_EQ(p.detail, "of ~  5.00MiB at  1.20MiB/s ETA 00:03");
    CHECK(kp_ytdl_progress_fraction(&p) == (1 + 0.5) / 4);
    CHECK(!kp_ytdl_progress_feed(&p, "[youtube] abc: Downloading webpage"));
    CHECK(!kp_ytdl_progress_feed(&p, "[download] Downloading playlist: Mix"));
    CHECK_INT_EQ(p.item, 2);
    CHECK(kp_ytdl_progress_feed(&p, "[download] Song.webm has already been downloaded"));
    CHECK(p.percent == 100.0);
    kp_ytdl_progress_clear(&p);
}

static GMainLoop *loop;
static int lines;
static bool was_cancelled;
static char *failure;

static void on_line(void *ud, const char *l)
{
    (void)ud;
    (void)l;
    lines++;
}

static void on_done(void *ud, bool cancelled, const char *err)
{
    (void)ud;
    was_cancelled = cancelled;
    failure = g_strdup(err);
    g_main_loop_quit(loop);
}

TEST(runs_and_reports_the_last_error)
{
    char *argv[] = {"/bin/sh", "-c", "echo one; echo 'ERROR: [youtube] x: gone' >&2; exit 1", NULL};
    loop = g_main_loop_new(NULL, FALSE);
    lines = 0;
    CHECK(kp_ytdl_start(argv, on_line, on_done, NULL, NULL) != NULL);
    g_main_loop_run(loop);
    CHECK_INT_EQ(lines, 2);
    CHECK(!was_cancelled);
    CHECK_STR_EQ(failure, "[youtube] x: gone");
    g_free(failure);
    g_main_loop_unref(loop);
}

static gboolean cancel_it(gpointer y)
{
    kp_ytdl_cancel(y);
    return G_SOURCE_REMOVE;
}

TEST(cancel_ends_it)
{
    char *argv[] = {"/bin/sh", "-c", "sleep 30", NULL};
    loop = g_main_loop_new(NULL, FALSE);
    kp_ytdl *y = kp_ytdl_start(argv, on_line, on_done, NULL, NULL);
    CHECK(y != NULL);
    g_timeout_add(100, cancel_it, y);
    g_main_loop_run(loop);
    CHECK(was_cancelled);
    CHECK(failure == NULL);
    g_main_loop_unref(loop);
}

TEST(cannot_start)
{
    char *argv[] = {"/nonexistent/yt-dlp", NULL};
    char *err = NULL;
    CHECK(kp_ytdl_start(argv, on_line, on_done, NULL, &err) == NULL);
    CHECK(err != NULL);
    g_free(err);
}

int main(void)
{
    RUN(splits_links_by_space_and_line);
    RUN(builds_the_command);
    RUN(audio_only_ignores_video_options);
    RUN(rejects_bad_input);
    RUN(reads_progress);
    RUN(runs_and_reports_the_last_error);
    RUN(cancel_ends_it);
    RUN(cannot_start);
    return harness_report();
}
