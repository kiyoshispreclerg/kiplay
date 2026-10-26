/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ytdl.h"

#include <signal.h>
#include <string.h>

#include "i18n.h"
#include "log.h"

char **kp_ytdl_split_urls(const char *text)
{
    GPtrArray *out = g_ptr_array_new();
    char **lines = g_strsplit(text ? text : "", "\n", -1);
    for (char **l = lines; *l; l++) {
        const char *s = g_strstrip(*l);
        if (*s == '#')
            continue;
        char **words = g_strsplit_set(s, " \t\r", -1);
        for (char **w = words; *w; w++)
            if (**w)
                g_ptr_array_add(out, g_strdup(*w));
        g_strfreev(words);
    }
    g_strfreev(lines);
    g_ptr_array_add(out, NULL);
    return (char **)g_ptr_array_free(out, FALSE);
}

static bool nonempty(const char *s)
{
    return s && *s;
}

static void add(GPtrArray *a, const char *s)
{
    g_ptr_array_add(a, g_strdup(s));
}

static bool valid_items(const char *s)
{
    return strspn(s, "0123456789:,-") == strlen(s);
}

char **kp_ytdl_argv(const char *program, const kp_ytdl_opts *o, char **err)
{
    if (!o->urls || !o->urls[0]) {
        if (err)
            *err = g_strdup(_("Enter at least one link."));
        return NULL;
    }
    if (nonempty(o->items) && !valid_items(o->items)) {
        if (err)
            *err = g_strdup(_("The playlist items must look like 1:3 or 1,2,5."));
        return NULL;
    }
    char **extra = NULL;
    if (nonempty(o->extra)) {
        GError *e = NULL;
        if (!g_shell_parse_argv(o->extra, NULL, &extra, &e)) {
            if (err)
                *err = g_strdup_printf(_("Cannot read the extra arguments: %s"), e->message);
            g_clear_error(&e);
            return NULL;
        }
    }
    GPtrArray *a = g_ptr_array_new();
    add(a, program);
    add(a, "--newline"); /* one progress line per update, not carriage returns */
    if (nonempty(o->dest)) {
        add(a, "-P");
        add(a, o->dest);
    }
    if (nonempty(o->tmpl)) {
        add(a, "-o");
        add(a, o->tmpl);
    }
    if (o->audio_only) {
        add(a, "-x");
        if (nonempty(o->audio_format) && strcmp(o->audio_format, "best") != 0) {
            add(a, "--audio-format");
            add(a, o->audio_format);
        }
    } else {
        if (o->max_height > 0) {
            char *f = g_strdup_printf("bestvideo[height<=%d]+bestaudio/best[height<=%d]",
                                      o->max_height, o->max_height);
            add(a, "-f");
            g_ptr_array_add(a, f);
        }
        if (o->remux_mp4) {
            add(a, "--remux-video");
            add(a, "mp4");
        }
    }
    if (o->subs) {
        add(a, "--write-subs");
        if (o->auto_subs)
            add(a, "--write-auto-subs");
        add(a, "--sub-langs");
        add(a, nonempty(o->sub_langs) ? o->sub_langs : "all");
        add(a, "--sub-format");
        add(a, "srv3/best"); /* YouTube's styled format first, as Kiplay reads it */
    }
    if (o->thumbnail)
        add(a, "--write-thumbnail");
    if (o->restrict_names)
        add(a, "--restrict-filenames");
    if (o->no_playlist)
        add(a, "--no-playlist");
    if (nonempty(o->items)) {
        add(a, "-I");
        add(a, o->items);
    }
    if (o->skip_download)
        add(a, "--skip-download");
    for (char **x = extra; x && *x; x++)
        add(a, *x);
    g_strfreev(extra);
    add(a, "--");
    for (char *const *u = o->urls; *u; u++)
        add(a, *u);
    g_ptr_array_add(a, NULL);
    return (char **)g_ptr_array_free(a, FALSE);
}

char *kp_ytdl_command_line(char *const *argv)
{
    GString *s = g_string_new(NULL);
    for (char *const *a = argv; *a; a++) {
        if (s->len)
            g_string_append_c(s, ' ');
        /* Plain words stay as they are; the rest is quoted for a shell. */
        bool plain = **a && strspn(*a, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                       "0123456789-_./:=,") == strlen(*a);
        if (plain) {
            g_string_append(s, *a);
        } else {
            char *q = g_shell_quote(*a);
            g_string_append(s, q);
            g_free(q);
        }
    }
    return g_string_free(s, FALSE);
}

/* ---- progress ------------------------------------------------------------ */

void kp_ytdl_progress_clear(kp_ytdl_progress *p)
{
    g_free(p->file);
    g_free(p->detail);
    *p = (kp_ytdl_progress){0};
}

static void set_str(char **dst, char *v)
{
    g_free(*dst);
    *dst = v;
}

/* The text after "[...] Destination: " or `Merging formats into "`, as a
 * file name, or NULL. */
static char *file_name_in(const char *line)
{
    const char *d = strstr(line, "] Destination: ");
    if (d) {
        d += strlen("] Destination: ");
        return g_path_get_basename(d);
    }
    d = strstr(line, "Merging formats into \"");
    if (d) {
        char *path = g_strndup(d + strlen("Merging formats into \""),
                               strlen(d) - strlen("Merging formats into \"") -
                                   (g_str_has_suffix(d, "\"") ? 1 : 0));
        char *base = g_path_get_basename(path);
        g_free(path);
        return base;
    }
    return NULL;
}

bool kp_ytdl_progress_feed(kp_ytdl_progress *p, const char *line)
{
    char *name = file_name_in(line);
    if (name) {
        set_str(&p->file, name);
        if (strstr(line, "[download]")) {
            p->percent = 0;
            set_str(&p->detail, NULL);
        }
        return true;
    }
    if (!g_str_has_prefix(line, "[download]"))
        return false;
    char *own = g_strstrip(g_strdup(line + strlen("[download]")));
    const char *rest = own;
    bool changed = false;

    /* "Downloading item 3 of 10" (older yt-dlp: "Downloading video 3 of 10") */
    if (g_str_has_prefix(rest, "Downloading ")) {
        const char *w = strchr(rest + strlen("Downloading "), ' ');
        char *end = NULL;
        gint64 n = w ? g_ascii_strtoll(w, &end, 10) : 0;
        if (n > 0 && end && g_str_has_prefix(end, " of ")) {
            gint64 m = g_ascii_strtoll(end + 4, NULL, 10);
            if (m > 0) {
                p->item = (int)n;
                p->items = (int)m;
                p->percent = 0;
                set_str(&p->file, NULL);
                set_str(&p->detail, NULL);
                changed = true;
            }
        }
    } else if (strstr(rest, "has already been downloaded")) {
        p->percent = 100;
        changed = true;
    } else {
        char *end = NULL;
        double v = g_ascii_strtod(rest, &end);
        if (end != rest && *end == '%' && v >= 0 && v <= 100) {
            p->percent = v;
            const char *d = end + 1;
            while (*d == ' ')
                d++;
            set_str(&p->detail, *d ? g_strdup(d) : NULL);
            changed = true;
        }
    }
    g_free(own);
    return changed;
}

double kp_ytdl_progress_fraction(const kp_ytdl_progress *p)
{
    double f = p->percent / 100.0;
    if (p->items > 0 && p->item > 0)
        f = ((p->item - 1) + f) / p->items;
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

/* ---- running ------------------------------------------------------------- */

struct kp_ytdl {
    GSubprocess *proc;
    GDataInputStream *in;
    GCancellable *cancel;
    kp_ytdl_line_cb line;
    kp_ytdl_done_cb done;
    void *ud;
    bool cancelled; /* we asked it to stop */
    bool silent;    /* the owner is gone: no callbacks */
    char *last_error;
};

static void y_free(kp_ytdl *y)
{
    g_object_unref(y->in);
    g_object_unref(y->cancel);
    g_object_unref(y->proc);
    g_free(y->last_error);
    g_free(y);
}

static void on_exit_status(GObject *src, GAsyncResult *res, gpointer data)
{
    kp_ytdl *y = data;
    GError *e = NULL;
    g_subprocess_wait_finish(G_SUBPROCESS(src), res, &e);
    g_clear_error(&e);
    if (!y->silent) {
        bool ok = g_subprocess_get_if_exited(y->proc) &&
                  g_subprocess_get_exit_status(y->proc) == 0;
        y->done(y->ud, y->cancelled,
                y->cancelled || ok ? NULL : (y->last_error ? y->last_error : _("yt-dlp failed.")));
    }
    y_free(y);
}

static void on_line(GObject *src, GAsyncResult *res, gpointer data)
{
    kp_ytdl *y = data;
    GError *e = NULL;
    char *line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(src), res, NULL, &e);
    g_clear_error(&e);
    if (y->silent) {
        g_free(line);
        y_free(y);
        return;
    }
    if (!line) { /* end of output (or cancelled): now the exit status */
        g_subprocess_wait_async(y->proc, NULL, on_exit_status, y);
        return;
    }
    kp_log_debug("yt-dlp< %s", line);
    if (g_str_has_prefix(line, "ERROR:")) {
        g_free(y->last_error);
        y->last_error = g_strstrip(g_strdup(line + 6));
    }
    y->line(y->ud, line);
    g_free(line);
    g_data_input_stream_read_line_async(y->in, G_PRIORITY_DEFAULT, y->cancel, on_line, y);
}

kp_ytdl *kp_ytdl_start(char *const *argv, kp_ytdl_line_cb line, kp_ytdl_done_cb done, void *ud,
                       char **err)
{
    GError *e = NULL;
    GSubprocess *p = g_subprocess_newv((const char *const *)argv,
                                       G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                           G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                       &e);
    if (!p) {
        if (err)
            *err = g_strdup(e->message);
        g_clear_error(&e);
        return NULL;
    }
    kp_ytdl *y = g_new0(kp_ytdl, 1);
    y->proc = p;
    y->in = g_data_input_stream_new(g_subprocess_get_stdout_pipe(p));
    g_data_input_stream_set_newline_type(y->in, G_DATA_STREAM_NEWLINE_TYPE_ANY);
    y->cancel = g_cancellable_new();
    y->line = line;
    y->done = done;
    y->ud = ud;
    g_data_input_stream_read_line_async(y->in, G_PRIORITY_DEFAULT, y->cancel, on_line, y);
    return y;
}

void kp_ytdl_cancel(kp_ytdl *y)
{
    y->cancelled = true;
    g_subprocess_send_signal(y->proc, SIGTERM);
}

void kp_ytdl_abort(kp_ytdl *y)
{
    y->silent = true;
    g_subprocess_send_signal(y->proc, SIGTERM);
    g_cancellable_cancel(y->cancel);
}
