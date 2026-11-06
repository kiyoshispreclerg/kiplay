/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <glib.h>
#include <glib/gstdio.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "../harness.h"
#include "backend.h"
#include "mpvopts.h"
#include "prefs.h"

/* MPlayer in slave mode: the real program, since what it prints is the
 * whole interface. */

#define MAX_EVENTS 64

typedef struct {
    kp_event ev[MAX_EVENTS];
    char text[MAX_EVENTS][128];
    int n;
} recorder;

static void record(void *ud, const kp_event *ev)
{
    recorder *r = ud;
    if (r->n >= MAX_EVENTS)
        return;
    r->ev[r->n] = *ev;
    if (ev->text) {
        g_strlcpy(r->text[r->n], ev->text, sizeof r->text[0]);
        r->ev[r->n].text = r->text[r->n];
    }
    r->n++;
}

typedef struct {
    GMainLoop *loop;
    recorder r;
    kp_event_type wait_for;
    bool got;
} session;

static void on_event(void *ud, const kp_event *ev)
{
    session *s = ud;
    record(&s->r, ev);
    if (ev->type == s->wait_for) {
        s->got = true;
        g_main_loop_quit(s->loop);
    }
}

static gboolean on_timeout(gpointer data)
{
    g_main_loop_quit(data);
    return G_SOURCE_REMOVE;
}

/* Runs the loop until an event of that type comes (true) or ms pass. */
static bool wait_event(session *s, kp_event_type type, int ms)
{
    s->wait_for = type;
    s->got = false;
    guint t = g_timeout_add(ms, on_timeout, s->loop);
    g_main_loop_run(s->loop);
    if (s->got)
        g_source_remove(t);
    return s->got;
}

static const kp_event *find(const recorder *r, kp_event_type type)
{
    for (int i = r->n - 1; i >= 0; i--)
        if (r->ev[i].type == type)
            return &r->ev[i];
    return NULL;
}

/* 3 s of a quiet 440 Hz tone, 16-bit mono 8 kHz. */
static void write_wav(const char *path)
{
    const int rate = 8000, n = rate * 3;
    FILE *f = fopen(path, "wb");
    unsigned char h[44] = "RIFF____WAVEfmt ";
    unsigned data = (unsigned)n * 2, riff = 36 + data;
    memcpy(h + 4, &riff, 4);
    unsigned fmt_len = 16, byte_rate = (unsigned)rate * 2;
    unsigned short pcm = 1, ch = 1, align = 2, bits = 16;
    memcpy(h + 16, &fmt_len, 4);
    memcpy(h + 20, &pcm, 2);
    memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    fwrite(h, 1, sizeof h, f);
    for (int i = 0; i < n; i++) {
        short v = (short)(3000 * sin(2 * M_PI * 440 * i / rate));
        fwrite(&v, 2, 1, f);
    }
    fclose(f);
}

TEST(plays_with_real_mplayer)
{
    char *mp = g_find_program_in_path("mplayer");
    if (!mp) {
        fprintf(stderr, "  (mplayer not installed: skipped)\n");
        return;
    }
    g_free(mp);
    char *dir = g_dir_make_tmp("kp-test-XXXXXX", NULL);
    char *wav = g_build_filename(dir, "tom ação \"x\".wav", NULL);
    write_wav(wav);

    session s = {.loop = g_main_loop_new(NULL, FALSE)};
    static const char *const test_args[] = {"-ao", "null", "-vo", "null", NULL};
    kp_backend_opts o = {.volume = 50, .test_args = test_args};
    kp_backend *b = kp_backend_new_mplayer(&o, on_event, &s);
    CHECK_STR_EQ(kp_backend_name(b), "mplayer");
    CHECK(kp_backend_caps(b) & KP_CAP_VIDEO);
    CHECK(!(kp_backend_caps(b) & (KP_CAP_TRACKS | KP_CAP_GAPLESS)));
    char *err = NULL;
    CHECK(kp_backend_start(b, &err));
    kp_backend_set_ticking(b, true);

    /* Played to the end by itself; quoting in the name survives. */
    kp_backend_load(b, wav, NULL);
    CHECK(wait_event(&s, KP_EV_LOADED, 10000));
    const kp_event *e = find(&s.r, KP_EV_DURATION);
    CHECK(e && fabs(e->value - 3.0) < 0.1);
    e = find(&s.r, KP_EV_VIDEO);
    CHECK(e && !e->flag);
    e = find(&s.r, KP_EV_TITLE);
    CHECK(e && strstr(e->text, "ação"));
    CHECK(wait_event(&s, KP_EV_TIME, 3000));
    CHECK(wait_event(&s, KP_EV_ENDED, 10000));
    CHECK(!find(&s.r, KP_EV_ENDED)->flag);

    /* Pause holds the time still; seeking while paused keeps the pause. */
    s.r.n = 0;
    kp_backend_load(b, wav, NULL);
    CHECK(wait_event(&s, KP_EV_LOADED, 10000));
    kp_backend_set_pause(b, true);
    CHECK(find(&s.r, KP_EV_PAUSED)->flag); /* said at once */
    CHECK(wait_event(&s, KP_EV_TIME, 3000));
    kp_backend_seek(b, 2.0);
    s.r.n = 0;
    wait_event(&s, KP_EV_ENDED, 1200); /* it must not end: it is paused */
    CHECK(find(&s.r, KP_EV_ENDED) == NULL);
    e = find(&s.r, KP_EV_TIME);
    CHECK(e && e->value >= 1.9 && e->value < 2.3);
    kp_backend_set_pause(b, false);
    CHECK(wait_event(&s, KP_EV_ENDED, 5000)); /* the last second */

    /* A missing file ends with an error; the next file is not taken for it. */
    s.r.n = 0;
    char *missing = g_build_filename(dir, "nope.mp3", NULL);
    kp_backend_load(b, missing, NULL);
    CHECK(wait_event(&s, KP_EV_ENDED, 10000));
    CHECK(find(&s.r, KP_EV_ENDED)->flag);
    g_free(missing);
    s.r.n = 0;
    kp_backend_load(b, wav, &(kp_load_opts){.start = 2.0, .aid = -1, .sid = -1});
    CHECK(wait_event(&s, KP_EV_LOADED, 10000));
    wait_event(&s, KP_EV_ENDED, 700); /* no stale end from the earlier poll */
    CHECK(find(&s.r, KP_EV_ENDED) == NULL);
    CHECK(wait_event(&s, KP_EV_ENDED, 5000));
    e = find(&s.r, KP_EV_TIME);
    CHECK(e && e->value >= 1.9);

    /* stop() leaves nothing to end. */
    s.r.n = 0;
    kp_backend_load(b, wav, NULL);
    CHECK(wait_event(&s, KP_EV_LOADED, 10000));
    kp_backend_stop(b);
    wait_event(&s, KP_EV_ENDED, 800);
    CHECK(find(&s.r, KP_EV_ENDED) == NULL);

    /* If it dies, we hear about it and can start it again. */
    s.r.n = 0;
    CHECK(kp_backend_pid(b) > 0);
    CHECK(kill(kp_backend_pid(b), SIGKILL) == 0);
    CHECK(wait_event(&s, KP_EV_CRASHED, 5000));
    CHECK(!kp_backend_running(b));
    CHECK(kp_backend_start(b, &err));
    kp_backend_load(b, wav, NULL);
    CHECK(wait_event(&s, KP_EV_LOADED, 10000));

    kp_backend_free(b);
    g_main_loop_unref(s.loop);
    g_remove(wav);
    g_rmdir(dir);
    g_free(wav);
    g_free(dir);
}

/* No mpv to run: MPlayer takes over, but only if it can run itself. */
TEST(falls_back_when_mpv_is_missing)
{
    char *dir = g_dir_make_tmp("kp-test-XXXXXX", NULL);
    char *ini = g_build_filename(dir, "kiplay.ini", NULL);
    g_file_set_contents(ini, "[mpv]\nprogram = /nonexistent/mpv\n[mplayer]\nprogram = /bin/true\n",
                        -1, NULL);
    kp_prefs *p = kp_prefs_load(ini);
    CHECK_STR_EQ(kp_video_backend(p), "mplayer");
    CHECK(kp_video_fallback(p));
    kp_prefs_free(p);
    g_remove(ini);
    g_rmdir(dir);
    g_free(ini);
    g_free(dir);
}

int main(void)
{
    RUN(falls_back_when_mpv_is_missing);
    RUN(plays_with_real_mplayer);
    return harness_report();
}
