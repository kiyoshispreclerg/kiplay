/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_INSTANCE_H
#define KP_INSTANCE_H

#include <stdbool.h>

/* One Kiplay per settings folder. "kiplay song.mp3" while one is running
 * hands the files to it over a unix socket and exits, so opening files
 * from the file manager never stacks windows and mpv processes.
 *
 * Protocol, one connection per request, UTF-8 lines:
 *   kiplay 1 play|enqueue[ new]
 *   <absolute path or URI>
 *   ...
 *   <empty line>
 *
 * Memory: kp_instance owns its listening service; the locations passed to
 * the callback are only valid during it. */

/* What to do with the locations (kp_instance_cb, kp_instance_forward). */
enum {
    KP_OPEN_ENQUEUE = 1,      /* add them without playing */
    KP_OPEN_NEW_PLAYLIST = 2, /* into a new playlist tab, not the current one */
};

typedef void (*kp_instance_cb)(void *ud, char **locations, unsigned flags);

typedef struct kp_instance kp_instance;

/* The socket for the settings folder config_dir, in $XDG_RUNTIME_DIR
 * (different folders, e.g. in tests, get different sockets). g_free it. */
char *kp_instance_socket_path(const char *config_dir);

/* Hands the locations to a running instance: true if one took them.
 * Relative paths are made absolute first (the other process has its own
 * working directory). */
bool kp_instance_forward(const char *socket_path, char **locations, unsigned flags);

/* Listens for requests on the GLib main loop; NULL with *err if it can't
 * (a stale socket from a crash is replaced). */
kp_instance *kp_instance_listen(const char *socket_path, kp_instance_cb cb, void *ud, char **err);
void kp_instance_free(kp_instance *in);

/* What a request line turns into: an absolute path or the URI as is.
 * g_free it. Exposed for the tests. */
char *kp_instance_absolute(const char *location);

#endif
