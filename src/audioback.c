/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gio/gio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "backend_impl.h"
#include "child.h"
#include "lineio.h"
#include "log.h"

/* kiplay-audio (audio/), Kiplay's own music engine, behind kp_backend.
 * It talks over its stdin/stdout with the line protocol of audio/cmd.h;
 * here the lines become the same events mpv's do. */

typedef struct {
    kp_backend base;
    char *program, *device, *soundfont;
    kp_event_cb cb;
    void *ud;
    int volume;
    kp_child *child;
    kp_lineio *io;
} audioback;

static void emit(audioback *a, kp_event_type type, double value, bool flag, const char *text)
{
    kp_event ev = {.type = type, .value = value, .flag = flag, .text = text};
    a->cb(a->ud, &ev);
}

/* "word rest": rest if line starts with that word. */
static const char *arg(const char *line, const char *word)
{
    size_t n = strlen(word);
    if (strncmp(line, word, n) != 0)
        return NULL;
    return line[n] == ' ' ? line + n + 1 : line[n] == '\0' ? line + n : NULL;
}

static void on_line(void *ud, const char *line)
{
    audioback *a = ud;
    if (!line)
        return; /* the process is going away; its exit reports it */
    if (!arg(line, "time"))
        kp_log_debug("audio< %s", line);
    const char *v;
    if ((v = arg(line, "time")))
        emit(a, KP_EV_TIME, g_ascii_strtod(v, NULL), false, NULL);
    else if (arg(line, "loaded")) {
        /* Never a video here: the area goes if the last file had one. */
        emit(a, KP_EV_VIDEO, 0, false, NULL);
        emit(a, KP_EV_LOADED, 0, false, NULL);
    } else if ((v = arg(line, "duration")))
        emit(a, KP_EV_DURATION, g_ascii_strtod(v, NULL), false, NULL);
    else if ((v = arg(line, "tags"))) {
        emit(a, KP_EV_TITLE, 0, false, v);
        emit(a, KP_EV_TAGS, 0, false, v);
    } else if ((v = arg(line, "paused")))
        emit(a, KP_EV_PAUSED, 0, *v == '1', NULL);
    else if (arg(line, "next"))
        emit(a, KP_EV_NEXT, 0, false, NULL);
    else if ((v = arg(line, "ended"))) {
        const char *msg = arg(v, "error");
        emit(a, KP_EV_ENDED, 0, msg != NULL, msg);
    }
}

static void send(audioback *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void send(audioback *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *line = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    kp_lineio_send(a->io, line);
    g_free(line);
}

/* Numbers always with a dot, whatever the locale. */
static const char *num(char *buf, double v)
{
    return g_ascii_formatd(buf, G_ASCII_DTOSTR_BUF_SIZE, "%.3f", v);
}

/* The path as one protocol line: a name with a newline goes as a URI. */
static char *one_line(const char *path)
{
    if (!strchr(path, '\n'))
        return g_strdup(path);
    char *uri = g_filename_to_uri(path, NULL, NULL);
    return uri ? uri : g_strdelimit(g_strdup(path), "\n", ' ');
}

static void on_exit(void *ud, const char *why)
{
    audioback *a = ud;
    kp_log_debug("kiplay-audio: exited (%s)", why);
    kp_lineio_detach(a->io);
    kp_child_stop(a->child);
    a->child = NULL;
    emit(a, KP_EV_CRASHED, 0, false, why);
}

static bool start(kp_backend *b, char **err)
{
    audioback *a = (audioback *)b;
    if (a->child)
        return true;
    const char *argv[] = {a->program, "-d", a->device, "-s", a->soundfont, NULL};
    a->child = kp_child_spawn(argv, G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE,
                              on_exit, a, err);
    if (!a->child)
        return false;
    GSubprocess *p = kp_child_proc(a->child);
    kp_lineio_attach(a->io, g_subprocess_get_stdout_pipe(p), g_subprocess_get_stdin_pipe(p));
    send(a, "volume %d", a->volume);
    return true;
}

static bool running(const kp_backend *b)
{
    return ((const audioback *)b)->child != NULL;
}

static int pid(const kp_backend *b)
{
    return kp_child_pid(((const audioback *)b)->child);
}

static void load(kp_backend *b, const char *path, const kp_load_opts *lo)
{
    char n[G_ASCII_DTOSTR_BUF_SIZE];
    char *p = one_line(path);
    send((audioback *)b, "load %s %s", num(n, lo->start), p);
    g_free(p);
}

static void queue(kp_backend *b, const char *path)
{
    char *p = one_line(path ? path : "");
    send((audioback *)b, "queue %s", p);
    g_free(p);
}

static void stop(kp_backend *b)
{
    send((audioback *)b, "stop");
}

static void set_pause(kp_backend *b, bool paused)
{
    send((audioback *)b, "pause %d", paused);
}

static void seek(kp_backend *b, double seconds, bool relative)
{
    char n[G_ASCII_DTOSTR_BUF_SIZE];
    send((audioback *)b, "%s %s", relative ? "seekrel" : "seek", num(n, seconds));
}

static void set_volume(kp_backend *b, int volume)
{
    audioback *a = (audioback *)b;
    a->volume = volume;
    send(a, "volume %d", volume);
}

static void set_ticking(kp_backend *b, bool on)
{
    send((audioback *)b, "tick %d", on);
}

static void audioback_free(kp_backend *b)
{
    audioback *a = (audioback *)b;
    if (a->child) {
        send(a, "quit");
        kp_lineio_detach(a->io);
        kp_child_stop(a->child);
    }
    kp_lineio_free(a->io);
    g_free(a->program);
    g_free(a->device);
    g_free(a->soundfont);
    g_free(a);
}

static const kp_backend_ops audio_ops = {
    .name = "kiplay-audio",
    .caps = KP_CAP_GAPLESS,
    .start = start,
    .running = running,
    .pid = pid,
    .load = load,
    .queue = queue,
    .stop = stop,
    .set_pause = set_pause,
    .seek = seek,
    .set_volume = set_volume,
    .set_ticking = set_ticking,
    .free = audioback_free,
};

kp_backend *kp_backend_new_audio(const kp_backend_opts *opts, kp_event_cb cb, void *ud)
{
    audioback *a = g_new0(audioback, 1);
    a->base.ops = &audio_ops;
    a->program = g_strdup(opts->program ? opts->program : "kiplay-audio");
    a->device = g_strdup(opts->device && *opts->device ? opts->device : "default");
    a->soundfont = g_strdup(opts->soundfont ? opts->soundfont : "");
    a->volume = opts->volume;
    a->cb = cb;
    a->ud = ud;
    a->io = kp_lineio_new("kiplay-audio", on_line, a);
    return &a->base;
}
