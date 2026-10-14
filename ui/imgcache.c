/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

#include "app.h"
#include "log.h"
#include "util.h"

/* Pictures of the chat (channel emoji, avatars) by URL: kept on disk under
 * ~/.cache/kiplay/chat/<sha1 of the URL>, so the next time the same live
 * opens without the network, and in memory as pixbufs. Downloads are
 * `curl` child processes, a few at a time, never blocking the window.
 *
 * Memory: the table owns the pixbufs and is emptied when it reaches
 * MAX_PICTURES (the visible ones come back from the disk at once). A URL
 * that failed is not retried until kp_img_reset. */

#define MAX_PICTURES 400
#define MAX_RUNNING 4
#define CURL_TIMEOUT "20"

static GHashTable *pictures; /* url -> GdkPixbuf */
static GHashTable *failed;   /* url -> (set) */
static GHashTable *wanted;   /* url -> (set): queued or running */
static GQueue queue;         /* urls (owned by wanted's keys) */
static int running;
static kp_img_cb changed_cb;
static void *changed_ud;

typedef struct {
    char *url, *dest, *tmp;
} job;

void kp_img_init(kp_img_cb changed, void *ud)
{
    changed_cb = changed;
    changed_ud = ud;
    if (!pictures) {
        pictures = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
        failed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        wanted = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }
}

void kp_img_reset(void)
{
    if (failed)
        g_hash_table_remove_all(failed);
}

static char *cache_file(const char *url)
{
    char *sum = g_compute_checksum_for_string(G_CHECKSUM_SHA1, url, -1);
    char *path = g_build_filename(g_get_user_cache_dir(), "kiplay", "chat", sum, NULL);
    g_free(sum);
    return path;
}

static bool load_from_disk(const char *url, const char *path)
{
    GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, NULL);
    if (!pb)
        return false;
    if (g_hash_table_size(pictures) >= MAX_PICTURES)
        g_hash_table_remove_all(pictures);
    g_hash_table_replace(pictures, g_strdup(url), pb);
    return true;
}

static void start_next(void);

static void finished(GObject *src, GAsyncResult *res, gpointer data)
{
    job *j = data;
    GSubprocess *p = G_SUBPROCESS(src);
    g_subprocess_wait_finish(p, res, NULL);
    running--;
    bool ok = g_subprocess_get_if_exited(p) && g_subprocess_get_exit_status(p) == 0 &&
              g_rename(j->tmp, j->dest) == 0 && load_from_disk(j->url, j->dest);
    if (!ok) {
        kp_log_debug("chat picture failed: %s", j->url);
        g_unlink(j->tmp);
        g_hash_table_replace(failed, g_strdup(j->url), GINT_TO_POINTER(1));
    }
    g_hash_table_remove(wanted, j->url);
    if (changed_cb)
        changed_cb(changed_ud);
    g_object_unref(p);
    g_free(j->url);
    g_free(j->dest);
    g_free(j->tmp);
    g_free(j);
    start_next();
}

static void start_next(void)
{
    while (running < MAX_RUNNING && !g_queue_is_empty(&queue)) {
        char *url = g_queue_pop_head(&queue);
        job *j = g_new0(job, 1);
        j->url = url;
        j->dest = cache_file(url);
        j->tmp = g_strdup_printf("%s.part", j->dest);
        kp_make_parent_dirs(j->dest);
        const char *argv[] = {"curl", "-fsSL", "--max-time", CURL_TIMEOUT, "-o", j->tmp, url, NULL};
        GError *err = NULL;
        GSubprocess *p = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDERR_SILENCE, &err);
        if (!p) {
            kp_log_debug("curl: %s", err->message);
            g_error_free(err);
            g_hash_table_replace(failed, g_strdup(url), GINT_TO_POINTER(1));
            g_hash_table_remove(wanted, url);
            g_free(j->url);
            g_free(j->dest);
            g_free(j->tmp);
            g_free(j);
            continue;
        }
        running++;
        g_subprocess_wait_async(p, NULL, finished, j);
    }
}

GdkPixbuf *kp_img_get(const char *url)
{
    if (!url || !pictures)
        return NULL;
    GdkPixbuf *pb = g_hash_table_lookup(pictures, url);
    if (pb)
        return pb;
    if (g_hash_table_contains(failed, url) || g_hash_table_contains(wanted, url))
        return NULL;
    char *path = cache_file(url);
    if (kp_file_exists(path) && load_from_disk(url, path)) {
        g_free(path);
        return g_hash_table_lookup(pictures, url);
    }
    g_free(path);
    if (!g_str_has_prefix(url, "https://") && !g_str_has_prefix(url, "http://"))
        return NULL;
    char *copy = g_strdup(url);
    g_hash_table_replace(wanted, copy, GINT_TO_POINTER(1));
    /* The queue holds its own copy: wanted's key may go with its entry. */
    g_queue_push_tail(&queue, g_strdup(url));
    start_next();
    return NULL;
}
