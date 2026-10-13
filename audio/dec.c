/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dec.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "out.h"

struct ka_dec {
    char *path;
    int subtune;
    /* libgme: no real seeking in ffmpeg 4 (it reads to the end looking for
     * the time). Emulation runs ~1000x real time, so seeking decodes up to
     * the target instead (from the start again when going back). */
    bool emulated;
    AVFormatContext *fmt;
    AVCodecContext *codec;
    SwrContext *swr;
    AVPacket *pkt;
    AVFrame *frame;
    /* Converted samples waiting to be read: each decoded frame goes
     * through the resampler whole, and the resampler is flushed only once,
     * at the end (flushing mid-song would click). */
    AVAudioFifo *fifo;
    int16_t *tmp;
    int tmp_cap;
    bool flushed;
    int stream;
    double duration;
    char *tags;
    bool demux_eof, decode_eof;
    /* After a seek: the target, until the first frame says where we are;
     * then how many output frames to throw away to land on it. */
    double seek_target;
    bool seeking;
    long skip;
    /* Game music without a length loops forever: like Audacious, it ends
     * after a default length, fading out over the last seconds. Frames
     * handed out so far, to know where we are. */
    bool synthetic_end;
    long long played;
};

#define GAME_FADE_S 8

/* "%C3%BA" -> bytes, for file:// URIs. */
static char *uri_to_path(const char *uri)
{
    const char *p = uri + strlen("file://");
    char *out = malloc(strlen(p) + 1), *o = out;
    if (!out)
        return NULL;
    for (; *p; p++) {
        if (p[0] == '%' && p[1] && p[2]) {
            char hex[3] = {p[1], p[2], 0};
            char *end;
            long v = strtol(hex, &end, 16);
            if (!*end) {
                *o++ = (char)v;
                p += 2;
                continue;
            }
        }
        *o++ = *p;
    }
    *o = '\0';
    return out;
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

char *ka_dec_split_subtune(const char *location, int *subtune)
{
    *subtune = 0;
    char *path = strncmp(location, "file://", 7) == 0 ? uri_to_path(location) : strdup(location);
    if (!path)
        return NULL;
    char *q = strrchr(path, '?');
    if (!q || !q[1] || q[1 + strspn(q + 1, "0123456789")] != '\0')
        return path;
    /* A file really named "x?3" wins over song 3 of "x". */
    if (file_exists(path))
        return path;
    *subtune = atoi(q + 1);
    *q = '\0';
    return path;
}

static const char *meta(const AVDictionary *d, const char *key)
{
    const AVDictionaryEntry *e = av_dict_get(d, key, NULL, 0); /* any case */
    return e && e->value[0] ? e->value : NULL;
}

static void read_tags(ka_dec *d)
{
    const AVDictionary *dicts[] = {d->fmt->metadata, d->fmt->streams[d->stream]->metadata};
    const char *title = NULL, *artist = NULL, *song = NULL, *game = NULL;
    for (int i = 0; i < 2; i++) {
        if (!title)
            title = meta(dicts[i], "title");
        if (!artist)
            artist = meta(dicts[i], "artist");
        /* libgme's names */
        if (!song)
            song = meta(dicts[i], "song");
        if (!game)
            game = meta(dicts[i], "game");
    }
    /* Music: "Artist - Title". Game music: "Game - Song", as Audacious. */
    const char *first = artist, *second = title;
    if (!title) {
        first = song ? game : NULL;
        second = song ? song : game;
    }
    if (!second)
        return;
    size_t n = strlen(second) + (first ? strlen(first) + 3 : 0) + 1;
    d->tags = malloc(n);
    if (!d->tags)
        return;
    if (first)
        snprintf(d->tags, n, "%s - %s", first, second);
    else
        snprintf(d->tags, n, "%s", second);
    /* One line on the protocol. */
    for (char *c = d->tags; *c; c++)
        if (*c == '\n' || *c == '\r')
            *c = ' ';
}

/* The libav side, which a game-music seek closes and opens again. */
static void close_av(ka_dec *d)
{
    swr_free(&d->swr);
    avcodec_free_context(&d->codec);
    avformat_close_input(&d->fmt);
}

static int open_av(ka_dec *d)
{
    AVDictionary *opts = NULL;
    if (d->subtune > 0) {
        char n[16];
        snprintf(n, sizeof n, "%d", d->subtune - 1); /* libgme counts from 0 */
        av_dict_set(&opts, "track_index", n, 0);
    }
    int r = avformat_open_input(&d->fmt, d->path, NULL, &opts);
    av_dict_free(&opts);
    if (r < 0)
        return r;
    r = avformat_find_stream_info(d->fmt, NULL);
    if (r < 0)
        return r;
    AVCodec *codec = NULL; /* ffmpeg 4: not const yet */
    d->stream = av_find_best_stream(d->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (d->stream < 0)
        return d->stream;
    for (unsigned i = 0; i < d->fmt->nb_streams; i++)
        if ((int)i != d->stream)
            d->fmt->streams[i]->discard = AVDISCARD_ALL;
    d->codec = avcodec_alloc_context3(codec);
    if (!d->codec)
        return AVERROR(ENOMEM);
    avcodec_parameters_to_context(d->codec, d->fmt->streams[d->stream]->codecpar);
    d->codec->thread_count = 1; /* music doesn't need more, and threads cost memory */
    r = avcodec_open2(d->codec, codec, NULL);
    if (r < 0)
        return r;
    int64_t in_layout = d->codec->channel_layout ? (int64_t)d->codec->channel_layout
                                                 : av_get_default_channel_layout(d->codec->channels);
    d->swr = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, KA_RATE, in_layout,
                                d->codec->sample_fmt, d->codec->sample_rate, 0, NULL);
    if (!d->swr)
        return AVERROR(ENOMEM);
    d->emulated = !strcmp(d->fmt->iformat->name, "libgme");
    d->demux_eof = d->decode_eof = d->flushed = d->seeking = false;
    d->skip = 0;
    d->played = 0;
    return swr_init(d->swr);
}

ka_dec *ka_dec_open(const char *location, char *err, int errsize)
{
    ka_dec *d = calloc(1, sizeof *d);
    if (d)
        d->path = ka_dec_split_subtune(location, &d->subtune);
    if (!d || !d->path) {
        snprintf(err, (size_t)errsize, "out of memory");
        free(d);
        return NULL;
    }
    int r = open_av(d);
    if (r < 0)
        goto fail;
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    d->fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_S16, KA_CHANNELS, 8192);
    if (!d->pkt || !d->frame || !d->fifo) {
        r = AVERROR(ENOMEM);
        goto fail;
    }
    AVStream *st = d->fmt->streams[d->stream];
    if (d->fmt->duration > 0)
        d->duration = (double)d->fmt->duration / AV_TIME_BASE;
    else if (st->duration > 0)
        d->duration = (double)st->duration * av_q2d(st->time_base);
    if (d->duration <= 0 && d->emulated) {
        d->duration = KA_GAME_LENGTH_S;
        d->synthetic_end = true;
    }
    read_tags(d);
    return d;

fail:
    av_strerror(r, err, (size_t)errsize);
    ka_dec_close(d);
    return NULL;
}

void ka_dec_close(ka_dec *d)
{
    if (!d)
        return;
    if (d->fifo)
        av_audio_fifo_free(d->fifo);
    free(d->tmp);
    av_frame_free(&d->frame);
    av_packet_free(&d->pkt);
    close_av(d);
    free(d->tags);
    free(d->path);
    free(d);
}

double ka_dec_duration(const ka_dec *d)
{
    return d->duration;
}

const char *ka_dec_tags(const ka_dec *d)
{
    return d->tags;
}

/* Converts (in = NULL: flushes) into the fifo, dropping what a seek
 * landed before the target. */
static bool convert(ka_dec *d, const uint8_t **in, int in_count)
{
    int want = swr_get_out_samples(d->swr, in_count) + 32;
    if (want > d->tmp_cap) {
        int16_t *t = realloc(d->tmp, (size_t)want * KA_CHANNELS * sizeof *t);
        if (!t)
            return false;
        d->tmp = t;
        d->tmp_cap = want;
    }
    uint8_t *o[1] = {(uint8_t *)d->tmp};
    int n = swr_convert(d->swr, o, d->tmp_cap, in, in_count);
    if (n <= 0)
        return n == 0;
    int16_t *start = d->tmp;
    if (d->skip > 0) {
        long drop = n < d->skip ? n : d->skip;
        d->skip -= drop;
        start += drop * KA_CHANNELS;
        n -= (int)drop;
    }
    void *planes[1] = {start};
    return n == 0 || av_audio_fifo_write(d->fifo, planes, n) == n;
}

/* Decodes one more frame into the fifo. false at the end. */
static bool feed(ka_dec *d)
{
    for (;;) {
        int r = avcodec_receive_frame(d->codec, d->frame);
        if (r == 0) {
            if (d->seeking) {
                /* Where did we land? Drop output up to the target. */
                int64_t pts = d->frame->best_effort_timestamp;
                if (pts != AV_NOPTS_VALUE) {
                    double at = (double)pts * av_q2d(d->fmt->streams[d->stream]->time_base);
                    if (d->fmt->start_time != AV_NOPTS_VALUE)
                        at -= (double)d->fmt->start_time / AV_TIME_BASE;
                    if (d->seek_target > at)
                        d->skip = (long)((d->seek_target - at) * KA_RATE);
                }
                d->seeking = false;
            }
            bool ok = convert(d, (const uint8_t **)d->frame->extended_data, d->frame->nb_samples);
            av_frame_unref(d->frame);
            return ok;
        }
        if (r != AVERROR(EAGAIN) || d->demux_eof) {
            /* AVERROR_EOF after draining, or a decoder error: the end. */
            d->decode_eof = true;
            if (!d->flushed) {
                d->flushed = true;
                convert(d, NULL, 0);
            }
            return false;
        }
        r = av_read_frame(d->fmt, d->pkt);
        if (r < 0) {
            d->demux_eof = true;
            avcodec_send_packet(d->codec, NULL); /* drain: frames, then EOF */
            continue;
        }
        if (d->pkt->stream_index == d->stream)
            /* A broken packet is skipped, not the end of the song. */
            avcodec_send_packet(d->codec, d->pkt);
        av_packet_unref(d->pkt);
    }
}

/* The default ending of game music: a linear fade, then the end. */
static long fade_out(ka_dec *d, int16_t *out, long n)
{
    long long end = (long long)KA_GAME_LENGTH_S * KA_RATE, fade = (long long)GAME_FADE_S * KA_RATE;
    if (d->played >= end)
        return 0;
    if (d->played + n > end)
        n = (long)(end - d->played);
    for (long i = 0; i < n; i++) {
        long long left = end - (d->played + i);
        if (left < fade) {
            int q = (int)(left * 32768 / fade);
            out[2 * i] = (int16_t)((out[2 * i] * q) >> 15);
            out[2 * i + 1] = (int16_t)((out[2 * i + 1] * q) >> 15);
        }
    }
    return n;
}

long ka_dec_read(ka_dec *d, int16_t *out, long max)
{
    while (av_audio_fifo_size(d->fifo) < max && !d->decode_eof)
        if (!feed(d))
            break;
    int n = av_audio_fifo_size(d->fifo);
    if (n > max)
        n = (int)max;
    if (n == 0)
        return 0;
    void *planes[1] = {out};
    long got = av_audio_fifo_read(d->fifo, planes, n);
    if (got > 0 && d->synthetic_end)
        got = fade_out(d, out, got);
    if (got > 0)
        d->played += got;
    return got;
}

/* Game music: decode (and throw away) up to the target. */
static bool seek_by_decoding(ka_dec *d, double seconds)
{
    long long target = (long long)(seconds * KA_RATE);
    if (target < d->played) {
        close_av(d);
        av_audio_fifo_reset(d->fifo);
        if (open_av(d) < 0)
            return false;
    }
    static int16_t scratch[4096 * KA_CHANNELS];
    while (d->played < target) {
        long want = target - d->played < 4096 ? (long)(target - d->played) : 4096;
        if (ka_dec_read(d, scratch, want) <= 0)
            break;
    }
    return true;
}

bool ka_dec_seek(ka_dec *d, double seconds)
{
    if (seconds < 0)
        seconds = 0;
    if (d->emulated)
        return seek_by_decoding(d, seconds);
    /* In the audio stream's own time base: some demuxers (libgme) read the
     * timestamp in it whatever stream index is given. */
    AVStream *st = d->fmt->streams[d->stream];
    double start = d->fmt->start_time != AV_NOPTS_VALUE ? (double)d->fmt->start_time / AV_TIME_BASE : 0;
    int64_t ts = (int64_t)((seconds + start) / av_q2d(st->time_base));
    if (av_seek_frame(d->fmt, d->stream, ts, AVSEEK_FLAG_BACKWARD) < 0)
        return false;
    avcodec_flush_buffers(d->codec);
    /* Drop what the resampler and the fifo hold from before the jump. */
    swr_close(d->swr);
    swr_init(d->swr);
    av_audio_fifo_reset(d->fifo);
    d->demux_eof = d->decode_eof = d->flushed = false;
    d->seek_target = seconds;
    d->seeking = true;
    d->skip = 0;
    d->played = (long long)(seconds * KA_RATE);
    return true;
}
