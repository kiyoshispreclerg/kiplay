/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_LINEIO_H
#define KP_LINEIO_H

#include <gio/gio.h>

/* Line-based talk with a child process on the GLib main loop, without ever
 * blocking: lines are read asynchronously, and written with non-blocking
 * writes plus a queue (which also holds what is sent before the channel
 * exists, e.g. while mpv hasn't opened its socket yet).
 *
 * Memory: kp_lineio refs the streams it is attached to; the line passed to
 * the callback is only valid during it. */

typedef struct kp_lineio kp_lineio;

/* line = NULL: the other side closed (or the stream failed). The callback
 * may detach or re-attach, but not free, the kp_lineio. */
typedef void (*kp_line_cb)(void *ud, const char *line);

kp_lineio *kp_lineio_new(const char *name, kp_line_cb cb, void *ud);
/* Stops reading, drops the streams and anything not yet written. */
void kp_lineio_free(kp_lineio *io);

/* Starts reading from in and writing to out (a GPollableOutputStream:
 * sockets and pipes are). Queued lines go out now. */
void kp_lineio_attach(kp_lineio *io, GInputStream *in, GOutputStream *out);
/* Back to unattached; the queue is emptied. */
void kp_lineio_detach(kp_lineio *io);

/* Queues line plus '\n'. */
void kp_lineio_send(kp_lineio *io, const char *line);

#endif
