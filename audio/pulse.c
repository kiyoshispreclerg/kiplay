/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pulse.h"

#include <pulse/pulseaudio.h>
#include <stdio.h>
#include <stdlib.h>

#define FRAME_BYTES 4 /* S16 stereo */
/* Connecting gives up after this (a server that's there but stuck). */
#define CONNECT_TIMEOUT_US 2000000
/* prebuf asked for: this part of the buffer. */
#define PREBUF_PART 10

struct ka_pulse {
    pa_mainloop *ml;
    pa_context *ctx;
    pa_stream *st;
    int rate;
    long period;
};

/* What came from the server, without waiting for more. */
static void pump(ka_pulse *p)
{
    while (pa_mainloop_iterate(p->ml, 0, NULL) > 0)
        ;
}

static bool alive(const ka_pulse *p)
{
    return pa_context_get_state(p->ctx) == PA_CONTEXT_READY &&
           pa_stream_get_state(p->st) == PA_STREAM_READY;
}

/* Until the operation is done (or the connection breaks). */
static void finish(ka_pulse *p, pa_operation *op)
{
    if (!op)
        return;
    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING &&
           pa_context_get_state(p->ctx) == PA_CONTEXT_READY)
        if (pa_mainloop_iterate(p->ml, 1, NULL) < 0)
            break;
    pa_operation_unref(op);
}

static pa_cvolume cubic(int volume)
{
    double g = volume / 100.0;
    pa_cvolume cv;
    pa_cvolume_set(&cv, 2, pa_sw_volume_from_linear(g * g * g));
    return cv;
}

ka_pulse *ka_pulse_open(int rate, int buffer_ms, char *err, int errsize)
{
    ka_pulse *p = calloc(1, sizeof *p);
    if (!p || !(p->ml = pa_mainloop_new())) {
        snprintf(err, (size_t)errsize, "out of memory");
        free(p);
        return NULL;
    }
    p->rate = rate;
    pa_proplist *props = pa_proplist_new();
    pa_proplist_sets(props, PA_PROP_APPLICATION_NAME, "Kiplay");
    pa_proplist_sets(props, PA_PROP_APPLICATION_ID, "kiplay");
    pa_proplist_sets(props, PA_PROP_APPLICATION_ICON_NAME, "kiplay");
    p->ctx = pa_context_new_with_proplist(pa_mainloop_get_api(p->ml), "Kiplay", props);
    if (!p->ctx || pa_context_connect(p->ctx, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0)
        goto fail;
    /* Connecting: the only wait with a limit of our own. */
    for (int waited = 0;; waited += 100000) {
        pa_context_state_t s = pa_context_get_state(p->ctx);
        if (s == PA_CONTEXT_READY)
            break;
        if (!PA_CONTEXT_IS_GOOD(s) || waited >= CONNECT_TIMEOUT_US)
            goto fail;
        if (pa_mainloop_prepare(p->ml, 100000) < 0 || pa_mainloop_poll(p->ml) < 0 ||
            pa_mainloop_dispatch(p->ml) < 0)
            goto fail;
    }

    pa_sample_spec ss = {.format = PA_SAMPLE_S16NE, .rate = (uint32_t)rate, .channels = 2};
    pa_proplist_sets(props, PA_PROP_MEDIA_ROLE, "music");
    p->st = pa_stream_new_with_proplist(p->ctx, "Music", &ss, NULL, props);
    if (!p->st)
        goto fail;
    /* As Audacious: the whole latency is buffer_ms (ADJUST_LATENCY lets
     * the server split it between us and the card), and the server
     * interpolates the clock between its timing updates.
     *
     * But a small prebuf, not the server's (nearly all of tlength): that
     * one assumes we write whenever minreq fits, and we write a period at
     * a time. After an underrun, with a short write in between (a file's
     * last one), the buffer could stop above tlength - period and below
     * prebuf: the server waiting for more, we for room, forever. Below a
     * small prebuf there's always room for a period (checked below). */
    pa_buffer_attr attr = {
        .maxlength = (uint32_t)-1,
        .tlength = (uint32_t)pa_usec_to_bytes((pa_usec_t)buffer_ms * 1000, &ss),
        .prebuf = (uint32_t)pa_usec_to_bytes((pa_usec_t)buffer_ms * 1000 / PREBUF_PART, &ss),
        .minreq = (uint32_t)-1,
        .fragsize = (uint32_t)-1,
    };
    pa_stream_flags_t flags =
        PA_STREAM_ADJUST_LATENCY | PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE;
    if (pa_stream_connect_playback(p->st, NULL, &attr, flags, NULL, NULL) < 0)
        goto fail;
    for (;;) {
        pa_stream_state_t s = pa_stream_get_state(p->st);
        if (s == PA_STREAM_READY)
            break;
        if (!PA_STREAM_IS_GOOD(s) || pa_mainloop_iterate(p->ml, 1, NULL) < 0)
            goto fail;
    }
    /* Write in halves of what the server keeps for us. */
    const pa_buffer_attr *got = pa_stream_get_buffer_attr(p->st);
    p->period = got ? (long)(got->tlength / FRAME_BYTES / 2) : rate / 4;
    if (got && p->period < (long)(got->minreq / FRAME_BYTES))
        p->period = (long)(got->minreq / FRAME_BYTES);
    /* The server asks for room in minreq steps: below prebuf, there's room
     * for a period only if prebuf + period + minreq fit in tlength. With
     * a server that cut tlength much shorter, smaller writes. */
    if (got) {
        long minreq = (long)(got->minreq / FRAME_BYTES);
        long fit = (long)(got->tlength / FRAME_BYTES) - (long)(got->prebuf / FRAME_BYTES) - minreq;
        if (p->period > fit)
            p->period = fit > minreq ? fit : minreq;
    }
    pa_proplist_free(props);
    return p;

fail:
    snprintf(err, (size_t)errsize, "pulse: %s",
             p->ctx ? pa_strerror(pa_context_errno(p->ctx)) : "cannot connect");
    pa_proplist_free(props);
    ka_pulse_close(p);
    return NULL;
}

void ka_pulse_close(ka_pulse *p)
{
    if (!p)
        return;
    if (p->st) {
        pa_stream_disconnect(p->st);
        pa_stream_unref(p->st);
    }
    if (p->ctx) {
        pa_context_disconnect(p->ctx);
        pa_context_unref(p->ctx);
    }
    pa_mainloop_free(p->ml);
    free(p);
}

long ka_pulse_period(const ka_pulse *p)
{
    return p->period;
}

long ka_pulse_room(ka_pulse *p)
{
    pump(p);
    if (!alive(p))
        return -1;
    size_t n = pa_stream_writable_size(p->st);
    return n == (size_t)-1 ? -1 : (long)(n / FRAME_BYTES);
}

bool ka_pulse_write(ka_pulse *p, const int16_t *frames, long n)
{
    if (!alive(p))
        return false;
    if (pa_stream_write(p->st, frames, (size_t)n * FRAME_BYTES, NULL, 0, PA_SEEK_RELATIVE) < 0)
        return false;
    pump(p);
    return true;
}

long ka_pulse_delay(ka_pulse *p)
{
    pump(p);
    pa_usec_t us;
    int negative;
    if (!alive(p) || pa_stream_get_latency(p->st, &us, &negative) < 0 || negative)
        return 0;
    return (long)(us * (pa_usec_t)p->rate / 1000000);
}

void ka_pulse_drop(ka_pulse *p)
{
    if (alive(p))
        finish(p, pa_stream_flush(p->st, NULL, NULL));
}

void ka_pulse_drain(ka_pulse *p)
{
    if (alive(p))
        finish(p, pa_stream_drain(p->st, NULL, NULL));
}

bool ka_pulse_pause(ka_pulse *p, bool on)
{
    if (!alive(p))
        return false;
    finish(p, pa_stream_cork(p->st, on, NULL, NULL));
    return true;
}

bool ka_pulse_set_volume(ka_pulse *p, int volume)
{
    if (!alive(p))
        return false;
    pa_cvolume cv = cubic(volume);
    pa_operation *op = pa_context_set_sink_input_volume(p->ctx, pa_stream_get_index(p->st), &cv,
                                                        NULL, NULL);
    if (op)
        pa_operation_unref(op); /* sent; no need to wait for the answer */
    pump(p);
    return true;
}
