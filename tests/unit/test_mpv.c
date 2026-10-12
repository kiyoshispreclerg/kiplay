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

/* ---- the parser, without mpv ---------------------------------------------- */

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

TEST(parses_events)
{
    recorder r = {0};
    kp_mpv_parse_line("{\"event\":\"property-change\",\"id\":1,\"name\":\"duration\",\"data\":198.5}",
                      record, &r);
    kp_mpv_parse_line("{\"event\":\"property-change\",\"id\":1,\"name\":\"duration\"}", record, &r);
    kp_mpv_parse_line("{\"event\":\"property-change\",\"id\":2,\"name\":\"pause\",\"data\":true}",
                      record, &r);
    kp_mpv_parse_line("{\"event\":\"property-change\",\"id\":3,\"name\":\"media-title\","
                      "\"data\":\"Ação \\u00e9\"}",
                      record, &r);
    kp_mpv_parse_line("{\"event\":\"file-loaded\"}", record, &r);
    kp_mpv_parse_line("{\"request_id\":100,\"error\":\"success\",\"data\":12.25}", record, &r);
    kp_mpv_parse_line("{\"event\":\"end-file\",\"reason\":\"stop\"}", record, &r);
    kp_mpv_parse_line("{\"event\":\"end-file\",\"reason\":\"eof\"}", record, &r);
    kp_mpv_parse_line("{\"event\":\"end-file\",\"reason\":\"error\",\"file_error\":\"no such file\"}",
                      record, &r);
    kp_mpv_parse_line("not json", record, &r);
    kp_mpv_parse_line("{\"event\":\"idle\"}", record, &r);

    CHECK_INT_EQ(r.n, 9);
    CHECK_INT_EQ(r.ev[0].type, KP_EV_DURATION);
    CHECK(fabs(r.ev[0].value - 198.5) < 1e-9);
    CHECK(r.ev[1].value < 0); /* unknown, e.g. a stream */
    CHECK_INT_EQ(r.ev[2].type, KP_EV_PAUSED);
    CHECK(r.ev[2].flag);
    CHECK_INT_EQ(r.ev[3].type, KP_EV_TITLE);
    CHECK_STR_EQ(r.ev[3].text, "Ação é");
    CHECK_INT_EQ(r.ev[4].type, KP_EV_LOADED);
    CHECK_INT_EQ(r.ev[5].type, KP_EV_TIME);
    CHECK(fabs(r.ev[5].value - 12.25) < 1e-9);
    /* "stop" is ignored; eof and error end the file. */
    CHECK_INT_EQ(r.ev[6].type, KP_EV_ENDED);
    CHECK(!r.ev[6].flag);
    CHECK_INT_EQ(r.ev[7].type, KP_EV_ENDED);
    CHECK(r.ev[7].flag);
    CHECK_STR_EQ(r.ev[7].text, "no such file");
    CHECK_INT_EQ(r.ev[8].type, KP_EV_IDLE);
}

/* Under a locale with a decimal comma (pt_BR), numbers must still parse. */
TEST(numbers_ignore_the_locale)
{
    const char *names[] = {"pt_BR.UTF-8", "de_DE.UTF-8", "fr_FR.UTF-8", NULL};
    const char *got = NULL;
    for (int i = 0; names[i] && !got; i++)
        got = setlocale(LC_NUMERIC, names[i]);
    if (!got) {
        fprintf(stderr, "  (no decimal-comma locale installed: skipped)\n");
        return;
    }
    recorder r = {0};
    kp_mpv_parse_line("{\"event\":\"property-change\",\"id\":1,\"data\":268.052}", record, &r);
    setlocale(LC_NUMERIC, "C");
    CHECK_INT_EQ(r.n, 1);
    CHECK(fabs(r.ev[0].value - 268.052) < 1e-9);
}

TEST(tags_make_artist_dash_title)
{
    recorder r = {0};
    kp_mpv_parse_line("{\"event\":\"property-change\",\"id\":5,\"name\":\"metadata\",\"data\":"
                      "{\"ARTIST\":\"DJ Ross\",\"TITLE\":\"Emotion\",\"album\":\"x\"}}",
                      record, &r);
    kp_mpv_parse_line("{\"event\":\"property-change\",\"id\":5,\"name\":\"metadata\",\"data\":"
                      "{\"title\":\"Só título\"}}",
                      record, &r);
    kp_mpv_parse_line("{\"event\":\"property-change\",\"id\":5,\"name\":\"metadata\",\"data\":"
                      "{\"encoder\":\"Lavf\"}}",
                      record, &r);
    CHECK_INT_EQ(r.n, 3);
    CHECK_INT_EQ(r.ev[0].type, KP_EV_TAGS);
    CHECK_STR_EQ(r.ev[0].text, "DJ Ross - Emotion");
    CHECK_STR_EQ(r.ev[1].text, "Só título");
    CHECK(r.ev[2].text == NULL);
}

TEST(cover_art_is_not_video)
{
    recorder r = {0};
    kp_mpv_parse_line("{\"event\":\"property-change\",\"id\":4,\"name\":\"track-list\",\"data\":["
                      "{\"type\":\"audio\",\"id\":1},"
                      "{\"type\":\"video\",\"id\":1,\"albumart\":true}]}",
                      record, &r);
    kp_mpv_parse_line("{\"event\":\"property-change\",\"id\":4,\"name\":\"track-list\",\"data\":["
                      "{\"type\":\"video\",\"id\":1,\"albumart\":false},"
                      "{\"type\":\"audio\",\"id\":1}]}",
                      record, &r);
    CHECK_INT_EQ(r.n, 2);
    CHECK_INT_EQ(r.ev[0].type, KP_EV_VIDEO);
    CHECK(!r.ev[0].flag);
    CHECK(r.ev[1].flag);
}

/* ---- a real mpv ------------------------------------------------------------ */

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

/* 1.5 s of a quiet 440 Hz tone, 16-bit mono 8 kHz. */
static void write_wav(const char *path)
{
    const int rate = 8000, n = rate * 3 / 2;
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

TEST(plays_with_real_mpv)
{
    char *mpv = g_find_program_in_path("mpv");
    if (!mpv) {
        fprintf(stderr, "  (mpv not installed: skipped)\n");
        return;
    }
    g_free(mpv);
    char *dir = g_dir_make_tmp("kp-test-XXXXXX", NULL);
    char *wav = g_build_filename(dir, "tom ação.wav", NULL);
    write_wav(wav);

    session s = {.loop = g_main_loop_new(NULL, FALSE)};
    static const char *const test_args[] = {"--ao=null", "--vo=null", NULL};
    kp_backend_opts o = {.volume = 50, .test_args = test_args};
    kp_backend *b = kp_backend_new_mpv(&o, on_event, &s);
    char *err = NULL;
    CHECK(kp_backend_start(b, &err));
    kp_backend_set_ticking(b, true);

    /* Commands queued before mpv listens still arrive. */
    kp_backend_load(b, wav, 0);
    CHECK(wait_event(&s, KP_EV_LOADED, 10000));
    CHECK(wait_event(&s, KP_EV_ENDED, 10000));
    const kp_event *e = find(&s.r, KP_EV_ENDED);
    CHECK(!e->flag);
    e = find(&s.r, KP_EV_DURATION);
    CHECK(e && fabs(e->value - 1.5) < 0.05);
    CHECK(find(&s.r, KP_EV_TIME) != NULL);
    e = find(&s.r, KP_EV_VIDEO);
    CHECK(e && !e->flag);
    e = find(&s.r, KP_EV_TITLE);
    CHECK(e && strstr(e->text, "ação"));

    /* A missing file ends with an error. */
    s.r.n = 0;
    char *missing = g_build_filename(dir, "nope.mp3", NULL);
    kp_backend_load(b, missing, 0);
    CHECK(wait_event(&s, KP_EV_ENDED, 10000));
    CHECK(find(&s.r, KP_EV_ENDED)->flag);
    g_free(missing);

    /* If mpv dies, we hear about it and can start it again. */
    s.r.n = 0;
    CHECK(kp_backend_pid(b) > 0);
    CHECK(kill(kp_backend_pid(b), SIGKILL) == 0);
    CHECK(wait_event(&s, KP_EV_CRASHED, 5000));
    CHECK(!kp_backend_running(b));
    CHECK(kp_backend_start(b, &err));
    kp_backend_load(b, wav, 1.0);
    CHECK(wait_event(&s, KP_EV_LOADED, 10000));
    CHECK(wait_event(&s, KP_EV_ENDED, 10000));

    kp_backend_free(b);
    g_main_loop_unref(s.loop);
    g_remove(wav);
    g_rmdir(dir);
    g_free(wav);
    g_free(dir);
}

int main(void)
{
    RUN(parses_events);
    RUN(numbers_ignore_the_locale);
    RUN(tags_make_artist_dash_title);
    RUN(cover_art_is_not_video);
    RUN(plays_with_real_mpv);
    return harness_report();
}
