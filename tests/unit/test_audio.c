/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../harness.h"
#include "backend.h"
#include "cmd.h"
#include "dec.h"
#include "tagreader.h"
#include "tags.h"

TEST(parses_commands)
{
    ka_cmd c;
    CHECK(ka_cmd_parse("load 12.5 /música/com espaço.mp3", &c));
    CHECK_INT_EQ(c.type, KA_CMD_LOAD);
    CHECK(fabs(c.num - 12.5) < 1e-9);
    CHECK_STR_EQ(c.path, "/música/com espaço.mp3");
    CHECK(ka_cmd_parse("queue /a.ogg", &c));
    CHECK_INT_EQ(c.type, KA_CMD_QUEUE);
    CHECK_STR_EQ(c.path, "/a.ogg");
    CHECK(ka_cmd_parse("queue", &c));
    CHECK_STR_EQ(c.path, "");
    CHECK(ka_cmd_parse("seekrel -5", &c));
    CHECK_INT_EQ(c.type, KA_CMD_SEEKREL);
    CHECK(c.num == -5);
    CHECK(ka_cmd_parse("pause 1", &c) && c.type == KA_CMD_PAUSE && c.num == 1);
    CHECK(ka_cmd_parse("stop", &c) && c.type == KA_CMD_STOP);
    CHECK(!ka_cmd_parse("stopx", &c));
    CHECK(!ka_cmd_parse("load abc /x", &c));
    CHECK(!ka_cmd_parse("load 1", &c));
    CHECK(!ka_cmd_parse("volume", &c));
    CHECK(!ka_cmd_parse("", &c));
}

TEST(splits_subtunes)
{
    int n;
    char *p = ka_dec_split_subtune("/m/jogo.nsf?3", &n);
    CHECK_STR_EQ(p, "/m/jogo.nsf");
    CHECK_INT_EQ(n, 3);
    free(p);
    p = ka_dec_split_subtune("file:///m/M%C3%BAsica/a%20b.spc?1", &n);
    CHECK_STR_EQ(p, "/m/Música/a b.spc");
    CHECK_INT_EQ(n, 1);
    free(p);
    p = ka_dec_split_subtune("/m/x.mp3", &n);
    CHECK_STR_EQ(p, "/m/x.mp3");
    CHECK_INT_EQ(n, 0);
    free(p);
    p = ka_dec_split_subtune("/m/what?.mp3", &n);
    CHECK_STR_EQ(p, "/m/what?.mp3");
    CHECK_INT_EQ(n, 0);
    free(p);
    /* A file really named "x?3" is that file. */
    char *dir = g_dir_make_tmp("kp-test-XXXXXX", NULL);
    char *odd = g_build_filename(dir, "x?3", NULL);
    g_file_set_contents(odd, "", 0, NULL);
    p = ka_dec_split_subtune(odd, &n);
    CHECK_STR_EQ(p, odd);
    CHECK_INT_EQ(n, 0);
    free(p);
    g_remove(odd);
    g_rmdir(dir);
    g_free(odd);
    g_free(dir);
}

/* ---- the engine through kp_backend ---------------------------------------- */

#define MAX_EVENTS 256

typedef struct {
    GMainLoop *loop;
    kp_event_type ev[MAX_EVENTS];
    double val[MAX_EVENTS];
    bool flag[MAX_EVENTS];
    int n;
    kp_event_type wait_for;
    bool got;
} session;

static void on_event(void *ud, const kp_event *ev)
{
    session *s = ud;
    if (s->n < MAX_EVENTS) {
        s->ev[s->n] = ev->type;
        s->val[s->n] = ev->value;
        s->flag[s->n] = ev->flag;
        s->n++;
    }
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

static int count(const session *s, kp_event_type type)
{
    int c = 0;
    for (int i = 0; i < s->n; i++)
        c += s->ev[i] == type;
    return c;
}

static double last(const session *s, kp_event_type type, bool *flag)
{
    for (int i = s->n - 1; i >= 0; i--)
        if (s->ev[i] == type) {
            if (flag)
                *flag = s->flag[i];
            return s->val[i];
        }
    return -1;
}

/* secs of a quiet tone, 16-bit mono 44.1 kHz (the engine resamples). */
static void write_wav(const char *path, double secs)
{
    const int rate = 44100, n = (int)(rate * secs);
    FILE *f = fopen(path, "wb");
    unsigned char h[44] = "RIFF____WAVEfmt ";
    unsigned data = (unsigned)n * 2, riff = 36 + data, fmt_len = 16, byte_rate = rate * 2;
    unsigned short pcm = 1, ch = 1, align = 2, bits = 16;
    memcpy(h + 4, &riff, 4);
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
        short v = (short)(2000 * sin(2 * M_PI * 440 * i / rate));
        fwrite(&v, 2, 1, f);
    }
    fclose(f);
}

/* A short WAV with RIFF INFO tags (title, artist, album). */
static void write_tagged_wav(const char *path)
{
    write_wav(path, 0.5);
    char *data;
    gsize len;
    g_file_get_contents(path, &data, &len, NULL);
    static const char info[] = "LIST\x2e\0\0\0INFO"
                               "INAM\x06\0\0\0Cancao"
                               "IART\x06\0\0\0Banda\0"
                               "IPRD\x06\0\0\0Disco\0";
    GByteArray *out = g_byte_array_new();
    g_byte_array_append(out, (const guint8 *)data, 36); /* RIFF + fmt */
    g_byte_array_append(out, (const guint8 *)info, sizeof info - 1);
    g_byte_array_append(out, (const guint8 *)data + 36, (guint)len - 36);
    unsigned riff = out->len - 8;
    memcpy(out->data + 4, &riff, 4);
    g_file_set_contents(path, (const char *)out->data, out->len, NULL);
    g_byte_array_free(out, TRUE);
    g_free(data);
}

TEST(reads_tags)
{
    char *dir = g_dir_make_tmp("kp-test-XXXXXX", NULL);
    char *a = g_build_filename(dir, "a.wav", NULL);
    write_tagged_wav(a);
    ka_tags t;
    char err[128];
    CHECK(ka_tags_read(a, &t, err, sizeof err));
    CHECK_STR_EQ(t.title, "Cancao");
    CHECK_STR_EQ(t.artist, "Banda");
    CHECK_STR_EQ(t.album, "Disco");
    CHECK(fabs(t.duration - 0.5) < 0.01);
    ka_tags_clear(&t);
    char *missing = g_build_filename(dir, "none.mp3", NULL);
    CHECK(!ka_tags_read(missing, &t, err, sizeof err));
    CHECK(t.title == NULL);
    g_remove(a);
    g_rmdir(dir);
    g_free(a);
    g_free(missing);
    g_free(dir);
}

typedef struct {
    GMainLoop *loop;
    int n, wanted;
    long index[8];
    char *title[8];
    int64_t length[8];
    bool failed[8];
} tag_answers;

static void on_tags(void *ud, int list_id, long index, const char *location, const kp_tags *t)
{
    (void)location;
    tag_answers *a = ud;
    if (list_id != 7 || a->n >= 8)
        return;
    a->index[a->n] = index;
    a->failed[a->n] = t == NULL;
    a->title[a->n] = t && t->title ? g_strdup(t->title) : NULL;
    a->length[a->n] = t ? t->length_ms : -1;
    if (++a->n == a->wanted)
        g_main_loop_quit(a->loop);
}

static gboolean give_up(gpointer data)
{
    g_main_loop_quit(data);
    return G_SOURCE_REMOVE;
}

static bool wait_answers(tag_answers *a, int wanted)
{
    a->wanted = wanted;
    guint t = g_timeout_add(10000, give_up, a->loop);
    if (a->n < wanted)
        g_main_loop_run(a->loop);
    g_source_remove(t);
    return a->n == wanted;
}

TEST(tag_reader_in_background)
{
    char *dir = g_dir_make_tmp("kp-test-XXXXXX", NULL);
    char *a = g_build_filename(dir, "a.wav", NULL);
    char *b = g_build_filename(dir, "b\nnova linha.wav", NULL);
    char *bad = g_build_filename(dir, "bad.mp3", NULL);
    char *missing = g_build_filename(dir, "none.ogg", NULL);
    write_tagged_wav(a);
    write_wav(b, 0.25);
    g_file_set_contents(bad, "not audio at all", -1, NULL);

    tag_answers ans = {.loop = g_main_loop_new(NULL, FALSE)};
    kp_tagreader *tr = kp_tagreader_new(KP_AUDIO_BIN, on_tags, &ans);
    kp_tagreader_add(tr, 7, 0, a);
    kp_tagreader_add(tr, 7, 1, b);
    kp_tagreader_add(tr, 7, 2, bad);
    kp_tagreader_add(tr, 7, 3, missing);
    kp_tagreader_add(tr, 9, 0, a); /* forgotten before it is sent? maybe not */
    kp_tagreader_forget(tr, 9);
    CHECK(wait_answers(&ans, 4));
    CHECK(ans.index[0] == 0 && ans.index[3] == 3);
    CHECK_STR_EQ(ans.title[0], "Cancao");
    CHECK_INT_EQ(ans.length[0], 500);
    /* No tags, but a length; a name with a newline goes as a URI. */
    CHECK(!ans.failed[1] && ans.title[1] == NULL);
    CHECK_INT_EQ(ans.length[1], 250);
    CHECK(ans.failed[2] && ans.failed[3]);

    /* A reader that dies costs the file it was on; the rest goes on. */
    int pid = kp_tagreader_pid(tr);
    CHECK(pid > 0);
    CHECK(kill(pid, SIGKILL) == 0);
    kp_tagreader_add(tr, 7, 4, a);
    CHECK(wait_answers(&ans, 5));
    /* Either answered before the kill landed, or failed and retried. */
    kp_tagreader_add(tr, 7, 5, a);
    CHECK(wait_answers(&ans, 6));
    CHECK_STR_EQ(ans.title[5], "Cancao");
    CHECK_INT_EQ(kp_tagreader_pending(tr), 0);

    kp_tagreader_free(tr);
    for (int i = 0; i < ans.n; i++)
        g_free(ans.title[i]);
    g_main_loop_unref(ans.loop);
    g_remove(a);
    g_remove(b);
    g_remove(bad);
    g_rmdir(dir);
    g_free(a);
    g_free(b);
    g_free(bad);
    g_free(missing);
    g_free(dir);
}

TEST(plays_through_the_engine)
{
    char *dir = g_dir_make_tmp("kp-test-XXXXXX", NULL);
    char *a = g_build_filename(dir, "a ação.wav", NULL);
    char *b = g_build_filename(dir, "b.wav", NULL);
    write_wav(a, 1.0);
    write_wav(b, 0.8);

    session s = {.loop = g_main_loop_new(NULL, FALSE)};
    kp_backend_opts o = {.program = KP_AUDIO_BIN, .device = "null", .volume = 80};
    kp_backend *be = kp_backend_new_audio(&o, on_event, &s);
    CHECK(kp_backend_caps(be) & KP_CAP_GAPLESS);
    char *err = NULL;
    CHECK(kp_backend_start(be, &err));
    kp_backend_set_ticking(be, true);

    /* A, then B without a gap: one NEXT, and ENDED only after B. */
    kp_backend_load(be, a, NULL);
    kp_backend_queue(be, b);
    CHECK(wait_event(&s, KP_EV_DURATION, 5000));
    CHECK(fabs(last(&s, KP_EV_DURATION, NULL) - 1.0) < 0.01);
    CHECK(count(&s, KP_EV_VIDEO) == 1 && !s.flag[0]);
    CHECK(wait_event(&s, KP_EV_NEXT, 5000));
    CHECK_INT_EQ(count(&s, KP_EV_ENDED), 0);
    CHECK(wait_event(&s, KP_EV_ENDED, 5000));
    bool error = true;
    last(&s, KP_EV_ENDED, &error);
    CHECK(!error);
    CHECK(fabs(last(&s, KP_EV_DURATION, NULL) - 0.8) < 0.01);
    CHECK(count(&s, KP_EV_TIME) >= 4);

    /* Seek and pause land where asked. */
    s.n = 0;
    kp_backend_load(be, a, &(kp_load_opts){.start = 0.5, .aid = -1, .sid = -1});
    CHECK(wait_event(&s, KP_EV_LOADED, 5000));
    CHECK(wait_event(&s, KP_EV_TIME, 2000));
    CHECK(last(&s, KP_EV_TIME, NULL) >= 0.5);
    kp_backend_set_pause(be, true);
    CHECK(wait_event(&s, KP_EV_PAUSED, 2000));
    CHECK(s.flag[s.n - 1]);
    kp_backend_stop(be);

    /* A file that isn't audio ends with an error. */
    s.n = 0;
    char *bad = g_build_filename(dir, "bad.mp3", NULL);
    g_file_set_contents(bad, "not audio at all", -1, NULL);
    kp_backend_load(be, bad, NULL);
    CHECK(wait_event(&s, KP_EV_ENDED, 5000));
    last(&s, KP_EV_ENDED, &error);
    CHECK(error);

    /* If the engine dies, we hear about it and can start it again. */
    s.n = 0;
    CHECK(kill(kp_backend_pid(be), SIGKILL) == 0);
    CHECK(wait_event(&s, KP_EV_CRASHED, 5000));
    CHECK(!kp_backend_running(be));
    CHECK(kp_backend_start(be, &err));
    kp_backend_load(be, b, NULL);
    CHECK(wait_event(&s, KP_EV_ENDED, 5000));

    kp_backend_free(be);
    g_main_loop_unref(s.loop);
    g_remove(a);
    g_remove(b);
    g_remove(bad);
    g_rmdir(dir);
    g_free(a);
    g_free(b);
    g_free(bad);
    g_free(dir);
}

int main(void)
{
    RUN(parses_commands);
    RUN(splits_subtunes);
    RUN(reads_tags);
    RUN(tag_reader_in_background);
    RUN(plays_through_the_engine);
    return harness_report();
}
