/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "child.h"

#include <signal.h>
#include <stdlib.h>

#include "log.h"

struct kp_child {
    GSubprocess *proc;
    GCancellable *cancel;
    kp_child_exit_cb on_exit;
    void *ud;
};

static void on_wait(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *err = NULL;
    bool ok = g_subprocess_wait_finish(G_SUBPROCESS(src), res, &err);
    if (!ok && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err);
        return; /* kp_child_stop: c is gone */
    }
    g_clear_error(&err);
    kp_child *c = data;
    GSubprocess *p = G_SUBPROCESS(src);
    char *why = g_subprocess_get_if_signaled(p)
                    ? g_strdup_printf("signal %d", g_subprocess_get_term_sig(p))
                    : g_strdup_printf("exit status %d", g_subprocess_get_exit_status(p));
    c->on_exit(c->ud, why);
    g_free(why);
}

kp_child *kp_child_spawn(const char *const *argv, GSubprocessFlags flags, kp_child_exit_cb on_exit,
                         void *ud, char **err)
{
    if (kp_log_debug_enabled()) {
        char *line = g_strjoinv(" ", (char **)argv);
        kp_log_debug("spawn: %s", line);
        g_free(line);
    }
    GError *gerr = NULL;
    GSubprocess *proc = g_subprocess_newv(argv, flags, &gerr);
    if (!proc) {
        if (err)
            *err = g_strdup(gerr->message);
        g_error_free(gerr);
        return NULL;
    }
    kp_child *c = g_new0(kp_child, 1);
    c->proc = proc;
    c->cancel = g_cancellable_new();
    c->on_exit = on_exit;
    c->ud = ud;
    g_subprocess_wait_async(proc, c->cancel, on_wait, c);
    return c;
}

void kp_child_stop(kp_child *c)
{
    if (!c)
        return;
    g_cancellable_cancel(c->cancel);
    g_object_unref(c->cancel);
    /* Nobody waits for it after this; GLib still reaps it. */
    g_subprocess_send_signal(c->proc, SIGTERM);
    g_object_unref(c->proc);
    g_free(c);
}

GSubprocess *kp_child_proc(kp_child *c)
{
    return c->proc;
}

int kp_child_pid(const kp_child *c)
{
    const char *id = c ? g_subprocess_get_identifier(c->proc) : NULL;
    return id ? atoi(id) : 0;
}
