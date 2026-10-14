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

void kp_backend_load(kp_backend *b, const char *path, const kp_load_opts *lo)
{
    static const kp_load_opts plain = KP_LOAD_OPTS_INIT;
    b->ops->load(b, path, lo ? lo : &plain);
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

void kp_backend_set_track(kp_backend *b, kp_track_type type, int id)
{
    if (b->ops->set_track)
        b->ops->set_track(b, type, id);
}

void kp_backend_set_delay(kp_backend *b, kp_track_type type, double seconds)
{
    if (b->ops->set_delay)
        b->ops->set_delay(b, type, seconds);
}

void kp_backend_set_aspect(kp_backend *b, const char *aspect)
{
    if (b->ops->set_aspect)
        b->ops->set_aspect(b, aspect);
}

void kp_backend_show_text(kp_backend *b, const char *text)
{
    if (b->ops->show_text)
        b->ops->show_text(b, text);
}

void kp_backend_overlay(kp_backend *b, const char *path, int x, int y, int w, int h, int stride)
{
    if (b->ops->overlay)
        b->ops->overlay(b, path, x, y, w, h, stride);
}

void kp_backend_set_margin_right(kp_backend *b, double ratio)
{
    if (b->ops->set_margin_right)
        b->ops->set_margin_right(b, ratio);
}
