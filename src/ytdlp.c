/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ytdlp.h"

#include <string.h>

#include "i18n.h"
#include "json.h"
#include "log.h"

#define TIMEOUT_S 120

void kp_ytdlp_sub_free(kp_ytdlp_sub *s)
{
    if (!s)
        return;
    g_free(s->lang);
    g_free(s->name);
    g_free(s);
}

char *kp_ytdlp_program(const kp_prefs *p, char **err)
{
    const char *set = kp_prefs_get_str(p, "tools", "ytdlp", "");
    if (!set || !*set) {
        char *found = g_find_program_in_path("yt-dlp");
        if (!found && err)
            *err = g_strdup(_("yt-dlp was not found. Set its path in Preferences > Advanced."));
        return found;
    }
    char *path = g_str_has_prefix(set, "~/") ? g_build_filename(g_get_home_dir(), set + 2, NULL)
                                             : g_strdup(set);
    if (!g_file_test(path, G_FILE_TEST_IS_EXECUTABLE) ||
        g_file_test(path, G_FILE_TEST_IS_DIR)) {
        if (err)
            *err = g_strdup_printf(_("Cannot run yt-dlp at %s. Check its path in Preferences > "
                                     "Advanced."),
                                   path);
        g_free(path);
        return NULL;
    }
    return path;
}

static void add_sub(GPtrArray *out, const char *lang, const cJSON *formats, bool automatic)
{
    kp_ytdlp_sub *s = g_new0(kp_ytdlp_sub, 1);
    s->lang = g_strdup(lang);
    s->automatic = automatic;
    const cJSON *first = cJSON_IsArray(formats) ? cJSON_GetArrayItem(formats, 0) : NULL;
    const char *name = first ? kp_json_str(first, "name") : NULL;
    s->name = name && *name ? g_strdup(name) : NULL;
    g_ptr_array_add(out, s);
}

GPtrArray *kp_ytdlp_parse_subs(const char *json, char **err)
{
    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root) || !kp_json_str(root, "id")) {
        cJSON_Delete(root);
        if (err)
            *err = g_strdup(_("yt-dlp did not return a video."));
        return NULL;
    }
    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)kp_ytdlp_sub_free);
    const cJSON *manual = cJSON_GetObjectItem(root, "subtitles");
    for (const cJSON *c = cJSON_IsObject(manual) ? manual->child : NULL; c; c = c->next)
        if (c->string && strcmp(c->string, "live_chat") != 0)
            add_sub(out, c->string, c, false);
    /* The automatic captions in the video's language are "xx-orig" (or, in
     * older output, "xx"); every other key is a machine translation. */
    const char *lang = kp_json_str(root, "language");
    const cJSON *autos = cJSON_GetObjectItem(root, "automatic_captions");
    bool have_orig = false;
    for (const cJSON *c = cJSON_IsObject(autos) ? autos->child : NULL; c; c = c->next)
        have_orig = have_orig || (c->string && g_str_has_suffix(c->string, "-orig"));
    for (const cJSON *c = cJSON_IsObject(autos) ? autos->child : NULL; c; c = c->next) {
        if (!c->string || !strcmp(c->string, "live_chat"))
            continue;
        bool wanted = have_orig ? g_str_has_suffix(c->string, "-orig")
                                : lang && !strcmp(c->string, lang);
        if (wanted)
            add_sub(out, c->string, c, true);
    }
    cJSON_Delete(root);
    return out;
}

char *kp_ytdlp_player_lang(const char *lang)
{
    return g_str_has_suffix(lang, "-orig") ? g_strndup(lang, strlen(lang) - 5) : g_strdup(lang);
}

/* ---- running ------------------------------------------------------------- */

typedef struct {
    GSubprocess *proc;
    GCancellable *cancel;
    guint timeout;
    void (*finish)(void *job, const char *out, const char *error);
    void *ud;
    void (*cb)(void); /* the caller's callback, any type */
} job;

static void job_free(job *j)
{
    if (j->timeout)
        g_source_remove(j->timeout);
    g_object_unref(j->cancel);
    g_object_unref(j->proc);
    g_free(j);
}

static gboolean on_timeout(gpointer data)
{
    job *j = data;
    j->timeout = 0;
    g_subprocess_force_exit(j->proc);
    g_cancellable_cancel(j->cancel);
    return G_SOURCE_REMOVE;
}

/* The last line of yt-dlp's complaint ("ERROR: [youtube] ID: Video
 * unavailable"), which is the useful one. g_free it. */
static char *last_error(const char *stderr_text)
{
    char **lines = g_strsplit(stderr_text ? stderr_text : "", "\n", -1);
    char *msg = NULL;
    for (char **l = lines; *l; l++) {
        char *s = g_strstrip(*l);
        if (g_str_has_prefix(s, "ERROR:"))
            s = g_strstrip(s + 6);
        if (*s && !g_str_has_prefix(s, "WARNING:")) {
            g_free(msg);
            msg = g_strdup(s);
        }
    }
    g_strfreev(lines);
    return msg;
}

static void on_done(GObject *src, GAsyncResult *res, gpointer data)
{
    job *j = data;
    char *out = NULL, *errtext = NULL;
    GError *e = NULL;
    char *error = NULL;
    if (!g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, &errtext, &e)) {
        error = g_strdup(g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)
                             ? _("yt-dlp took too long.")
                             : e->message);
        g_clear_error(&e);
    } else if (!g_subprocess_get_if_exited(G_SUBPROCESS(src)) ||
               g_subprocess_get_exit_status(G_SUBPROCESS(src)) != 0) {
        char *last = last_error(errtext);
        error = last ? last : g_strdup(_("yt-dlp failed."));
    }
    j->finish(j, out, error);
    g_free(out);
    g_free(errtext);
    g_free(error);
    job_free(j);
}

static void run(const char *const *argv, void (*finish)(void *, const char *, const char *),
                void (*cb)(void), void *ud, GSubprocessFlags extra)
{
    GError *e = NULL;
    GSubprocess *p = g_subprocess_newv(
        argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE | extra, &e);
    if (!p) {
        finish(&(job){.cb = cb, .ud = ud}, NULL, e->message);
        g_clear_error(&e);
        return;
    }
    job *j = g_new0(job, 1);
    *j = (job){p, g_cancellable_new(), 0, finish, ud, cb};
    j->timeout = g_timeout_add_seconds(TIMEOUT_S, on_timeout, j);
    g_subprocess_communicate_utf8_async(p, NULL, j->cancel, on_done, j);
}

static char *video_url(const char *id)
{
    return g_strdup_printf("https://www.youtube.com/watch?v=%s", id);
}

static void list_finish(void *jp, const char *out, const char *error)
{
    job *j = jp;
    char *err = NULL;
    GPtrArray *subs = error ? NULL : kp_ytdlp_parse_subs(out, &err);
    ((kp_ytdlp_list_cb)j->cb)(j->ud, subs, error ? error : err);
    if (subs)
        g_ptr_array_free(subs, TRUE);
    g_free(err);
}

void kp_ytdlp_list(const char *program, const char *id, kp_ytdlp_list_cb done, void *ud)
{
    char *url = video_url(id);
    const char *argv[] = {program, "--skip-download", "--no-playlist", "--no-warnings", "-J", "--",
                          url, NULL};
    kp_log_debug("yt-dlp: %s -J %s", program, url);
    run(argv, list_finish, (void (*)(void))done, ud, 0);
    g_free(url);
}

static void get_finish(void *jp, const char *out, const char *error)
{
    (void)out;
    job *j = jp;
    ((kp_ytdlp_get_cb)j->cb)(j->ud, error);
}

void kp_ytdlp_download(const char *program, const char *id, const char *base,
                       const char *const *langs, kp_ytdlp_get_cb done, void *ud)
{
    char *url = video_url(id);
    /* yt-dlp reads % in the template: the video's own are doubled. */
    char **parts = g_strsplit(base, "%", -1);
    char *escaped = g_strjoinv("%%", parts);
    char *tmpl = g_strconcat(escaped, ".%(ext)s", NULL);
    char *list = g_strjoinv(",", (char **)langs);
    const char *argv[] = {program,       "--skip-download", "--no-playlist", "--no-warnings",
                          "--write-subs", "--write-auto-subs", "--sub-langs", list,
                          "--sub-format", "srv3/best",      "-o",            tmpl,
                          "--",          url,               NULL};
    kp_log_debug("yt-dlp: subtitles %s of %s into %s", list, url, tmpl);
    run(argv, get_finish, (void (*)(void))done, ud, 0);
    g_strfreev(parts);
    g_free(escaped);
    g_free(tmpl);
    g_free(list);
    g_free(url);
}
