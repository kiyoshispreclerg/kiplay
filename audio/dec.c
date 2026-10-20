/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dec.h"
#include "avcompat.h"

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

#include "game.h"
#include "midi.h"
#include "out.h"

struct ka_dec {
    char *path;
    int subtune;
    int rate;
    ka_game *game; /* game music: none of the libav side below */
    ka_midi *midi; /* MIDI: neither */
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
};

/* Game music and MIDI have no rate of their own: Audacious's default. */
#define GAME_RATE 44100

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

/* "First - second", second alone, or nothing. */
static void set_tags(ka_dec *d, const char *first, const char *second)
{
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

/* Music: "Artist - Title". */
static void read_tags(ka_dec *d)
{
    const AVDictionary *dicts[] = {d->fmt->metadata, d->fmt->streams[d->stream]->metadata};
    const char *title = NULL, *artist = NULL;
    for (int i = 0; i < 2; i++) {
        if (!title)
            title = meta(dicts[i], "title");
        if (!artist)
            artist = meta(dicts[i], "artist");
    }
    set_tags(d, artist, title);
}

static void close_av(ka_dec *d)
{
    swr_free(&d->swr);
    avcodec_free_context(&d->codec);
    avformat_close_input(&d->fmt);
}

/* What the ALSA plug and the sound servers take without trouble. */
static int usable_rate(int rate)
{
    return rate >= 8000 && rate <= 192000 ? rate : 48000;
}

static int open_av(ka_dec *d)
{
    int r = avformat_open_input(&d->fmt, d->path, NULL, NULL);
    if (r < 0)
        return r;
    r = avformat_find_stream_info(d->fmt, NULL);
    if (r < 0)
        return r;
    ka_av_codec *codec = NULL;
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
    if (d->rate <= 0)
        d->rate = usable_rate(d->codec->sample_rate);
    d->swr = ka_av_swr_alloc(d->codec, d->rate);
    if (!d->swr)
        return AVERROR(ENOMEM);
    return swr_init(d->swr);
}

static ka_dec *open_game(ka_dec *d, char *err, int errsize)
{
    if (d->rate <= 0)
        d->rate = GAME_RATE;
    d->game = ka_game_open(d->path, d->subtune, d->rate, err, errsize);
    if (!d->game) {
        ka_dec_close(d);
        return NULL;
    }
    d->duration = ka_game_duration(d->game);
    /* "Game - Song", as Audacious. */
    const char *song = ka_game_song(d->game), *game = ka_game_name(d->game);
    set_tags(d, song ? game : NULL, song ? song : game);
    return d;
}

static ka_dec *open_midi(ka_dec *d, char *err, int errsize)
{
    if (d->rate <= 0)
        d->rate = GAME_RATE;
    d->midi = ka_midi_open(d->path, d->rate, err, errsize);
    if (!d->midi) {
        ka_dec_close(d);
        return NULL;
    }
    d->duration = ka_midi_duration(d->midi);
    return d;
}

ka_dec *ka_dec_open(const char *location, int rate, char *err, int errsize)
{
    ka_dec *d = calloc(1, sizeof *d);
    if (d)
        d->path = ka_dec_split_subtune(location, &d->subtune);
    if (!d || !d->path) {
        snprintf(err, (size_t)errsize, "out of memory");
        free(d);
        return NULL;
    }
    d->rate = rate;
    if (ka_game_is(d->path))
        return open_game(d, err, errsize);
    if (ka_midi_is(d->path))
        return open_midi(d, err, errsize);
    /* Not MIDI (a MIDI after this one would read the SoundFont again). */
    ka_midi_trim();
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
    ka_game_close(d->game);
    ka_midi_close(d->midi);
    free(d->tags);
    free(d->path);
    free(d);
}

int ka_dec_rate(const ka_dec *d)
{
    return d->rate;
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
                        d->skip = (long)((d->seek_target - at) * d->rate);
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

long ka_dec_read(ka_dec *d, int16_t *out, long max)
{
    if (d->game)
        return ka_game_read(d->game, out, max);
    if (d->midi)
        return ka_midi_read(d->midi, out, max);
    while (av_audio_fifo_size(d->fifo) < max && !d->decode_eof)
        if (!feed(d))
            break;
    int n = av_audio_fifo_size(d->fifo);
    if (n > max)
        n = (int)max;
    if (n == 0)
        return 0;
    void *planes[1] = {out};
    return av_audio_fifo_read(d->fifo, planes, n);
}

bool ka_dec_seek(ka_dec *d, double seconds)
{
    if (seconds < 0)
        seconds = 0;
    if (d->game)
        return ka_game_seek(d->game, seconds);
    if (d->midi)
        return ka_midi_seek(d->midi, seconds);
    /* In the audio stream's own time base: some demuxers read the
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
    return true;
}
