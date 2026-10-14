/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include "../harness.h"
#include "instance.h"

TEST(absolute_paths)
{
    char *cwd = g_get_current_dir();
    char *a = kp_instance_absolute("x/../song.mp3");
    char *want = g_build_filename(cwd, "song.mp3", NULL);
    CHECK_STR_EQ(a, want);
    g_free(a);
    g_free(want);
    a = kp_instance_absolute("/abs/v.mkv");
    CHECK_STR_EQ(a, "/abs/v.mkv");
    g_free(a);
    a = kp_instance_absolute("https://example.com/a?b=1");
    CHECK_STR_EQ(a, "https://example.com/a?b=1");
    g_free(a);
    g_free(cwd);
}

TEST(sockets_differ_per_config)
{
    char *a = kp_instance_socket_path("/home/x/.config/kiplay");
    char *b = kp_instance_socket_path("/tmp/test/kiplay");
    CHECK(strcmp(a, b) != 0);
    CHECK(strlen(a) < 108);
    g_free(a);
    g_free(b);
}

typedef struct {
    GMainLoop *loop;
    char **got;
    unsigned flags;
    int calls;
} receiver;

static void on_files(void *ud, char **locations, unsigned flags)
{
    receiver *r = ud;
    g_strfreev(r->got);
    r->got = g_strdupv(locations);
    r->flags = flags;
    r->calls++;
    g_main_loop_quit(r->loop);
}

typedef struct {
    const char *path;
    char **files;
    unsigned flags;
    bool result;
} sender;

/* The other Kiplay: a thread here, since forward blocks until we read. */
static gpointer send_thread(gpointer data)
{
    sender *s = data;
    s->result = kp_instance_forward(s->path, s->files, s->flags);
    return NULL;
}

static gboolean give_up(gpointer data)
{
    g_main_loop_quit(data);
    return G_SOURCE_REMOVE;
}

TEST(hands_files_to_the_running_one)
{
    char *dir = g_dir_make_tmp("kp-test-XXXXXX", NULL);
    char *path = g_build_filename(dir, "i.sock", NULL);

    /* Nobody listening: the caller starts its own window. */
    char *one[] = {"/a.mp3", NULL};
    CHECK(!kp_instance_forward(path, one, false));

    receiver r = {.loop = g_main_loop_new(NULL, FALSE)};
    char *err = NULL;
    kp_instance *in = kp_instance_listen(path, on_files, &r, &err);
    CHECK(in != NULL);

    char *files[] = {"/música/ação.ogg", "rel.mp3", "http://radio/x", NULL};
    sender s = {path, files, KP_OPEN_ENQUEUE | KP_OPEN_NEW_PLAYLIST, false};
    GThread *t = g_thread_new("send", send_thread, &s);
    guint timer = g_timeout_add_seconds(5, give_up, r.loop);
    g_main_loop_run(r.loop);
    g_source_remove(timer);
    g_thread_join(t);

    CHECK(s.result);
    CHECK_INT_EQ(r.calls, 1);
    CHECK_INT_EQ((int)r.flags, KP_OPEN_ENQUEUE | KP_OPEN_NEW_PLAYLIST);
    CHECK(r.got && g_strv_length(r.got) == 3);
    CHECK_STR_EQ(r.got[0], "/música/ação.ogg");
    CHECK(g_path_is_absolute(r.got[1]) && g_str_has_suffix(r.got[1], "/rel.mp3"));
    CHECK_STR_EQ(r.got[2], "http://radio/x");

    /* A stale socket (the first one crashed) is replaced. */
    kp_instance_free(in);
    CHECK(!g_file_test(path, G_FILE_TEST_EXISTS));
    in = kp_instance_listen(path, on_files, &r, &err);
    CHECK(in != NULL);
    kp_instance_free(in);

    g_strfreev(r.got);
    g_main_loop_unref(r.loop);
    g_rmdir(dir);
    g_free(path);
    g_free(dir);
}

int main(void)
{
    RUN(absolute_paths);
    RUN(sockets_differ_per_config);
    RUN(hands_files_to_the_running_one);
    return harness_report();
}
