/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KA_AVCOMPAT_H
#define KA_AVCOMPAT_H

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <stdbool.h>

/* The FFmpeg API this builds against ranges from 4.4 (Ubuntu 22.04) to 7.x
 * (Debian 13): the few calls that changed are wrapped here, so the callers
 * read the same everywhere. */

/* av_find_best_stream's decoder: const from FFmpeg 5.0 (libavformat 59). */
#if LIBAVFORMAT_VERSION_MAJOR >= 59
typedef const AVCodec ka_av_codec;
#else
typedef AVCodec ka_av_codec;
#endif

/* av_guess_format's result and avformat_alloc_output_context2's argument:
 * const from 5.0 too. */
#if LIBAVFORMAT_VERSION_MAJOR >= 59
typedef const AVOutputFormat ka_av_oformat;
#else
typedef AVOutputFormat ka_av_oformat;
#endif

/* AVChannelLayout replaced the channel_layout/channels fields in 5.1
 * (libavcodec 59.24) and the old ones are gone in 7.0. */
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100)
#define KA_AV_CHLAYOUT 1
#endif

static inline int ka_av_channels(const AVCodecContext *c)
{
#ifdef KA_AV_CHLAYOUT
    return c->ch_layout.nb_channels;
#else
    return c->channels;
#endif
}

/* The encoder gets the decoder's channels (split.c); <0 on failure. */
static inline int ka_av_copy_layout(AVCodecContext *enc, const AVCodecContext *dec)
{
#ifdef KA_AV_CHLAYOUT
    if (dec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        av_channel_layout_default(&enc->ch_layout, dec->ch_layout.nb_channels);
        return 0;
    }
    return av_channel_layout_copy(&enc->ch_layout, &dec->ch_layout);
#else
    enc->channels = dec->channels;
    enc->channel_layout = dec->channel_layout ? dec->channel_layout
                                              : (uint64_t)av_get_default_channel_layout(dec->channels);
    return 0;
#endif
}

/* A frame for the encoder: its channels. <0 on failure. */
static inline int ka_av_frame_layout(AVFrame *f, const AVCodecContext *enc)
{
#ifdef KA_AV_CHLAYOUT
    return av_channel_layout_copy(&f->ch_layout, &enc->ch_layout);
#else
    f->channels = enc->channels;
    f->channel_layout = enc->channel_layout;
    return 0;
#endif
}

/* Whether the encoder takes samples in fmt. avcodec_get_supported_config
 * replaced AVCodec.sample_fmts in 7.1 (libavcodec 61.13). */
static inline bool ka_av_encoder_takes(const AVCodec *codec, enum AVSampleFormat fmt)
{
    const enum AVSampleFormat *list = NULL;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void *cfg = NULL;
    if (avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &cfg, NULL) < 0)
        return false;
    list = cfg;
#else
    list = codec->sample_fmts;
#endif
    if (!list)
        return true; /* "any" */
    for (; *list != AV_SAMPLE_FMT_NONE; list++)
        if (*list == fmt)
            return true;
    return false;
}

/* A resampler from what the codec decodes to stereo S16 at out_rate (and
 * the codec's own rate); NULL when out of memory. Not initialised yet. */
static inline SwrContext *ka_av_swr_alloc(const AVCodecContext *c, int out_rate)
{
#ifdef KA_AV_CHLAYOUT
    AVChannelLayout in = {0}, out = {0};
    if (av_channel_layout_copy(&in, &c->ch_layout) < 0)
        return NULL;
    if (in.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&in, in.nb_channels);
    av_channel_layout_default(&out, 2);
    SwrContext *swr = NULL;
    if (swr_alloc_set_opts2(&swr, &out, AV_SAMPLE_FMT_S16, out_rate, &in, c->sample_fmt,
                            c->sample_rate, 0, NULL) < 0)
        swr = NULL;
    av_channel_layout_uninit(&in);
    av_channel_layout_uninit(&out);
    return swr;
#else
    int64_t in = c->channel_layout ? (int64_t)c->channel_layout
                                   : av_get_default_channel_layout(c->channels);
    return swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, out_rate, in,
                              c->sample_fmt, c->sample_rate, 0, NULL);
#endif
}

#endif
