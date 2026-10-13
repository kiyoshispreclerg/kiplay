/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tags.h"

#include <libavformat/avformat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dec.h"

static const char *meta(const AVDictionary *d, const char *key)
{
    const AVDictionaryEntry *e = av_dict_get(d, key, NULL, 0); /* any case */
    return e && e->value[0] ? e->value : NULL;
}

/* The first of the dictionaries that has the key. */
static char *pick(const AVDictionary *const dicts[], int n, const char *key)
{
    for (int i = 0; i < n; i++) {
        const char *v = dicts[i] ? meta(dicts[i], key) : NULL;
        if (v)
            return strdup(v);
    }
    return NULL;
}

bool ka_tags_read(const char *location, ka_tags *t, char *err, int errsize)
{
    memset(t, 0, sizeof *t);
    int subtune;
    char *path = ka_dec_split_subtune(location, &subtune);
    if (!path) {
        snprintf(err, (size_t)errsize, "out of memory");
        return false;
    }
    AVDictionary *opts = NULL;
    if (subtune > 0) {
        char n[16];
        snprintf(n, sizeof n, "%d", subtune - 1); /* libgme counts from 0 */
        av_dict_set(&opts, "track_index", n, 0);
    }
    AVFormatContext *fmt = NULL;
    int r = avformat_open_input(&fmt, path, NULL, &opts);
    av_dict_free(&opts);
    free(path);
    /* Most containers say how long they are when opened. The others (MP3
     * without a header, raw AAC) need a look at the first packets, which
     * opens decoders: one thread each (a video decoder with one per core
     * costs megabytes), and only then. */
    if (r >= 0 && fmt->duration <= 0) {
        AVDictionary **per = calloc(fmt->nb_streams ? fmt->nb_streams : 1, sizeof *per);
        for (unsigned i = 0; per && i < fmt->nb_streams; i++)
            av_dict_set(&per[i], "threads", "1", 0);
        r = avformat_find_stream_info(fmt, per);
        for (unsigned i = 0; per && i < fmt->nb_streams; i++)
            av_dict_free(&per[i]);
        free(per);
    }
    if (r < 0) {
        av_strerror(r, err, (size_t)errsize);
        avformat_close_input(&fmt);
        return false;
    }
    int st = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (st < 0)
        st = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    const AVDictionary *dicts[] = {fmt->metadata, st >= 0 ? fmt->streams[st]->metadata : NULL};
    t->title = pick(dicts, 2, "title");
    t->artist = pick(dicts, 2, "artist");
    t->album = pick(dicts, 2, "album");
    if (!t->title) {
        /* libgme's names: the song, its game and who wrote it. */
        t->title = pick(dicts, 2, "song");
        char *game = pick(dicts, 2, "game");
        if (!t->title) {
            t->title = game;
            game = NULL;
        }
        if (game && !t->album)
            t->album = game;
        else
            free(game);
        if (!t->artist)
            t->artist = pick(dicts, 2, "author");
    }
    if (fmt->duration > 0)
        t->duration = (double)fmt->duration / AV_TIME_BASE;
    else if (st >= 0 && fmt->streams[st]->duration > 0)
        t->duration = (double)fmt->streams[st]->duration * av_q2d(fmt->streams[st]->time_base);
    if (t->duration <= 0 && !strcmp(fmt->iformat->name, "libgme"))
        t->duration = KA_GAME_LENGTH_S;
    avformat_close_input(&fmt);
    return true;
}

void ka_tags_clear(ka_tags *t)
{
    free(t->title);
    free(t->artist);
    free(t->album);
    memset(t, 0, sizeof *t);
}

/* A field of the reply: no tab or newline in it. */
static void put_field(const char *s)
{
    for (; s && *s; s++)
        putchar(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s);
}

int ka_tags_serve(void)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, stdin)) > 0) {
        if (line[n - 1] == '\n')
            line[--n] = '\0';
        ka_tags t;
        char err[256];
        if (ka_tags_read(line, &t, err, sizeof err)) {
            printf("ok %.3f\t", t.duration > 0 ? t.duration : -1.0);
            put_field(t.title);
            putchar('\t');
            put_field(t.artist);
            putchar('\t');
            put_field(t.album);
            putchar('\n');
            ka_tags_clear(&t);
        } else {
            printf("error ");
            put_field(err);
            putchar('\n');
        }
        fflush(stdout);
    }
    free(line);
    return 0;
}
