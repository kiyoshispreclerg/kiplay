/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_CHILD_H
#define KP_CHILD_H

#include <gio/gio.h>
#include <stdbool.h>

/* A helper process (mpv, kiplay-audio): spawned without blocking, and its
 * end reported on the main loop unless we stopped it ourselves.
 *
 * Memory: kp_child owns its GSubprocess; the text passed to on_exit is only
 * valid during the call. */

typedef struct kp_child kp_child;

typedef void (*kp_child_exit_cb)(void *ud, const char *why);

/* flags: G_SUBPROCESS_FLAGS_STDIN_PIPE etc. NULL with *err (g_free it). */
kp_child *kp_child_spawn(const char *const *argv, GSubprocessFlags flags, kp_child_exit_cb on_exit,
                         void *ud, char **err);
/* SIGTERM (it should save nothing; the backends hold no state), no more
 * callbacks. */
void kp_child_stop(kp_child *c);
GSubprocess *kp_child_proc(kp_child *c);
int kp_child_pid(const kp_child *c);

#endif
