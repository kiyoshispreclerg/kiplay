/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_UI_WINDOW_H
#define KP_UI_WINDOW_H

#include <stdbool.h>

/* The main window: menu bar, the playlists in tabs, the video area and the
 * transport bar in the configured layout, and a status line. Size and
 * layout are remembered in kiplay.ini; the playlists in
 * ~/.config/kiplay/playlists/. */

typedef struct {
    /* NULL = the default ~/.config/kiplay/kiplay.ini. */
    const char *prefs_path;
    /* Quits right after the window is first drawn (startup timing, tests). */
    bool exit_after_map;
    /* Files to add (and play, unless enqueue) once the window is up. */
    char **files;
    unsigned flags; /* KP_OPEN_* */
    /* The single-instance socket to listen on (kp_instance), or NULL. */
    const char *instance_socket;
    /* g_get_monotonic_time() when main started, for the -v startup time. */
    long long start_us;
} kp_window_opts;

/* gtk_init must have been called. Runs the main loop; returns the process
 * exit code. */
int kp_window_run(const kp_window_opts *opts);

#endif
