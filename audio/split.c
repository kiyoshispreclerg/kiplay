/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "split.h"
#include "avcompat.h"

#include <errno.h>
#include <libavutil/audio_fifo.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "game.h"
#include "midi.h"

/* More channels than this and the file isn't music. */
#define MAX_CHANNELS 64
/* Samples per frame for encoders that take any number (PCM). */
#define VAR_FRAME 4096

typedef struct {
    double start, end; /* seconds; end < 0: to the end */
    char *title, *artist, *album, *dest, *part;
    bool written; /* the .part is complete */
} piece;

typedef struct {
    AVFormatContext *in;
    int ast;           /* the audio stream */
    int pics[8], npics; /* covers (attached pictures) */
    bool exact;        /* decode and encode again (lossless formats) */
    AVCodecContext *dec;
    const AVCodec *encoder;
    ka_av_oformat *ofmt;

    piece *pieces;
    int n;
    int cur;           /* the piece being worked on */
    AVFormatContext *out; /* its file, NULL until its first packet */
    AVCodecContext *enc;
    AVAudioFifo *fifo;
    int64_t offset;    /* copy: the piece's first timestamp (input time base) */
    int64_t next_pts;  /* exact: samples given to the encoder */
    int64_t pos;       /* exact: samples decoded so far */
    int64_t guess;     /* copy: running time for packets without one */
    double duration;
    double progress;
    char err[256];
} job;

static volatile sig_atomic_t stopping;

static void on_term(int sig)
{
    (void)sig;
    stopping = 1;
}

static void fail(job *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(job *j, const char *fmt, ...)
{
    if (j->err[0])
        return; /* the first one says what went wrong */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(j->err, sizeof j->err, fmt, ap);
    va_end(ap);
}

static void fail_av(job *j, const char *what, int r)
{
    char e[128];
    av_strerror(r, e, sizeof e);
    fail(j, "%s: %s", what, e);
}

/* ---- the input ------------------------------------------------------------ */

static bool open_decoder(job *j)
{
    const AVCodecParameters *par = j->in->streams[j->ast]->codecpar;
    const AVCodecDescriptor *desc = avcodec_descriptor_get(par->codec_id);
    if (!desc || !(desc->props & AV_CODEC_PROP_LOSSLESS))
        return false;
    const AVCodec *decoder = avcodec_find_decoder(par->codec_id);
    j->encoder = avcodec_find_encoder(par->codec_id);
    if (!decoder || !j->encoder)
        return false;
    j->dec = avcodec_alloc_context3(decoder);
    if (!j->dec || avcodec_parameters_to_context(j->dec, par) < 0)
        return false;
    j->dec->thread_count = 1;
    if (avcodec_open2(j->dec, decoder, NULL) < 0 || ka_av_channels(j->dec) > MAX_CHANNELS ||
        !ka_av_encoder_takes(j->encoder, j->dec->sample_fmt)) {
        avcodec_free_context(&j->dec);
        return false;
    }
    return true;
}

static bool open_input(job *j, const char *path)
{
    /* libav may open some of these too (libgme), but there is no
     * recording to cut: the engine plays them. */
    if (ka_game_is(path) || ka_midi_is(path)) {
        fail(j, "not a recording");
        return false;
    }
    int r = avformat_open_input(&j->in, path, NULL, NULL);
    if (r < 0) {
        fail_av(j, path, r);
        return false;
    }
    AVDictionary **per = calloc(j->in->nb_streams ? j->in->nb_streams : 1, sizeof *per);
    for (unsigned i = 0; per && i < j->in->nb_streams; i++)
        av_dict_set(&per[i], "threads", "1", 0);
    r = avformat_find_stream_info(j->in, per);
    for (unsigned i = 0; per && i < j->in->nb_streams; i++)
        av_dict_free(&per[i]);
    free(per);
    if (r < 0) {
        fail_av(j, path, r);
        return false;
    }
    j->ast = av_find_best_stream(j->in, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (j->ast < 0) {
        fail(j, "no sound in the file");
        return false;
    }
    for (unsigned i = 0; i < j->in->nb_streams; i++) {
        const AVStream *st = j->in->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
            continue;
        if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            fail(j, "the file has video");
            return false;
        }
        if (j->npics < (int)(sizeof j->pics / sizeof j->pics[0]))
            j->pics[j->npics++] = (int)i;
    }
    const AVStream *as = j->in->streams[j->ast];
    if (j->in->duration > 0)
        j->duration = (double)j->in->duration / AV_TIME_BASE;
    else if (as->duration > 0)
        j->duration = (double)as->duration * av_q2d(as->time_base);
    j->exact = open_decoder(j);
    return true;
}

static void close_input(job *j)
{
    avcodec_free_context(&j->dec);
    avformat_close_input(&j->in);
}

int ka_split_info(const char *path)
{
    job j = {0};
    if (!open_input(&j, path)) {
        printf("error %s\n", j.err);
        close_input(&j);
        return 1;
    }
    printf("duration %.3f\n", j.duration > 0 ? j.duration : -1.0);
    printf("mode %s\n", j.exact ? "exact" : "copy");
    for (unsigned i = 0; i < j.in->nb_chapters; i++) {
        const AVChapter *c = j.in->chapters[i];
        const AVDictionaryEntry *t = av_dict_get(c->metadata, "title", NULL, 0);
        printf("chapter %.3f\t", (double)c->start * av_q2d(c->time_base));
        for (const char *s = t ? t->value : ""; *s; s++)
            putchar(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s);
        putchar('\n');
    }
    close_input(&j);
    return 0;
}

/* ---- one piece's file ------------------------------------------------------ */

/* Tags of the whole file that would lie on a piece of it. */
static bool whole_file_tag(const char *key)
{
    static const char *const keys[] = {
        "encoder", "major_brand", "minor_version", "compatible_brands", "handler_name",
        "vendor_id", "duration", "creation_time", "cuesheet", "itunsmpb", "itunnorm",
        "replaygain_track_gain", "replaygain_track_peak", "r128_track_gain", "track",
        "tracktotal", "totaltracks", "tracknumber", "unsyncedlyrics"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        if (!strcasecmp(key, keys[i]))
            return true;
    return !strncasecmp(key, "lyrics", 6);
}

static AVDictionary *piece_tags(const job *j, int k)
{
    AVDictionary *all = NULL, *d = NULL;
    av_dict_copy(&all, j->in->metadata, 0);
    av_dict_copy(&all, j->in->streams[j->ast]->metadata, AV_DICT_DONT_OVERWRITE);
    const AVDictionaryEntry *e = NULL;
    while ((e = av_dict_get(all, "", e, AV_DICT_IGNORE_SUFFIX)))
        if (!whole_file_tag(e->key))
            av_dict_set(&d, e->key, e->value, 0);
    av_dict_free(&all);
    const piece *p = &j->pieces[k];
    if (p->title[0])
        av_dict_set(&d, "title", p->title, 0);
    if (p->artist[0])
        av_dict_set(&d, "artist", p->artist, 0);
    if (p->album[0])
        av_dict_set(&d, "album", p->album, 0);
    char track[32];
    snprintf(track, sizeof track, "%d/%d", k + 1, j->n);
    av_dict_set(&d, "track", track, 0);
    return d;
}

/* Muxers that keep a cover as an attached picture (the others refuse
 * the extra stream). */
static bool keeps_covers(const ka_av_oformat *f)
{
    static const char *const names[] = {"mp3", "flac", "ipod", "mp4", "mov"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(f->name, names[i]))
            return true;
    return false;
}

/* Vorbis comments are written from the stream's tags, not the file's. */
static bool tags_on_stream(const ka_av_oformat *f)
{
    return !strcmp(f->name, "ogg") || !strcmp(f->name, "oga") || !strcmp(f->name, "opus") ||
           !strcmp(f->name, "spx");
}

static void close_out(job *j)
{
    if (j->out) {
        if (j->out->pb)
            avio_closep(&j->out->pb);
        avformat_free_context(j->out);
        j->out = NULL;
    }
    avcodec_free_context(&j->enc);
    if (j->fifo) {
        av_audio_fifo_free(j->fifo);
        j->fifo = NULL;
    }
}

static bool add_encoder(job *j, AVStream *ost)
{
    AVCodecContext *enc = j->enc = avcodec_alloc_context3(j->encoder);
    if (!enc || ka_av_copy_layout(enc, j->dec) < 0) {
        fail(j, "out of memory");
        return false;
    }
    enc->sample_rate = j->dec->sample_rate;
    enc->sample_fmt = j->dec->sample_fmt;
    enc->bits_per_raw_sample = j->dec->bits_per_raw_sample;
    enc->time_base = (AVRational){1, enc->sample_rate};
    if (j->out->oformat->flags & AVFMT_GLOBALHEADER)
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    int r = avcodec_open2(enc, j->encoder, NULL);
    if (r < 0) {
        fail_av(j, "encoder", r);
        return false;
    }
    if (avcodec_parameters_from_context(ost->codecpar, enc) < 0) {
        fail(j, "out of memory");
        return false;
    }
    ost->time_base = enc->time_base;
    j->fifo = av_audio_fifo_alloc(enc->sample_fmt, ka_av_channels(enc), VAR_FRAME);
    if (!j->fifo) {
        fail(j, "out of memory");
        return false;
    }
    j->next_pts = 0;
    return true;
}

static bool try_open_piece(job *j, int k, bool covers)
{
    piece *p = &j->pieces[k];
    int r = avformat_alloc_output_context2(&j->out, j->ofmt, NULL, p->part);
    if (r < 0 || !j->out) {
        fail_av(j, p->dest, r < 0 ? r : AVERROR(ENOMEM));
        return false;
    }
    const AVStream *ist = j->in->streams[j->ast];
    AVStream *ost = avformat_new_stream(j->out, NULL);
    if (!ost) {
        fail(j, "out of memory");
        return false;
    }
    if (j->exact) {
        if (!add_encoder(j, ost))
            return false;
    } else {
        if (avcodec_parameters_copy(ost->codecpar, ist->codecpar) < 0) {
            fail(j, "out of memory");
            return false;
        }
        ost->codecpar->codec_tag = 0;
        ost->time_base = ist->time_base;
    }
    int npics = covers && keeps_covers(j->ofmt) ? j->npics : 0;
    for (int i = 0; i < npics; i++) {
        const AVStream *pin = j->in->streams[j->pics[i]];
        AVStream *ps = avformat_new_stream(j->out, NULL);
        if (!ps || avcodec_parameters_copy(ps->codecpar, pin->codecpar) < 0) {
            fail(j, "out of memory");
            return false;
        }
        ps->codecpar->codec_tag = 0;
        ps->disposition = AV_DISPOSITION_ATTACHED_PIC;
        ps->time_base = pin->time_base;
        av_dict_copy(&ps->metadata, pin->metadata, 0);
    }
    j->out->metadata = piece_tags(j, k);
    if (tags_on_stream(j->ofmt))
        av_dict_copy(&ost->metadata, j->out->metadata, 0);
    r = avio_open(&j->out->pb, p->part, AVIO_FLAG_WRITE);
    if (r < 0) {
        fail_av(j, p->part, r);
        return false;
    }
    r = avformat_write_header(j->out, NULL);
    if (r < 0) {
        fail_av(j, p->dest, r);
        return false;
    }
    for (int i = 0; i < npics; i++) {
        AVPacket *pic = av_packet_clone(&j->in->streams[j->pics[i]]->attached_pic);
        if (!pic) {
            fail(j, "out of memory");
            return false;
        }
        pic->stream_index = i + 1;
        r = av_interleaved_write_frame(j->out, pic);
        av_packet_free(&pic);
        if (r < 0) {
            fail_av(j, p->dest, r);
            return false;
        }
    }
    return true;
}

/* A cover the muxer turns down only costs the cover. */
static bool open_piece(job *j, int k)
{
    if (try_open_piece(j, k, true))
        return true;
    if (!j->npics || !keeps_covers(j->ofmt))
        return false;
    close_out(j);
    unlink(j->pieces[k].part);
    j->err[0] = '\0';
    return try_open_piece(j, k, false);
}

static bool write_encoded(job *j, AVPacket *pkt)
{
    for (;;) {
        int r = avcodec_receive_packet(j->enc, pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
            return true;
        if (r < 0) {
            fail_av(j, "encoder", r);
            return false;
        }
        av_packet_rescale_ts(pkt, j->enc->time_base, j->out->streams[0]->time_base);
        pkt->stream_index = 0;
        r = av_interleaved_write_frame(j->out, pkt);
        av_packet_unref(pkt);
        if (r < 0) {
            fail_av(j, j->pieces[j->cur].dest, r);
            return false;
        }
    }
}

/* Whole frames from the FIFO to the encoder (and what is left, at the
 * end of the piece). */
static bool encode_fifo(job *j, AVPacket *pkt, bool last)
{
    int fs = j->enc->frame_size > 0 && !(j->encoder->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
                 ? j->enc->frame_size
                 : VAR_FRAME;
    int have;
    while ((have = av_audio_fifo_size(j->fifo)) >= fs || (last && have > 0)) {
        int n = have < fs ? have : fs;
        AVFrame *f = av_frame_alloc();
        if (!f || ka_av_frame_layout(f, j->enc) < 0) {
            av_frame_free(&f);
            fail(j, "out of memory");
            return false;
        }
        f->nb_samples = n;
        f->format = j->enc->sample_fmt;
        f->sample_rate = j->enc->sample_rate;
        if (av_frame_get_buffer(f, 0) < 0 ||
            av_audio_fifo_read(j->fifo, (void **)f->extended_data, n) != n) {
            av_frame_free(&f);
            fail(j, "out of memory");
            return false;
        }
        f->pts = j->next_pts;
        j->next_pts += n;
        int r = avcodec_send_frame(j->enc, f);
        av_frame_free(&f);
        if (r < 0) {
            fail_av(j, "encoder", r);
            return false;
        }
        if (!write_encoded(j, pkt))
            return false;
    }
    return true;
}

static bool finish_piece(job *j, AVPacket *pkt)
{
    piece *p = &j->pieces[j->cur];
    if (!j->out) {
        fail(j, "piece %d is empty: it starts after the end of the sound", j->cur + 1);
        return false;
    }
    if (j->exact) {
        if (!encode_fifo(j, pkt, true))
            return false;
        avcodec_send_frame(j->enc, NULL);
        if (!write_encoded(j, pkt))
            return false;
    }
    int r = av_write_trailer(j->out);
    if (r < 0) {
        fail_av(j, p->dest, r);
        return false;
    }
    r = avio_closep(&j->out->pb);
    if (r < 0) {
        fail_av(j, p->dest, r);
        return false;
    }
    close_out(j);
    p->written = true;
    return true;
}

/* ---- the cut ------------------------------------------------------------- */

static void report_progress(job *j, double t)
{
    if (j->duration <= 0)
        return;
    double f = t / j->duration;
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    if (f - j->progress >= 0.01) {
        j->progress = f;
        printf("progress %.2f\n", f);
        fflush(stdout);
    }
}

/* Moves on past the pieces that end at or before t: false on an error.
 * j->cur == j->n when all are done. */
static bool advance(job *j, double t, AVPacket *pkt)
{
    while (j->cur < j->n && j->pieces[j->cur].end >= 0 && t >= j->pieces[j->cur].end) {
        if (!finish_piece(j, pkt))
            return false;
        j->cur++;
    }
    return true;
}

/* A packet as it is: into the piece its start falls in. */
static bool copy_packet(job *j, AVPacket *pkt, AVPacket *scratch)
{
    const AVStream *ist = j->in->streams[j->ast];
    int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
    if (ts == AV_NOPTS_VALUE)
        ts = j->guess;
    j->guess = ts + (pkt->duration > 0 ? pkt->duration : 0);
    int64_t base = ist->start_time != AV_NOPTS_VALUE ? ist->start_time : 0;
    double t = (double)(ts - base) * av_q2d(ist->time_base);
    report_progress(j, t);
    if (!advance(j, t, scratch))
        return false;
    /* The first piece takes what comes before 0 too (an encoder's delay
     * puts the first packet a little before the stream's start). */
    bool first = j->cur == 0 && j->pieces[0].start <= 0;
    if (j->cur == j->n || (t < j->pieces[j->cur].start && !first))
        return true;
    if (!j->out) {
        if (!open_piece(j, j->cur))
            return false;
        j->offset = ts;
    }
    if (pkt->pts != AV_NOPTS_VALUE)
        pkt->pts -= j->offset;
    else
        pkt->pts = ts - j->offset;
    if (pkt->dts != AV_NOPTS_VALUE)
        pkt->dts -= j->offset;
    av_packet_rescale_ts(pkt, ist->time_base, j->out->streams[0]->time_base);
    pkt->stream_index = 0;
    pkt->pos = -1;
    int r = av_interleaved_write_frame(j->out, pkt);
    if (r < 0) {
        fail_av(j, j->pieces[j->cur].dest, r);
        return false;
    }
    return true;
}

static int64_t at_sample(const job *j, double seconds)
{
    return seconds < 0 ? INT64_MAX : llrint(seconds * j->dec->sample_rate);
}

/* Decoded samples, each into its piece, cut on the exact sample. */
static bool route_frame(job *j, const AVFrame *f, AVPacket *pkt)
{
    int bps = av_get_bytes_per_sample(j->dec->sample_fmt);
    int ch = ka_av_channels(j->dec);
    bool planar = av_sample_fmt_is_planar(j->dec->sample_fmt);
    int done = 0;
    while (done < f->nb_samples) {
        int64_t at = j->pos + done;
        if (!advance(j, (double)at / j->dec->sample_rate + 1e-9, pkt))
            return false;
        if (j->cur == j->n)
            break;
        const piece *p = &j->pieces[j->cur];
        int64_t start = at_sample(j, p->start), end = at_sample(j, p->end);
        int64_t left = f->nb_samples - done;
        if (at < start) {
            done += (int)(start - at < left ? start - at : left);
            continue;
        }
        int take = (int)(end - at < left ? end - at : left);
        if (!j->out && !open_piece(j, j->cur))
            return false;
        void *ptrs[MAX_CHANNELS];
        for (int c = 0; c < (planar ? ch : 1); c++)
            ptrs[c] = f->extended_data[c] + (size_t)done * bps * (planar ? 1 : ch);
        if (av_audio_fifo_write(j->fifo, ptrs, take) != take) {
            fail(j, "out of memory");
            return false;
        }
        if (!encode_fifo(j, pkt, false))
            return false;
        done += take;
    }
    j->pos += f->nb_samples;
    report_progress(j, (double)j->pos / j->dec->sample_rate);
    return true;
}

static bool decode_packet(job *j, const AVPacket *in, AVFrame *f, AVPacket *scratch)
{
    int r = avcodec_send_packet(j->dec, in);
    if (r < 0 && r != AVERROR_EOF) {
        fail_av(j, "decoder", r);
        return false;
    }
    while ((r = avcodec_receive_frame(j->dec, f)) >= 0) {
        bool ok = route_frame(j, f, scratch);
        av_frame_unref(f);
        if (!ok)
            return false;
    }
    if (r != AVERROR(EAGAIN) && r != AVERROR_EOF) {
        fail_av(j, "decoder", r);
        return false;
    }
    return true;
}

static bool run(job *j)
{
    AVPacket *pkt = av_packet_alloc(), *scratch = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    bool ok = pkt && scratch && f;
    if (!ok)
        fail(j, "out of memory");
    while (ok && j->cur < j->n) {
        if (stopping) {
            fail(j, "cancelled");
            ok = false;
            break;
        }
        int r = av_read_frame(j->in, pkt);
        if (r == AVERROR_EOF)
            break;
        if (r < 0) {
            fail_av(j, "read", r);
            ok = false;
            break;
        }
        if (pkt->stream_index == j->ast)
            ok = j->exact ? decode_packet(j, pkt, f, scratch) : copy_packet(j, pkt, scratch);
        av_packet_unref(pkt);
    }
    if (ok && j->exact && j->cur < j->n)
        ok = decode_packet(j, NULL, f, scratch);
    /* The sound is over: the piece being written ends here, and none may
     * be left that never started. */
    if (ok && j->cur < j->n)
        ok = finish_piece(j, scratch);
    if (ok && j->cur + 1 < j->n) {
        fail(j, "piece %d starts after the end of the sound", j->cur + 2);
        ok = false;
    }
    av_packet_free(&pkt);
    av_packet_free(&scratch);
    av_frame_free(&f);
    return ok;
}

/* The .part takes its name, never over a file that is there. link() says
 * so atomically; file systems without hard links (FAT) get a check and a
 * rename. */
static bool publish(job *j, const piece *p)
{
    if (link(p->part, p->dest) == 0) {
        unlink(p->part);
        return true;
    }
    if (errno == EEXIST || access(p->dest, F_OK) == 0) {
        fail(j, "%s already exists", p->dest);
        return false;
    }
    if (rename(p->part, p->dest) != 0) {
        fail(j, "%s: %s", p->dest, strerror(errno));
        return false;
    }
    return true;
}

/* ---- the job from stdin -------------------------------------------------- */

/* Splits s on tabs into at most max fields, in place. */
static int fields(char *s, char **out, int max)
{
    int n = 0;
    out[n++] = s;
    for (char *c = s; *c && n < max; c++)
        if (*c == '\t') {
            *c = '\0';
            out[n++] = c + 1;
        }
    return n;
}

static bool add_piece(job *j, char *line)
{
    char *f[6];
    if (fields(line, f, 6) != 6 || !f[5][0]) {
        fail(j, "bad piece line");
        return false;
    }
    piece *grown = realloc(j->pieces, (size_t)(j->n + 1) * sizeof *grown);
    if (!grown) {
        fail(j, "out of memory");
        return false;
    }
    j->pieces = grown;
    piece *p = &j->pieces[j->n++];
    memset(p, 0, sizeof *p);
    char *end1, *end2;
    p->start = strtod(f[0], &end1);
    p->end = strtod(f[1], &end2);
    p->title = strdup(f[2]);
    p->artist = strdup(f[3]);
    p->album = strdup(f[4]);
    p->dest = strdup(f[5]);
    size_t n = strlen(f[5]) + sizeof ".part";
    p->part = malloc(n);
    if (!p->title || !p->artist || !p->album || !p->dest || !p->part) {
        fail(j, "out of memory");
        return false;
    }
    snprintf(p->part, n, "%s.part", f[5]);
    if (*end1 || *end2 || p->start < 0) {
        fail(j, "bad piece line");
        return false;
    }
    return true;
}

static bool check_pieces(job *j)
{
    if (!j->n) {
        fail(j, "no pieces");
        return false;
    }
    for (int i = 0; i < j->n; i++) {
        const piece *p = &j->pieces[i];
        if ((p->end >= 0 && p->end <= p->start) || (p->end < 0 && i + 1 < j->n) ||
            (i > 0 && p->start < j->pieces[i - 1].end)) {
            fail(j, "piece %d: bad times", i + 1);
            return false;
        }
        if (access(p->dest, F_OK) == 0) {
            fail(j, "%s already exists", p->dest);
            return false;
        }
    }
    return true;
}

/* An output format for the destination's extension, else the input's own
 * ("mp3" of "mp3", "mov" of "mov,mp4,m4a,..."). */
static ka_av_oformat *pick_format(const job *j)
{
    ka_av_oformat *f = av_guess_format(NULL, j->pieces[0].dest, NULL);
    if (f)
        return f;
    char name[32];
    snprintf(name, sizeof name, "%s", j->in->iformat->name);
    name[strcspn(name, ",")] = '\0';
    return av_guess_format(name, NULL, NULL);
}

static void free_job(job *j)
{
    close_out(j);
    close_input(j);
    for (int i = 0; i < j->n; i++) {
        free(j->pieces[i].title);
        free(j->pieces[i].artist);
        free(j->pieces[i].album);
        free(j->pieces[i].dest);
        free(j->pieces[i].part);
    }
    free(j->pieces);
}

int ka_split_serve(void)
{
    struct sigaction sa = {0};
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    job j = {0};
    char *line = NULL, *src = NULL;
    size_t cap = 0;
    ssize_t len;
    bool go = false, ok = true;
    while (ok && !go && (len = getline(&line, &cap, stdin)) > 0) {
        if (line[len - 1] == '\n')
            line[--len] = '\0';
        if (!strncmp(line, "src ", 4)) {
            free(src);
            src = strdup(line + 4);
        } else if (!strncmp(line, "piece ", 6))
            ok = add_piece(&j, line + 6);
        else if (!strcmp(line, "go"))
            go = true;
    }
    free(line);
    if (ok && (!go || !src)) {
        fail(&j, "incomplete job");
        ok = false;
    }
    ok = ok && check_pieces(&j) && open_input(&j, src);
    if (ok && !(j.ofmt = pick_format(&j))) {
        fail(&j, "no format to write %s", j.pieces[0].dest);
        ok = false;
    }
    if (ok) {
        printf("mode %s\n", j.exact ? "exact" : "copy");
        fflush(stdout);
        ok = run(&j);
    }
    for (int i = 0; ok && i < j.n; i++)
        ok = publish(&j, &j.pieces[i]);
    close_out(&j);
    if (!ok)
        for (int i = 0; i < j.n; i++)
            if (j.pieces[i].part)
                unlink(j.pieces[i].part);
    if (ok)
        printf("ok\n");
    else
        printf("error %s\n", j.err[0] ? j.err : "failed");
    fflush(stdout);
    free(src);
    free_job(&j);
    return ok ? 0 : 1;
}
