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
 * `curl` child processes (a batch of pictures each), never blocking the
 * window. The cache is emptied from the preferences (kp_img_cache_clear).
 *
 * Memory: the table owns the pixbufs and is emptied when it reaches
 * MAX_PICTURES (the visible ones come back from the disk at once). A URL
 * that failed is not retried until kp_img_reset. */

#define MAX_PICTURES 400
#define MAX_RUNNING 2
#define CURL_TIMEOUT "20"

static GHashTable *pictures; /* url -> GdkPixbuf */
static GHashTable *failed;   /* url -> (set) */
static GHashTable *wanted;   /* url -> (set): queued or running */
static GQueue queue;         /* urls (owned by wanted's keys) */
static int running;
static kp_img_cb changed_cb;
static void *changed_ud;

/* One curl child fetches up to BATCH pictures (one connection, one
 * process, instead of a process per avatar: a busy chat brings dozens of
 * new faces a minute). */
#define BATCH 12

typedef struct {
    int n;
    char *url[BATCH], *dest[BATCH], *tmp[BATCH];
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

static char *cache_dir(void)
{
    return g_build_filename(g_get_user_cache_dir(), "kiplay", "chat", NULL);
}

static char *cache_file(const char *url)
{
    char *sum = g_compute_checksum_for_string(G_CHECKSUM_SHA1, url, -1);
    char *dir = cache_dir();
    char *path = g_build_filename(dir, sum, NULL);
    g_free(dir);
    g_free(sum);
    return path;
}

/* Walks the cache folder: how many bytes it holds, and, with remove, empties
 * it (only regular files; the folder stays). */
static gint64 walk_cache(bool remove)
{
    char *dir = cache_dir();
    gint64 total = 0;
    GDir *d = g_dir_open(dir, 0, NULL);
    const char *name;
    while (d && (name = g_dir_read_name(d))) {
        char *path = g_build_filename(dir, name, NULL);
        GStatBuf st;
        if (g_stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            total += st.st_size;
            /* A picture on its way (.part) is left alone. */
            if (remove && !g_str_has_suffix(name, ".part"))
                g_unlink(path);
        }
        g_free(path);
    }
    if (d)
        g_dir_close(d);
    g_free(dir);
    return total;
}

gint64 kp_img_cache_size(void)
{
    return walk_cache(false);
}

gint64 kp_img_cache_clear(void)
{
    /* Pictures being downloaded land in the emptied folder: harmless. */
    if (pictures)
        g_hash_table_remove_all(pictures);
    if (failed)
        g_hash_table_remove_all(failed);
    gint64 freed = walk_cache(true);
    if (changed_cb)
        changed_cb(changed_ud); /* the panel asks again: it downloads what it shows */
    return freed;
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

static void job_free(job *j)
{
    for (int i = 0; i < j->n; i++) {
        g_free(j->url[i]);
        g_free(j->dest[i]);
        g_free(j->tmp[i]);
    }
    g_free(j);
}

static void finished(GObject *src, GAsyncResult *res, gpointer data)
{
    job *j = data;
    GSubprocess *p = G_SUBPROCESS(src);
    g_subprocess_wait_finish(p, res, NULL);
    running--;
    /* curl goes on after a picture that fails, so each is judged by its
     * own file: it has to be there and to decode. */
    for (int i = 0; i < j->n; i++) {
        bool ok = g_rename(j->tmp[i], j->dest[i]) == 0 && load_from_disk(j->url[i], j->dest[i]);
        if (!ok) {
            kp_log_debug("chat picture failed: %s", j->url[i]);
            g_unlink(j->tmp[i]);
            g_unlink(j->dest[i]);
            g_hash_table_replace(failed, g_strdup(j->url[i]), GINT_TO_POINTER(1));
        }
        g_hash_table_remove(wanted, j->url[i]);
    }
    if (changed_cb)
        changed_cb(changed_ud);
    g_object_unref(p);
    job_free(j);
    start_next();
}

static void start_next(void)
{
    while (running < MAX_RUNNING && !g_queue_is_empty(&queue)) {
        job *j = g_new0(job, 1);
        GPtrArray *argv = g_ptr_array_new();
        g_ptr_array_add(argv, (char *)"curl");
        g_ptr_array_add(argv, (char *)"-fsSL");
        g_ptr_array_add(argv, (char *)"--max-time");
        g_ptr_array_add(argv, (char *)CURL_TIMEOUT);
        while (j->n < BATCH && !g_queue_is_empty(&queue)) {
            char *url = g_queue_pop_head(&queue);
            j->url[j->n] = url;
            j->dest[j->n] = cache_file(url);
            j->tmp[j->n] = g_strdup_printf("%s.part", j->dest[j->n]);
            kp_make_parent_dirs(j->dest[j->n]);
            g_ptr_array_add(argv, (char *)"-o");
            g_ptr_array_add(argv, j->tmp[j->n]);
            g_ptr_array_add(argv, url);
            j->n++;
        }
        g_ptr_array_add(argv, NULL);
        GError *err = NULL;
        GSubprocess *p = g_subprocess_newv((const char *const *)argv->pdata,
                                           G_SUBPROCESS_FLAGS_STDERR_SILENCE, &err);
        g_ptr_array_free(argv, TRUE);
        if (!p) {
            kp_log_debug("curl: %s", err->message);
            g_error_free(err);
            for (int i = 0; i < j->n; i++) {
                g_hash_table_replace(failed, g_strdup(j->url[i]), GINT_TO_POINTER(1));
                g_hash_table_remove(wanted, j->url[i]);
            }
            job_free(j);
            continue;
        }
        running++;
        g_subprocess_wait_async(p, NULL, finished, j);
    }
}

static guint kick;

static gboolean kick_cb(gpointer data)
{
    (void)data;
    kick = 0;
    start_next();
    return G_SOURCE_REMOVE;
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
    /* Later, so the pictures one frame asks for travel together. */
    if (!kick)
        kick = g_timeout_add(250, kick_cb, NULL);
    return NULL;
}
