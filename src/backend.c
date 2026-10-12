/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "backend_impl.h"

/* The public calls, forwarded to the implementation. */

const char *kp_backend_name(const kp_backend *b)
{
    return b->ops->name;
}

unsigned kp_backend_caps(const kp_backend *b)
{
    return b->ops->caps;
}

void kp_backend_free(kp_backend *b)
{
    if (b)
        b->ops->free(b);
}

bool kp_backend_start(kp_backend *b, char **err)
{
    return b->ops->start(b, err);
}

bool kp_backend_running(const kp_backend *b)
{
    return b->ops->running(b);
}

int kp_backend_pid(const kp_backend *b)
{
    return b->ops->pid(b);
}

void kp_backend_load(kp_backend *b, const char *path, double start)
{
    b->ops->load(b, path, start);
}

void kp_backend_queue(kp_backend *b, const char *path)
{
    if (b->ops->queue)
        b->ops->queue(b, path);
}

void kp_backend_stop(kp_backend *b)
{
    b->ops->stop(b);
}

void kp_backend_set_pause(kp_backend *b, bool paused)
{
    b->ops->set_pause(b, paused);
}

void kp_backend_seek(kp_backend *b, double seconds)
{
    b->ops->seek(b, seconds, false);
}

void kp_backend_seek_relative(kp_backend *b, double seconds)
{
    b->ops->seek(b, seconds, true);
}

void kp_backend_set_volume(kp_backend *b, int volume)
{
    b->ops->set_volume(b, volume);
}

void kp_backend_set_ticking(kp_backend *b, bool on)
{
    b->ops->set_ticking(b, on);
}
