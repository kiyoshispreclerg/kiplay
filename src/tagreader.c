/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tagreader.h"

#include <glib.h>
#include <string.h>

#include "child.h"
#include "i18n.h"
#include "lineio.h"
#include "log.h"

/* Requests in flight at once: enough to keep the reader busy, few enough
 * that a crash sends little back to the queue. */
#define WINDOW 8
/* No answer for this long: the file hangs the reader (a dead network
 * mount...). A cold disk answers in tens of milliseconds. */
#define ANSWER_TIMEOUT_S 15
/* The reader goes this long after its last answer. */
#define IDLE_S 5

typedef struct {
    int list_id;
    long index;
    char *location;
} request;

struct kp_tagreader {
    char *program;
    kp_tags_cb cb;
    void *ud;
    kp_child *child;
    kp_lineio *io;
    GQueue pending;  /* request*, not sent yet */
    GQueue inflight; /* request*, sent, answers come in this order */
    guint timeout, idle;
    bool spawn_failed_logged;
};

static void free_request(request *r)
{
    g_free(r->location);
    g_free(r);
}

static void stop_child(kp_tagreader *tr)
{
    if (!tr->child)
        return;
    kp_lineio_detach(tr->io);
    kp_child_stop(tr->child);
    tr->child = NULL;
}

static void clear_timer(guint *t)
{
    if (*t) {
        g_source_remove(*t);
        *t = 0;
    }
}

static void answer(kp_tagreader *tr, request *r, const kp_tags *tags)
{
    tr->cb(tr->ud, r->list_id, r->index, r->location, tags);
    free_request(r);
}

static void fill(kp_tagreader *tr);

/* The reader is gone (crashed, hung): the file it was on fails, the
 * others go back to the queue, and a new reader goes on. */
static void lost(kp_tagreader *tr, const char *why)
{
    kp_log_debug("tag reader: %s", why);
    stop_child(tr);
    clear_timer(&tr->timeout);
    request *bad = g_queue_pop_head(&tr->inflight);
    request *r;
    while ((r = g_queue_pop_tail(&tr->inflight)))
        g_queue_push_head(&tr->pending, r);
    if (bad)
        answer(tr, bad, NULL);
    fill(tr);
}

static gboolean on_timeout(gpointer data)
{
    kp_tagreader *tr = data;
    tr->timeout = 0;
    lost(tr, "no answer, giving up on that file");
    return G_SOURCE_REMOVE;
}

static gboolean on_idle(gpointer data)
{
    kp_tagreader *tr = data;
    tr->idle = 0;
    kp_log_debug("tag reader: nothing to read, letting it go");
    stop_child(tr);
    return G_SOURCE_REMOVE;
}

static void arm_timeout(kp_tagreader *tr)
{
    clear_timer(&tr->timeout);
    if (tr->inflight.length)
        tr->timeout = g_timeout_add_seconds(ANSWER_TIMEOUT_S, on_timeout, tr);
}

static void on_exit(void *ud, const char *why)
{
    lost(ud, why);
}

/* "ok 12.5\tTitle\tArtist\tAlbum" or "error ...". */
static void on_line(void *ud, const char *line)
{
    kp_tagreader *tr = ud;
    if (!line)
        return; /* its exit is reported apart */
    request *r = g_queue_pop_head(&tr->inflight);
    if (!r) {
        kp_log_debug("tag reader: unexpected line: %s", line);
        return;
    }
    arm_timeout(tr);
    if (!strncmp(line, "ok ", 3)) {
        char **f = g_strsplit(line + 3, "\t", 4);
        double secs = f[0] ? g_ascii_strtod(f[0], NULL) : -1;
        kp_tags t = {
            .title = f[0] && f[1] && *f[1] ? f[1] : NULL,
            .artist = f[0] && f[1] && f[2] && *f[2] ? f[2] : NULL,
            .album = f[0] && f[1] && f[2] && f[3] && *f[3] ? f[3] : NULL,
            .length_ms = secs > 0 ? (int64_t)(secs * 1000 + 0.5) : -1,
        };
        answer(tr, r, &t);
        g_strfreev(f);
    } else {
        kp_log_debug("tag reader: %s: %s", r->location, line);
        answer(tr, r, NULL);
    }
    fill(tr);
}

static bool ensure_child(kp_tagreader *tr)
{
    if (tr->child)
        return true;
    const char *argv[] = {tr->program, "--tags", NULL};
    char *err = NULL;
    tr->child = kp_child_spawn(argv, G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE,
                               on_exit, tr, &err);
    if (!tr->child) {
        if (!tr->spawn_failed_logged)
            kp_log_warn(_("cannot read tags (%s): %s"), tr->program, err);
        tr->spawn_failed_logged = true;
        g_free(err);
        return false;
    }
    GSubprocess *p = kp_child_proc(tr->child);
    kp_lineio_attach(tr->io, g_subprocess_get_stdout_pipe(p), g_subprocess_get_stdin_pipe(p));
    return true;
}

/* A location as one protocol line: a name with a newline goes as a URI. */
static char *one_line(const char *location)
{
    if (!strchr(location, '\n'))
        return g_strdup(location);
    char *uri = location[0] == '/' ? g_filename_to_uri(location, NULL, NULL) : NULL;
    return uri ? uri : g_strdelimit(g_strdup(location), "\n", ' ');
}

/* Keeps WINDOW requests in flight; lets the reader go once all is done. */
static void fill(kp_tagreader *tr)
{
    if (!tr->pending.length) {
        if (!tr->inflight.length && tr->child && !tr->idle)
            tr->idle = g_timeout_add_seconds(IDLE_S, on_idle, tr);
        return;
    }
    clear_timer(&tr->idle);
    if (!ensure_child(tr)) {
        request *r;
        while ((r = g_queue_pop_head(&tr->pending)))
            answer(tr, r, NULL);
        return;
    }
    bool was_empty = !tr->inflight.length;
    while (tr->inflight.length < WINDOW && tr->pending.length) {
        request *r = g_queue_pop_head(&tr->pending);
        char *line = one_line(r->location);
        kp_lineio_send(tr->io, line);
        g_free(line);
        g_queue_push_tail(&tr->inflight, r);
    }
    if (was_empty)
        arm_timeout(tr);
}

kp_tagreader *kp_tagreader_new(const char *program, kp_tags_cb cb, void *ud)
{
    kp_tagreader *tr = g_new0(kp_tagreader, 1);
    tr->program = g_strdup(program);
    tr->cb = cb;
    tr->ud = ud;
    tr->io = kp_lineio_new("tags", on_line, tr);
    g_queue_init(&tr->pending);
    g_queue_init(&tr->inflight);
    return tr;
}

void kp_tagreader_free(kp_tagreader *tr)
{
    if (!tr)
        return;
    stop_child(tr);
    clear_timer(&tr->timeout);
    clear_timer(&tr->idle);
    kp_lineio_free(tr->io);
    g_queue_clear_full(&tr->pending, (GDestroyNotify)free_request);
    g_queue_clear_full(&tr->inflight, (GDestroyNotify)free_request);
    g_free(tr->program);
    g_free(tr);
}

void kp_tagreader_add(kp_tagreader *tr, int list_id, long index, const char *location)
{
    request *r = g_new(request, 1);
    *r = (request){list_id, index, g_strdup(location)};
    g_queue_push_tail(&tr->pending, r);
    fill(tr);
}

void kp_tagreader_forget(kp_tagreader *tr, int list_id)
{
    for (GList *l = tr->pending.head; l;) {
        GList *next = l->next;
        request *r = l->data;
        if (r->list_id == list_id) {
            free_request(r);
            g_queue_delete_link(&tr->pending, l);
        }
        l = next;
    }
    /* The ones in flight still answer; the caller finds no playlist. */
}

size_t kp_tagreader_pending(const kp_tagreader *tr)
{
    return tr->pending.length + tr->inflight.length;
}

int kp_tagreader_pid(const kp_tagreader *tr)
{
    return kp_child_pid(tr->child);
}
