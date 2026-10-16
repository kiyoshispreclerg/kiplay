/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "out.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef KA_HAVE_PULSE
#include "pulse.h"
#endif

/* Half a second of buffer, written a period (half of it) at a time: enough
 * to ride out a slow decode, few wakeups, and seek and pause don't feel it
 * (they drop or hold the buffer). */
#define BUFFER_MS 500
#define PERIODS 2

struct ka_out {
#ifdef KA_HAVE_PULSE
    ka_pulse *pulse; /* when set, none of the ALSA side below */
#endif
    snd_pcm_t *pcm;
    int rate;
    long period, buffer;
    bool null_sink;
    bool can_pause;
    /* null sink only: when the written frames would have been heard. */
    struct timespec clock;
    long fake_delay; /* frames "buffered" when paused */
    bool fake_paused;
};

/* Interleaved S16 stereo at that rate (ALSA's plug converts if the card
 * can't), BUFFER_MS of buffer in PERIODS periods; a period of room wakes
 * the engine (avail_min). */
static int set_params(ka_out *o)
{
    snd_pcm_hw_params_t *hw; /* not the _alloca macros: alloca isn't C11 */
    snd_pcm_sw_params_t *sw = NULL;
    int r = snd_pcm_hw_params_malloc(&hw);
    if (r < 0)
        return r;
    unsigned buffer = BUFFER_MS * 1000, period = buffer / PERIODS;
    unsigned rate = (unsigned)o->rate;
    snd_pcm_uframes_t psize, bsize;
    if ((r = snd_pcm_hw_params_any(o->pcm, hw)) < 0 ||
        (r = snd_pcm_hw_params_set_access(o->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (r = snd_pcm_hw_params_set_format(o->pcm, hw, SND_PCM_FORMAT_S16)) < 0 ||
        (r = snd_pcm_hw_params_set_channels(o->pcm, hw, KA_CHANNELS)) < 0 ||
        (r = snd_pcm_hw_params_set_rate_resample(o->pcm, hw, 1)) < 0 ||
        (r = snd_pcm_hw_params_set_rate_near(o->pcm, hw, &rate, NULL)) < 0 ||
        (r = snd_pcm_hw_params_set_buffer_time_near(o->pcm, hw, &buffer, NULL)) < 0 ||
        (r = snd_pcm_hw_params_set_period_time_near(o->pcm, hw, &period, NULL)) < 0 ||
        (r = snd_pcm_hw_params(o->pcm, hw)) < 0 ||
        (r = snd_pcm_hw_params_get_period_size(hw, &psize, NULL)) < 0 ||
        (r = snd_pcm_hw_params_get_buffer_size(hw, &bsize)) < 0)
        goto out;
    o->period = (long)psize;
    o->buffer = (long)bsize;
    o->can_pause = snd_pcm_hw_params_can_pause(hw);
    /* Start as soon as there's something: the engine fills it at once. */
    if ((r = snd_pcm_sw_params_malloc(&sw)) < 0 || (r = snd_pcm_sw_params_current(o->pcm, sw)) < 0 ||
        (r = snd_pcm_sw_params_set_avail_min(o->pcm, sw, psize)) < 0 ||
        (r = snd_pcm_sw_params_set_start_threshold(o->pcm, sw, psize)) < 0)
        goto out;
    r = snd_pcm_sw_params(o->pcm, sw);
out:
    snd_pcm_sw_params_free(sw);
    snd_pcm_hw_params_free(hw);
    return r;
}

static ka_out *open_alsa(ka_out *o, const char *device, char *err, int errsize)
{
    int r = snd_pcm_open(&o->pcm, device, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (r >= 0)
        r = set_params(o);
    if (r < 0) {
        snprintf(err, (size_t)errsize, "%s: %s", device, snd_strerror(r));
        ka_out_close(o);
        return NULL;
    }
    o->null_sink = strcmp(device, "null") == 0;
    clock_gettime(CLOCK_MONOTONIC, &o->clock);
    return o;
}

ka_out *ka_out_open(const char *device, int rate, char *err, int errsize)
{
    ka_out *o = calloc(1, sizeof *o);
    if (!o) {
        snprintf(err, (size_t)errsize, "out of memory");
        return NULL;
    }
    o->rate = rate;
#ifdef KA_HAVE_PULSE
    if (!strcmp(device, "default") || !strcmp(device, "pulse")) {
        o->pulse = ka_pulse_open(rate, BUFFER_MS, err, errsize);
        if (o->pulse) {
            o->period = ka_pulse_period(o->pulse);
            return o;
        }
        /* No server: ALSA's default is the card itself. */
        if (!strcmp(device, "pulse")) {
            free(o);
            return NULL;
        }
    }
#endif
    return open_alsa(o, device, err, errsize);
}

void ka_out_close(ka_out *o)
{
    if (!o)
        return;
#ifdef KA_HAVE_PULSE
    ka_pulse_close(o->pulse);
#endif
    if (o->pcm)
        snd_pcm_close(o->pcm);
    free(o);
}

int ka_out_rate(const ka_out *o)
{
    return o->rate;
}

long ka_out_period(const ka_out *o)
{
    return o->period;
}

static void advance(struct timespec *t, long frames, int rate)
{
    long long ns = t->tv_nsec + frames * 1000000000LL / rate;
    t->tv_sec += (time_t)(ns / 1000000000LL);
    t->tv_nsec = (long)(ns % 1000000000LL);
}

/* The null sink "plays" in real time from o->clock. */
static long null_delay(ka_out *o)
{
    if (o->fake_paused)
        return o->fake_delay;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long ns = (o->clock.tv_sec - now.tv_sec) * 1000000000LL + (o->clock.tv_nsec - now.tv_nsec);
    return ns > 0 ? (long)(ns * o->rate / 1000000000LL) : 0;
}

long ka_out_room(ka_out *o)
{
#ifdef KA_HAVE_PULSE
    if (o->pulse)
        return ka_pulse_room(o->pulse);
#endif
    if (o->null_sink)
        return o->buffer - null_delay(o);
    snd_pcm_sframes_t n = snd_pcm_avail_update(o->pcm);
    if (n < 0) {
        /* Underrun (-EPIPE) or suspend: recover and go on. */
        if (snd_pcm_recover(o->pcm, (int)n, 1) < 0)
            return -1;
        n = snd_pcm_avail_update(o->pcm);
    }
    return n < 0 ? -1 : (long)n;
}

long long ka_out_wait_ns(ka_out *o, long frames)
{
    long room = ka_out_room(o);
    if (room < 0 || room >= frames)
        return 0;
    return (long long)(frames - room) * 1000000000LL / o->rate;
}

bool ka_out_write(ka_out *o, const int16_t *frames, long n)
{
#ifdef KA_HAVE_PULSE
    if (o->pulse)
        return ka_pulse_write(o->pulse, frames, n);
#endif
    if (o->null_sink) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > o->clock.tv_sec ||
            (now.tv_sec == o->clock.tv_sec && now.tv_nsec > o->clock.tv_nsec))
            o->clock = now; /* fell behind (paused, idle): start from now */
        advance(&o->clock, n, o->rate);
    }
    while (n > 0) {
        snd_pcm_sframes_t w = snd_pcm_writei(o->pcm, frames, (snd_pcm_uframes_t)n);
        if (w == -EAGAIN) {
            /* Less room than it said (the null sink never does this). */
            snd_pcm_wait(o->pcm, 100);
            continue;
        }
        if (w < 0) {
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
#ifdef KA_HAVE_PULSE
    if (o->pulse)
        return ka_pulse_delay(o->pulse);
#endif
    if (o->null_sink)
        return null_delay(o);
    snd_pcm_sframes_t d;
    if (snd_pcm_delay(o->pcm, &d) < 0 || d < 0)
        return 0;
    return (long)d;
}

void ka_out_drop(ka_out *o)
{
#ifdef KA_HAVE_PULSE
    if (o->pulse) {
        ka_pulse_drop(o->pulse);
        return;
    }
#endif
    snd_pcm_drop(o->pcm);
    snd_pcm_prepare(o->pcm);
    clock_gettime(CLOCK_MONOTONIC, &o->clock);
    o->fake_paused = false;
}

void ka_out_drain(ka_out *o)
{
#ifdef KA_HAVE_PULSE
    if (o->pulse) {
        ka_pulse_drain(o->pulse);
        return;
    }
#endif
    if (o->null_sink) {
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &o->clock, NULL);
    } else {
        /* Draining is the one wait we want. */
        snd_pcm_nonblock(o->pcm, 0);
        snd_pcm_drain(o->pcm);
        snd_pcm_nonblock(o->pcm, 1);
    }
    snd_pcm_prepare(o->pcm);
}

bool ka_out_pause(ka_out *o, bool on)
{
#ifdef KA_HAVE_PULSE
    if (o->pulse)
        return ka_pulse_pause(o->pulse, on);
#endif
    if (o->null_sink) {
        /* Freeze the clock: what's "buffered" stays buffered. */
        if (on) {
            o->fake_delay = null_delay(o);
            o->fake_paused = true;
        } else {
            o->fake_paused = false;
            clock_gettime(CLOCK_MONOTONIC, &o->clock);
            advance(&o->clock, o->fake_delay, o->rate);
        }
        return true;
    }
    if (!o->can_pause)
        return false;
    return snd_pcm_pause(o->pcm, on ? 1 : 0) == 0;
}

bool ka_out_set_volume(ka_out *o, int volume)
{
#ifdef KA_HAVE_PULSE
    if (o->pulse)
        return ka_pulse_set_volume(o->pulse, volume);
#endif
    (void)o;
    (void)volume;
    return false;
}
