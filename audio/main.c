/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * kiplay-audio: Kiplay's music engine, a small process of its own so that
 * a file that crashes or hangs a decoder never takes the window down
 * (Kiplay sees the pipe close and starts it again).
 *
 * Two threads: this one reads commands from stdin; the engine thread
 * decodes and writes to ALSA, paced by the device. Requests go from one to
 * the other through `shared` under its mutex; events go to stdout, one
 * line each (cmd.h has the protocol).
 *
 *   kiplay-audio [-d ALSA_DEVICE] */
#include <libavformat/avformat.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cmd.h"
#include "dec.h"
#include "out.h"

/* ~21 ms per write: commands are seen at least this often while playing. */
#define CHUNK 1024
#define TICK_NS 250000000LL
/* The device is let go after this long without sound. */
#define IDLE_CLOSE_S 3

static const char *device = "default";

/* ---- requests from the command thread ------------------------------------ */

static struct {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    char *load; /* path to load, NULL = none */
    double load_start;
    char *queue; /* "" = clear */
    bool stop, quit, ticking;
    int pause; /* -1 = no request */
    bool seek, seek_rel;
    double seek_to;
    int volume;
} shared = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .wake = PTHREAD_COND_INITIALIZER,
    .pause = -1,
    .volume = 100,
};

/* ---- events -------------------------------------------------------------- */

static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;

__attribute__((format(printf, 1, 2))) static void event(const char *fmt, ...)
{
    pthread_mutex_lock(&out_lock);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
    pthread_mutex_unlock(&out_lock);
}

/* ---- the engine thread --------------------------------------------------- */

typedef struct {
    ka_out *out;
    ka_dec *dec;
    char *queued;
    bool playing, paused;
    bool pause_by_drop; /* the device can't pause: resume seeks back */
    double base;        /* seconds where the current run of writes started */
    long long written;  /* frames written since base (negative: the
                           previous file's tail is still in the buffer) */
    int volume;
    long long last_tick_ns, idle_since_ns;
} engine;

static long long now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}

static double position(engine *e)
{
    long long heard = e->written - (e->out ? ka_out_delay(e->out) : 0);
    double p = e->base + (double)heard / KA_RATE;
    return p < 0 ? 0 : p;
}

/* mpv's curve: volume is cubic, so 50% sounds like half. */
static void apply_volume(int16_t *s, long frames, int volume)
{
    if (volume >= 100)
        return;
    double g = volume / 100.0;
    int q = (int)(g * g * g * 32768.0);
    for (long i = 0; i < frames * KA_CHANNELS; i++)
        s[i] = (int16_t)((s[i] * q) >> 15);
}

static void announce(engine *e)
{
    event("loaded");
    event("duration %.3f", ka_dec_duration(e->dec));
    const char *tags = ka_dec_tags(e->dec);
    if (tags)
        event("tags %s", tags);
}

static bool ensure_out(engine *e)
{
    if (e->out)
        return true;
    char err[256];
    e->out = ka_out_open(device, err, sizeof err);
    if (!e->out)
        event("ended error %s", err);
    return e->out != NULL;
}

static void close_dec(engine *e)
{
    ka_dec_close(e->dec);
    e->dec = NULL;
    e->playing = e->paused = false;
}

static void do_load(engine *e, const char *path, double start)
{
    close_dec(e);
    char err[256];
    e->dec = ka_dec_open(path, err, sizeof err);
    if (!e->dec) {
        event("ended error %s", err);
        return;
    }
    if (!ensure_out(e)) {
        close_dec(e);
        return;
    }
    ka_out_drop(e->out);
    e->base = 0;
    e->written = 0;
    if (start > 0 && ka_dec_seek(e->dec, start))
        e->base = start;
    e->playing = true;
    announce(e);
    event("paused 0");
}

static void do_seek(engine *e, double to)
{
    if (!e->dec)
        return;
    double d = ka_dec_duration(e->dec);
    if (to < 0)
        to = 0;
    if (d > 0 && to > d)
        to = d;
    if (!ka_dec_seek(e->dec, to))
        return;
    if (e->out)
        ka_out_drop(e->out);
    e->base = to;
    e->written = 0;
}

static void do_pause(engine *e, bool on)
{
    if (!e->dec || on == e->paused)
        return;
    if (on) {
        double at = position(e);
        e->pause_by_drop = !e->out || !ka_out_pause(e->out, true);
        if (e->pause_by_drop) {
            /* Drop the buffer; on resume we start again where it was. */
            if (e->out)
                ka_out_drop(e->out);
            e->base = at;
            e->written = 0;
        }
    } else if (e->pause_by_drop) {
        do_seek(e, e->base);
    } else {
        ka_out_pause(e->out, false);
    }
    e->paused = on;
    event("paused %d", on);
}

/* The current file ran out: the queued one, or the end. */
static void finished(engine *e)
{
    if (e->queued && *e->queued) {
        char err[256];
        ka_dec *next = ka_dec_open(e->queued, err, sizeof err);
        free(e->queued);
        e->queued = NULL;
        if (next) {
            ka_dec_close(e->dec);
            e->dec = next;
            /* The old file's tail is still in the device buffer: the new
             * one's clock starts when it actually sounds. */
            e->base = 0;
            e->written = -(e->out ? ka_out_delay(e->out) : 0);
            event("next");
            announce(e);
            return;
        }
        /* The queued file is broken: end this one normally; Kiplay will
         * try it with load and skip it. */
    }
    ka_out_drain(e->out);
    close_dec(e);
    e->idle_since_ns = now_ns();
    event("ended ok");
}

static void *engine_thread(void *arg)
{
    (void)arg;
    engine e = {.volume = 100};
    static int16_t buf[CHUNK * KA_CHANNELS];
    for (;;) {
        pthread_mutex_lock(&shared.lock);
        /* Nothing to play: sleep until a request (or time to free the
         * device, or a tick while paused). */
        while (!shared.quit && !shared.load && !shared.queue && !shared.stop &&
               shared.pause < 0 && !shared.seek && shared.volume == e.volume &&
               (!e.playing || e.paused)) {
            struct timespec t;
            clock_gettime(CLOCK_REALTIME, &t);
            t.tv_nsec += TICK_NS;
            if (t.tv_nsec >= 1000000000L) {
                t.tv_sec++;
                t.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&shared.wake, &shared.lock, &t);
            if (!e.dec && e.out && now_ns() - e.idle_since_ns > IDLE_CLOSE_S * 1000000000LL) {
                ka_out_close(e.out);
                e.out = NULL;
            }
            if (e.paused)
                break;
        }
        bool quit = shared.quit, stop = shared.stop, seek = shared.seek, rel = shared.seek_rel;
        bool ticking = shared.ticking;
        char *load = shared.load, *queue = shared.queue;
        double start = shared.load_start, to = shared.seek_to;
        int pause = shared.pause;
        e.volume = shared.volume;
        shared.load = shared.queue = NULL;
        shared.stop = shared.seek = false;
        shared.pause = -1;
        pthread_mutex_unlock(&shared.lock);

        if (quit) {
            free(load);
            free(queue);
            break;
        }
        if (queue) {
            free(e.queued);
            e.queued = queue;
        }
        if (stop) {
            if (e.out)
                ka_out_drop(e.out);
            close_dec(&e);
            e.idle_since_ns = now_ns();
        }
        if (load) {
            do_load(&e, load, start);
            free(load);
            if (!e.dec)
                e.idle_since_ns = now_ns();
        }
        if (seek)
            do_seek(&e, rel ? position(&e) + to : to);
        if (pause >= 0)
            do_pause(&e, pause != 0);

        if (e.playing && !e.paused) {
            long n = ka_dec_read(e.dec, buf, CHUNK);
            if (n > 0) {
                apply_volume(buf, n, e.volume);
                if (!ka_out_write(e.out, buf, n)) {
                    event("ended error the sound device went away");
                    close_dec(&e);
                    ka_out_close(e.out);
                    e.out = NULL;
                    continue;
                }
                e.written += n;
            } else {
                finished(&e);
            }
        }
        if (ticking && e.dec && now_ns() - e.last_tick_ns >= TICK_NS) {
            e.last_tick_ns = now_ns();
            event("time %.3f", position(&e));
        }
    }
    close_dec(&e);
    free(e.queued);
    ka_out_close(e.out);
    return NULL;
}

/* ---- the command thread -------------------------------------------------- */

static void request(const ka_cmd *c)
{
    pthread_mutex_lock(&shared.lock);
    switch (c->type) {
    case KA_CMD_LOAD:
        free(shared.load);
        shared.load = strdup(c->path);
        shared.load_start = c->num;
        /* A seek or pause meant for the old file doesn't apply. */
        shared.seek = false;
        shared.pause = -1;
        break;
    case KA_CMD_QUEUE:
        free(shared.queue);
        shared.queue = strdup(c->path);
        break;
    case KA_CMD_STOP:
        shared.stop = true;
        free(shared.load);
        shared.load = NULL;
        break;
    case KA_CMD_PAUSE:
        shared.pause = c->num != 0;
        break;
    case KA_CMD_SEEK:
    case KA_CMD_SEEKREL:
        /* Relative seeks add up if they come faster than we play. */
        if (shared.seek && shared.seek_rel && c->type == KA_CMD_SEEKREL)
            shared.seek_to += c->num;
        else
            shared.seek_to = c->num;
        shared.seek_rel = c->type == KA_CMD_SEEKREL;
        shared.seek = true;
        break;
    case KA_CMD_VOLUME:
        shared.volume = c->num < 0 ? 0 : c->num > 100 ? 100 : (int)c->num;
        break;
    case KA_CMD_TICK:
        shared.ticking = c->num != 0;
        break;
    case KA_CMD_QUIT:
        shared.quit = true;
        break;
    case KA_CMD_NONE:
        break;
    }
    pthread_cond_signal(&shared.wake);
    pthread_mutex_unlock(&shared.lock);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc)
            device = argv[++i];
        else {
            fprintf(stderr, "usage: kiplay-audio [-d ALSA_DEVICE]\n");
            return 2;
        }
    }
    /* Kiplay going away closes our stdout: let the write kill us. */
    signal(SIGPIPE, SIG_DFL);
    av_log_set_level(AV_LOG_ERROR);

    pthread_t engine_tid;
    if (pthread_create(&engine_tid, NULL, engine_thread, NULL) != 0) {
        fprintf(stderr, "kiplay-audio: cannot start the engine thread\n");
        return 1;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, stdin)) > 0) {
        if (line[n - 1] == '\n')
            line[--n] = '\0';
        ka_cmd c;
        if (!ka_cmd_parse(line, &c)) {
            fprintf(stderr, "kiplay-audio: unknown command: %s\n", line);
            continue;
        }
        request(&c);
        if (c.type == KA_CMD_QUIT)
            break;
    }
    free(line);
    /* stdin closed (Kiplay is gone) or quit. */
    ka_cmd q = {.type = KA_CMD_QUIT};
    request(&q);
    pthread_join(engine_tid, NULL);
    return 0;
}
