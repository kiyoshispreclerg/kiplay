/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "out.h"

#include <alsa/asoundlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* 250 ms of buffer: enough to ride out a slow decode (game music
 * emulation, a busy disk), short enough that pause and seek feel instant. */
#define BUFFER_US 250000

struct ka_out {
    snd_pcm_t *pcm;
    bool null_sink;
    bool can_pause;
    /* null sink only: when the written frames would have been heard. */
    struct timespec clock;
    long fake_delay; /* frames "buffered" when paused */
    bool fake_paused;
};

ka_out *ka_out_open(const char *device, char *err, int errsize)
{
    snd_pcm_t *pcm;
    int r = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0);
    if (r < 0) {
        snprintf(err, (size_t)errsize, "%s: %s", device, snd_strerror(r));
        return NULL;
    }
    r = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED, KA_CHANNELS,
                           KA_RATE, 1, BUFFER_US);
    if (r < 0) {
        snprintf(err, (size_t)errsize, "%s: %s", device, snd_strerror(r));
        snd_pcm_close(pcm);
        return NULL;
    }
    ka_out *o = calloc(1, sizeof *o);
    if (!o) {
        snd_pcm_close(pcm);
        snprintf(err, (size_t)errsize, "out of memory");
        return NULL;
    }
    o->pcm = pcm;
    o->null_sink = strcmp(device, "null") == 0;
    snd_pcm_hw_params_t *hw; /* not the _alloca macro: alloca isn't C11 */
    if (snd_pcm_hw_params_malloc(&hw) == 0) {
        o->can_pause = snd_pcm_hw_params_current(pcm, hw) == 0 && snd_pcm_hw_params_can_pause(hw);
        snd_pcm_hw_params_free(hw);
    }
    clock_gettime(CLOCK_MONOTONIC, &o->clock);
    return o;
}

void ka_out_close(ka_out *o)
{
    if (!o)
        return;
    snd_pcm_close(o->pcm);
    free(o);
}

static void advance(struct timespec *t, long frames)
{
    long long ns = t->tv_nsec + frames * 1000000000LL / KA_RATE;
    t->tv_sec += (time_t)(ns / 1000000000LL);
    t->tv_nsec = (long)(ns % 1000000000LL);
}

bool ka_out_write(ka_out *o, const int16_t *frames, long n)
{
    if (o->null_sink) {
        /* Sleep until what was written before has "played", keeping about
         * a buffer's worth ahead, like a card. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > o->clock.tv_sec ||
            (now.tv_sec == o->clock.tv_sec && now.tv_nsec > o->clock.tv_nsec))
            o->clock = now; /* fell behind (paused, idle): start from now */
        struct timespec wake = o->clock;
        advance(&wake, -(long)((long long)KA_RATE * BUFFER_US / 1000000));
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake, NULL);
        advance(&o->clock, n);
    }
    while (n > 0) {
        snd_pcm_sframes_t w = snd_pcm_writei(o->pcm, frames, (snd_pcm_uframes_t)n);
        if (w < 0) {
            /* Underrun (-EPIPE) or suspend: recover and go on. */
            if (snd_pcm_recover(o->pcm, (int)w, 1) < 0)
                return false;
            continue;
        }
        frames += w * KA_CHANNELS;
        n -= w;
    }
    return true;
}

long ka_out_delay(ka_out *o)
{
    if (o->null_sink && o->fake_paused)
        return o->fake_delay;
    if (o->null_sink) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long ns = (o->clock.tv_sec - now.tv_sec) * 1000000000LL + (o->clock.tv_nsec - now.tv_nsec);
        return ns > 0 ? (long)(ns * KA_RATE / 1000000000LL) : 0;
    }
    snd_pcm_sframes_t d;
    if (snd_pcm_delay(o->pcm, &d) < 0 || d < 0)
        return 0;
    return (long)d;
}

void ka_out_drop(ka_out *o)
{
    snd_pcm_drop(o->pcm);
    snd_pcm_prepare(o->pcm);
    clock_gettime(CLOCK_MONOTONIC, &o->clock);
    o->fake_paused = false;
}

void ka_out_drain(ka_out *o)
{
    if (o->null_sink) {
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &o->clock, NULL);
    } else {
        snd_pcm_drain(o->pcm);
    }
    snd_pcm_prepare(o->pcm);
}

bool ka_out_pause(ka_out *o, bool on)
{
    if (o->null_sink) {
        /* Freeze the clock: what's "buffered" stays buffered. */
        if (on) {
            o->fake_delay = ka_out_delay(o);
            o->fake_paused = true;
        } else {
            o->fake_paused = false;
            clock_gettime(CLOCK_MONOTONIC, &o->clock);
            advance(&o->clock, o->fake_delay);
        }
        return true;
    }
    if (!o->can_pause)
        return false;
    return snd_pcm_pause(o->pcm, on ? 1 : 0) == 0;
}
