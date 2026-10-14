/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Phase 0 prototype of kiplay-audio: decode one file and play it through
 * ALSA, to measure what a small audio process costs.
 *
 *   audio [-d ALSA_DEVICE] [-m] FILE
 *     -m  decode MP3 with dr_mp3 instead of libavcodec
 *     -d null  ALSA's null sink, paced by the clock (no sound) */
#define _POSIX_C_SOURCE 200809L
#include <alsa/asoundlib.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include "../audio/avcompat.h"
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define DR_MP3_IMPLEMENTATION

#include "../vendor/dr_libs/dr_mp3.h"

#define RATE 48000
#define CHANNELS 2

static snd_pcm_t *pcm;
static bool pace; /* the null device takes everything at once */
static struct timespec next;

static void out_open(const char *dev)
{
    int r = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_PLAYBACK, 0);
    if (r < 0) {
        fprintf(stderr, "alsa %s: %s\n", dev, snd_strerror(r));
        exit(1);
    }
    r = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           CHANNELS, RATE, 1, 200000);
    if (r < 0) {
        fprintf(stderr, "alsa params: %s\n", snd_strerror(r));
        exit(1);
    }
    pace = !strcmp(dev, "null");
    clock_gettime(CLOCK_MONOTONIC, &next);
}

static void out_write(const int16_t *buf, long frames)
{
    if (pace) {
        long ns = frames * 1000000000L / RATE;
        next.tv_nsec += ns;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    while (frames > 0) {
        snd_pcm_sframes_t n = snd_pcm_writei(pcm, buf, (snd_pcm_uframes_t)frames);
        if (n < 0) {
            n = snd_pcm_recover(pcm, (int)n, 1);
            if (n < 0)
                return;
            continue;
        }
        buf += n * CHANNELS;
        frames -= n;
    }
}

static int play_drmp3(const char *path)
{
    drmp3 mp3;
    if (!drmp3_init_file(&mp3, path, NULL))
        return 1;
    if (mp3.sampleRate != RATE || mp3.channels != CHANNELS)
        fprintf(stderr, "aviso: %u Hz %u canais tocados como 48 kHz estéreo\n",
                mp3.sampleRate, mp3.channels);
    static int16_t buf[4096 * CHANNELS];
    drmp3_uint64 n;
    while ((n = drmp3_read_pcm_frames_s16(&mp3, 4096, buf)) > 0)
        out_write(buf, (long)n);
    drmp3_uninit(&mp3);
    return 0;
}

static int play_av(const char *path)
{
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0 ||
        avformat_find_stream_info(fmt, NULL) < 0) {
        fprintf(stderr, "não abriu %s\n", path);
        return 1;
    }
    ka_av_codec *codec = NULL;
    int si = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (si < 0) {
        fprintf(stderr, "sem áudio\n");
        return 1;
    }
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if ((int)i != si)
            fmt->streams[i]->discard = AVDISCARD_ALL;
    AVCodecContext *dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, fmt->streams[si]->codecpar);
    if (avcodec_open2(dec, codec, NULL) < 0)
        return 1;
    fprintf(stderr, "%s, %d Hz, %d canais\n", codec->name, dec->sample_rate, ka_av_channels(dec));

    SwrContext *swr = ka_av_swr_alloc(dec, RATE);
    swr_init(swr);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    static int16_t buf[16384 * CHANNELS];
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == si && avcodec_send_packet(dec, pkt) >= 0) {
            while (avcodec_receive_frame(dec, frame) >= 0) {
                uint8_t *outp[1] = {(uint8_t *)buf};
                int n = swr_convert(swr, outp, 16384, (const uint8_t **)frame->extended_data,
                                    frame->nb_samples);
                if (n > 0)
                    out_write(buf, n);
            }
        }
        av_packet_unref(pkt);
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    swr_free(&swr);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return 0;
}

int main(int argc, char **argv)
{
    const char *dev = "default", *path = NULL;
    bool mp3 = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc)
            dev = argv[++i];
        else if (!strcmp(argv[i], "-m"))
            mp3 = true;
        else
            path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "uso: audio [-d DISPOSITIVO] [-m] ARQUIVO\n");
        return 2;
    }
    out_open(dev);
    int r = mp3 ? play_drmp3(path) : play_av(path);
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    return r;
}
