/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdarg.h>
#include <string.h>

#include "backend_impl.h"
#include "child.h"
#include "lineio.h"
#include "log.h"

/* MPlayer in -slave -idle mode: commands as lines on its stdin, answers
 * (ANS_*) and -identify's ID_* lines on its stdout. It has no events, so
 * what mpv pushes is rebuilt here:
 *
 * - the start of a file is the "Starting playback..." line, after the
 *   ID_* lines that told the length, whether there is a video and the tags;
 * - the time is asked for 4 times a second while a file is loaded
 *   (get_property time_pos), and the same question finds the end: with
 *   nothing playing the answer is ANS_ERROR (idle mode prints no
 *   ID_EXIT=EOF);
 * - a file that can't be opened is the "Failed to open <file>." line.
 *
 * Every command goes out as "pausing_keep": without it MPlayer takes any
 * command as a reason to leave the pause. It has no set_property pause, so
 * the pause is a toggle and we remember which way it is.
 *
 * No KP_CAP_TRACKS and no KP_CAP_GAPLESS: the menus for tracks, delays and
 * aspect stay greyed, and files follow each other with a short gap. */

#define TICK_MS 250

typedef struct {
    kp_backend base;
    kp_backend_opts opts;
    char *program, *extra, *sub_langs;
    char **args, **test_args;
    kp_event_cb cb;
    void *ud;

    kp_child *child;
    kp_lineio *io;
    guint tick_timer;
    bool ticking;

    /* The file: opening (loadfile sent, no "Starting playback" yet), then
     * loaded. */
    bool opening, loaded;
    char *path;
    double start;
    bool paused;
    bool want_pause;           /* asked before it was loaded */
    GArray *subs;              /* owned kp_sub_file strings, loaded once playing */
    int sub_sid;               /* lo->sid: 0 = none, else the choice is ours */
    char *sub_pick;            /* the file of subs to show; once MPlayer numbers it */
    int last_sub_id;           /* the latest ID_FILE_SUB_ID */
    /* What the ID_* lines said about the file being opened. */
    bool has_video;
    double duration;
    char *clip_name;           /* the last ID_CLIP_INFO_NAMEn */
    char *tag_title, *tag_artist;
    /* Time questions asked and not yet answered, and how many of those
     * belong to a file we have moved on from. */
    int polls_out, polls_stale;
} mplayer;

static void emit(mplayer *m, kp_event_type type, double value, bool flag, const char *text)
{
    kp_event ev = {.type = type, .value = value, .flag = flag, .text = text};
    m->cb(m->ud, &ev);
}

/* ---- commands ------------------------------------------------------------ */

static void send(mplayer *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void send(mplayer *m, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *line = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    kp_lineio_send(m->io, line);
    g_free(line);
}

/* A string argument in MPlayer's command syntax: double quotes, with
 * backslash for " and \. A line break would end the command. */
static char *quote(const char *s)
{
    GString *o = g_string_new("\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            g_string_append_c(o, '\\');
        g_string_append_c(o, *s == '\n' || *s == '\r' ? ' ' : *s);
    }
    g_string_append_c(o, '"');
    return g_string_free(o, FALSE);
}

static char *num(double v)
{
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    return g_strdup(g_ascii_formatd(buf, sizeof buf, "%.3f", v));
}

static void clear_subs(mplayer *m)
{
    if (!m->subs)
        return;
    for (guint i = 0; i < m->subs->len; i++) {
        kp_sub_file *f = &g_array_index(m->subs, kp_sub_file, i);
        g_free((char *)f->path);
        g_free((char *)f->lang);
    }
    g_array_set_size(m->subs, 0);
}

/* The file whose subtitles show: the first one in the preferred languages
 * (as "ko" matches "ko" and "ko-KR"), else the first file. NULL for none. */
static const char *pick_sub(const mplayer *m)
{
    if (!m->subs || m->subs->len == 0 || m->sub_sid == 0)
        return NULL;
    char **langs = g_strsplit(m->sub_langs ? m->sub_langs : "", ",", -1);
    const char *pick = NULL;
    for (char **l = langs; *l && !pick; l++) {
        g_strstrip(*l);
        size_t n = strlen(*l);
        for (guint i = 0; n && i < m->subs->len && !pick; i++) {
            const kp_sub_file *f = &g_array_index(m->subs, kp_sub_file, i);
            if (f->lang && !g_ascii_strncasecmp(f->lang, *l, n) &&
                (f->lang[n] == '\0' || f->lang[n] == '-' || f->lang[n] == '_'))
                pick = f->path;
        }
    }
    g_strfreev(langs);
    return pick ? pick : g_array_index(m->subs, kp_sub_file, 0).path;
}

static void toggle_pause(mplayer *m, bool paused)
{
    if (paused == m->paused)
        return;
    m->paused = paused;
    send(m, "pause");
    emit(m, KP_EV_PAUSED, 0, paused, NULL);
}

/* ---- what it says -------------------------------------------------------- */

static void forget_file(mplayer *m)
{
    m->has_video = false;
    m->duration = -1;
    g_clear_pointer(&m->clip_name, g_free);
    g_clear_pointer(&m->tag_title, g_free);
    g_clear_pointer(&m->tag_artist, g_free);
}

static const char *after(const char *line, const char *prefix)
{
    return g_str_has_prefix(line, prefix) ? line + strlen(prefix) : NULL;
}

/* The playing file's picture of itself, as the events mpv would send. */
static void started(mplayer *m)
{
    m->opening = false;
    m->loaded = true;
    emit(m, KP_EV_VIDEO, 0, m->has_video, NULL);
    emit(m, KP_EV_DURATION, m->duration, false, NULL);
    char *tags = NULL;
    if (m->tag_title && m->tag_artist)
        tags = g_strdup_printf("%s - %s", m->tag_artist, m->tag_title);
    char *base = g_path_get_basename(m->path ? m->path : "");
    emit(m, KP_EV_TITLE, 0, false, tags ? tags : m->tag_title ? m->tag_title : base);
    emit(m, KP_EV_TAGS, 0, false, tags ? tags : m->tag_title);
    g_free(tags);
    g_free(base);
    emit(m, KP_EV_PAUSED, 0, false, NULL); /* loadfile leaves the pause */
    m->paused = false;
    emit(m, KP_EV_LOADED, 0, false, NULL);
    if (m->start > 0) {
        char *s = num(m->start);
        send(m, "pausing_keep seek %s 2", s);
        g_free(s);
    }
    /* Subtitle files: sub_load only adds each; the one to show is chosen
     * when MPlayer says which number it got (ID_FILE_SUB_ID). */
    g_free(m->sub_pick);
    m->sub_pick = g_strdup(pick_sub(m));
    if (m->subs)
        for (guint i = 0; i < m->subs->len; i++) {
            char *q = quote(g_array_index(m->subs, kp_sub_file, i).path);
            send(m, "pausing_keep sub_load %s", q);
            g_free(q);
        }
    clear_subs(m);
    if (m->want_pause)
        toggle_pause(m, true);
    m->want_pause = false;
}

static void on_line(void *ud, const char *line)
{
    mplayer *m = ud;
    if (!line)
        return; /* the process is going away; its exit reports it */
    if (!g_str_has_prefix(line, "ANS_time_pos") && !g_str_has_prefix(line, "ANS_ERROR"))
        kp_log_debug("mplayer< %.200s", line);
    const char *v;
    if ((v = after(line, "ANS_time_pos="))) {
        if (m->polls_out > 0)
            m->polls_out--;
        if (m->polls_stale > 0)
            m->polls_stale--;
        else if (m->loaded && m->ticking)
            emit(m, KP_EV_TIME, g_ascii_strtod(v, NULL), false, NULL);
    } else if (g_str_has_prefix(line, "ANS_ERROR=")) {
        /* Nothing is playing: the file ran out. */
        if (m->polls_out > 0)
            m->polls_out--;
        if (m->polls_stale > 0)
            m->polls_stale--;
        else if (m->loaded) {
            m->loaded = false;
            emit(m, KP_EV_ENDED, 0, false, NULL);
        }
    } else if ((v = after(line, "ID_FILE_SUB_ID="))) {
        m->last_sub_id = (int)g_ascii_strtoll(v, NULL, 10);
    } else if ((v = after(line, "ID_FILE_SUB_FILENAME=")) && m->sub_pick &&
               !strcmp(v, m->sub_pick)) {
        send(m, "pausing_keep sub_select %d", m->last_sub_id);
        g_clear_pointer(&m->sub_pick, g_free);
    } else if (!m->opening) {
        return;
    } else if (g_str_has_prefix(line, "ID_VIDEO_ID=")) {
        m->has_video = true;
    } else if ((v = after(line, "ID_LENGTH="))) {
        double d = g_ascii_strtod(v, NULL);
        m->duration = d > 0 ? d : -1;
    } else if ((v = after(line, "ID_CLIP_INFO_NAME")) && strchr(v, '=')) {
        g_free(m->clip_name);
        m->clip_name = g_strdup(strchr(v, '=') + 1);
    } else if ((v = after(line, "ID_CLIP_INFO_VALUE")) && strchr(v, '=') && m->clip_name) {
        const char *value = strchr(v, '=') + 1;
        if (*value && !g_ascii_strcasecmp(m->clip_name, "title")) {
            g_free(m->tag_title);
            m->tag_title = g_strdup(value);
        } else if (*value && !g_ascii_strcasecmp(m->clip_name, "artist")) {
            g_free(m->tag_artist);
            m->tag_artist = g_strdup(value);
        }
    } else if (!strcmp(line, "Starting playback...")) {
        started(m);
    } else if (m->path && g_str_has_prefix(line, "Failed to open ") &&
               g_str_has_suffix(line, ".") && strstr(line, m->path)) {
        m->opening = false;
        clear_subs(m);
        emit(m, KP_EV_ENDED, 0, true, line);
    }
}

static gboolean on_tick(gpointer data)
{
    mplayer *m = data;
    if (m->loaded) {
        m->polls_out++;
        send(m, "pausing_keep_force get_property time_pos");
    }
    return G_SOURCE_CONTINUE;
}

static void set_ticking(kp_backend *b, bool on)
{
    mplayer *m = (mplayer *)b;
    m->ticking = on;
}

/* The poll timer runs while a file is open, ticking or not: it is also
 * how the end of the file is noticed. */
static void start_polling(mplayer *m)
{
    if (!m->tick_timer)
        m->tick_timer = g_timeout_add(TICK_MS, on_tick, m);
}

static void stop_polling(mplayer *m)
{
    if (m->tick_timer) {
        g_source_remove(m->tick_timer);
        m->tick_timer = 0;
    }
}

/* ---- the process --------------------------------------------------------- */

static void on_exit(void *ud, const char *why)
{
    mplayer *m = ud;
    kp_log_debug("mplayer: exited (%s)", why);
    stop_polling(m);
    kp_lineio_detach(m->io);
    kp_child_stop(m->child);
    m->child = NULL;
    m->loaded = m->opening = false;
    emit(m, KP_EV_CRASHED, 0, false, why);
}

static void shutdown_process(mplayer *m)
{
    stop_polling(m);
    if (!m->child)
        return;
    send(m, "quit");
    kp_lineio_detach(m->io);
    kp_child_stop(m->child);
    m->child = NULL;
    m->loaded = m->opening = false;
}

static void mplayer_free(kp_backend *b)
{
    mplayer *m = (mplayer *)b;
    shutdown_process(m);
    kp_lineio_free(m->io);
    clear_subs(m);
    if (m->subs)
        g_array_free(m->subs, TRUE);
    forget_file(m);
    g_free(m->path);
    g_free(m->sub_pick);
    g_free(m->sub_langs);
    g_free(m->program);
    g_free(m->extra);
    g_strfreev(m->args);
    g_strfreev(m->test_args);
    g_free(m);
}

static bool running(const kp_backend *b)
{
    return ((const mplayer *)b)->child != NULL;
}

static int pid(const kp_backend *b)
{
    return kp_child_pid(((const mplayer *)b)->child);
}

static bool start(kp_backend *b, char **err)
{
    mplayer *m = (mplayer *)b;
    if (m->child)
        return true;
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup(m->program));
    if (!m->opts.use_config)
        g_ptr_array_add(argv, g_strdup("-noconfig")), g_ptr_array_add(argv, g_strdup("all"));
    /* Keys, mouse and the on-screen display are Kiplay's. -softvol keeps the
     * volume ours instead of the sound card's mixer; -fixed-vo keeps the
     * video window between files. */
    const char *fixed[] = {"-slave", "-idle", "-quiet", "-identify", "-fixed-vo", "-softvol",
                           "-nolirc", "-nojoystick", "-nomouseinput", "-noconsolecontrols",
                           "-nograbpointer", "-zoom", "-ass", "-osdlevel", "1", "-input",
                           "nodefault-bindings:conf=/dev/null", NULL};
    for (int i = 0; fixed[i]; i++)
        g_ptr_array_add(argv, g_strdup(fixed[i]));
    g_ptr_array_add(argv, g_strdup("-volume"));
    g_ptr_array_add(argv, g_strdup_printf("%d", m->opts.volume));
    if (m->opts.wid) {
        g_ptr_array_add(argv, g_strdup("-wid"));
        g_ptr_array_add(argv, g_strdup_printf("%lu", m->opts.wid));
    }
    for (char **t = m->args; t && *t; t++)
        g_ptr_array_add(argv, g_strdup(*t));
    for (char **t = m->test_args; t && *t; t++)
        g_ptr_array_add(argv, g_strdup(*t));
    if (m->extra && *m->extra) {
        int n;
        char **extra;
        GError *gerr = NULL;
        if (g_shell_parse_argv(m->extra, &n, &extra, &gerr)) {
            for (int i = 0; i < n; i++)
                g_ptr_array_add(argv, extra[i]);
            g_free(extra); /* the strings moved into argv */
        } else {
            kp_log_warn("mplayer: extra options: %s", gerr->message);
            g_error_free(gerr);
        }
    }
    g_ptr_array_add(argv, NULL);
    m->child = kp_child_spawn((const char *const *)argv->pdata,
                              G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                  G_SUBPROCESS_FLAGS_STDERR_MERGE,
                              on_exit, m, err);
    g_ptr_array_free(argv, TRUE);
    if (!m->child)
        return false;
    GSubprocess *p = kp_child_proc(m->child);
    kp_lineio_attach(m->io, g_subprocess_get_stdout_pipe(p), g_subprocess_get_stdin_pipe(p));
    m->polls_out = m->polls_stale = 0;
    return true;
}

static void load(kp_backend *b, const char *path, const kp_load_opts *lo)
{
    mplayer *m = (mplayer *)b;
    clear_subs(m);
    forget_file(m);
    if (lo->n_sub_files > 0) {
        if (!m->subs)
            m->subs = g_array_new(FALSE, FALSE, sizeof(kp_sub_file));
        for (int i = 0; i < lo->n_sub_files; i++) {
            kp_sub_file f = {g_strdup(lo->sub_files[i].path), NULL, g_strdup(lo->sub_files[i].lang)};
            g_array_append_val(m->subs, f);
        }
    }
    g_free(m->path);
    m->path = g_strdup(path);
    m->start = lo->start;
    m->sub_sid = lo->sid;
    g_clear_pointer(&m->sub_pick, g_free);
    m->opening = true;
    m->loaded = false;
    m->want_pause = false;
    m->polls_stale = m->polls_out; /* their answers are about the last file */
    start_polling(m);
    char *q = quote(path);
    send(m, "loadfile %s 0", q); /* the one command that should leave the pause */
    g_free(q);
    m->paused = false;
}

static void stop(kp_backend *b)
{
    mplayer *m = (mplayer *)b;
    m->polls_stale = m->polls_out;
    m->opening = m->loaded = false;
    m->paused = false;
    clear_subs(m);
    g_clear_pointer(&m->sub_pick, g_free);
    stop_polling(m);
    send(m, "stop");
    emit(m, KP_EV_IDLE, 0, false, NULL);
}

static void set_pause(kp_backend *b, bool paused)
{
    mplayer *m = (mplayer *)b;
    if (m->loaded)
        toggle_pause(m, paused);
    else if (m->opening)
        m->want_pause = paused;
}

static void seek(kp_backend *b, double seconds, bool relative)
{
    mplayer *m = (mplayer *)b;
    if (!m->loaded)
        return;
    char *s = num(seconds);
    send(m, "pausing_keep seek %s %d", s, relative ? 0 : 2);
    g_free(s);
}

static void step_frame(kp_backend *b, bool back)
{
    mplayer *m = (mplayer *)b;
    if (back || !m->loaded)
        return; /* MPlayer can't step back */
    send(m, "frame_step");
    if (!m->paused) {
        m->paused = true;
        emit(m, KP_EV_PAUSED, 0, true, NULL);
    }
}

static void set_volume(kp_backend *b, int volume)
{
    mplayer *m = (mplayer *)b;
    m->opts.volume = volume;
    send(m, "pausing_keep volume %d 1", volume);
}

static void show_text(kp_backend *b, const char *text)
{
    mplayer *m = (mplayer *)b;
    char *q = quote(text);
    send(m, "pausing_keep osd_show_text %s 1500 1", q);
    g_free(q);
}

static const kp_backend_ops mplayer_ops = {
    .name = "mplayer",
    .caps = KP_CAP_VIDEO,
    .start = start,
    .running = running,
    .pid = pid,
    .load = load,
    .stop = stop,
    .set_pause = set_pause,
    .seek = seek,
    .set_volume = set_volume,
    .set_ticking = set_ticking,
    .step_frame = step_frame,
    .show_text = show_text,
    .free = mplayer_free,
};

kp_backend *kp_backend_new_mplayer(const kp_backend_opts *opts, kp_event_cb cb, void *ud)
{
    mplayer *m = g_new0(mplayer, 1);
    m->base.ops = &mplayer_ops;
    m->opts = *opts;
    m->program = g_strdup(opts->program ? opts->program : "mplayer");
    m->extra = g_strdup(opts->extra);
    m->sub_langs = g_strdup(opts->sub_langs);
    m->args = g_strdupv((char **)opts->args);
    m->test_args = g_strdupv((char **)opts->test_args);
    m->opts.args = NULL;
    m->opts.program = m->program;
    m->opts.extra = m->extra;
    m->opts.sub_langs = m->sub_langs;
    m->opts.device = NULL;
    m->cb = cb;
    m->ud = ud;
    m->duration = -1;
    m->io = kp_lineio_new("mplayer", on_line, m);
    return &m->base;
}
