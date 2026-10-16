/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "app.h"
#include "i18n.h"
#include "log.h"
#include "media.h"
#include "sidecar.h"
#include "ytdlp.h"

/* "Download subtitles from YouTube": for a video named `Title [ID].ext`
 * (yt-dlp's own naming) asks yt-dlp what the video has, lets the person
 * choose the languages, downloads them next to the file and gives them to
 * the player that is playing it. Nothing here blocks: yt-dlp runs as a
 * child (ytdlp.c) and answers on the main loop.
 *
 * Memory: the job lives from the click to the end of the download. */

typedef struct {
    kp_app *app;
    char *program, *id, *video, *location; /* location: what was playing */
    GHashTable *before;                    /* paths of the sidecars at the start */
    char **langs;
} job;

static bool busy;

static void job_free(job *j)
{
    g_free(j->program);
    g_free(j->id);
    g_free(j->video);
    g_free(j->location);
    if (j->before)
        g_hash_table_destroy(j->before);
    g_strfreev(j->langs);
    g_free(j);
    busy = false;
}

/* The playing local video's path, or NULL. g_free it. */
static char *playing_video(kp_app *app)
{
    const char *loc = app->playing.location;
    if (!loc || !app->playing.loaded || kp_media_kind_of(loc) != KP_MEDIA_VIDEO)
        return NULL;
    if (g_str_has_prefix(loc, "file://"))
        return g_filename_from_uri(loc, NULL, NULL);
    return loc[0] == '/' ? g_strdup(loc) : NULL;
}

bool kp_subdl_possible(kp_app *app)
{
    char *video = playing_video(app);
    char *id = video ? kp_sidecar_video_id(video) : NULL;
    bool ok = id != NULL;
    g_free(video);
    g_free(id);
    return ok;
}

static void message_box(kp_app *app, GtkMessageType type, const char *text, const char *detail)
{
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL, type,
                                          GTK_BUTTONS_CLOSE, "%s", text);
    if (detail)
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", detail);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static GHashTable *sidecar_paths(const char *video)
{
    GHashTable *set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GPtrArray *found = kp_sidecars_find(video);
    for (guint i = 0; i < found->len; i++)
        g_hash_table_add(set, g_strdup(((kp_sidecar *)g_ptr_array_index(found, i))->path));
    g_ptr_array_free(found, TRUE);
    return set;
}

/* The files that appeared go into the playing video; the first is shown. */
static void downloaded(void *ud, const char *err)
{
    job *j = ud;
    kp_app *app = j->app;
    if (err) {
        message_box(app, GTK_MESSAGE_ERROR, _("Could not download the subtitles."), err);
        job_free(j);
        return;
    }
    bool same = app->playing.location && !strcmp(app->playing.location, j->location);
    kp_backend *b = app->playing.backend;
    kp_ytass_opts o;
    kp_sidecar_ass_opts(app->prefs, &o);
    GPtrArray *found = kp_sidecars_find(j->video);
    int added = 0;
    for (guint i = 0; i < found->len; i++) {
        const kp_sidecar *sc = g_ptr_array_index(found, i);
        if (sc->kind == KP_SIDECAR_CHAT || g_hash_table_contains(j->before, sc->path))
            continue;
        added++;
        if (!same || !b || !(kp_backend_caps(b) & KP_CAP_TRACKS))
            continue;
        char *e = NULL;
        char *file = kp_sidecar_play_path(sc, &o, NULL, &e);
        if (!file) {
            kp_log_warn(_("cannot read the captions %s: %s"), sc->path, e);
            g_free(e);
            continue;
        }
        char *lang = kp_ytdlp_player_lang(sc->lang);
        kp_sub_file f = {file, *sc->lang ? sc->lang : NULL, *lang ? lang : NULL};
        kp_backend_add_sub(b, &f, added == 1);
        g_free(lang);
        g_free(file);
    }
    g_ptr_array_free(found, TRUE);
    kp_player_message(app, added ? _("Subtitles downloaded")
                                 : _("Those subtitles were already downloaded"));
    job_free(j);
}

/* Which languages start checked: the preferred ones ([subtitles] languages),
 * compared by their main part (pt-BR matches pt); else the first. */
static bool preferred(const kp_app *app, const char *lang)
{
    const char *list = kp_prefs_get_str(app->prefs, "subtitles", "languages", "");
    char **want = g_strsplit(list ? list : "", ",", -1);
    bool hit = false;
    size_t n = strcspn(lang, "-_");
    for (char **w = want; *w && !hit; w++) {
        char *s = g_strstrip(*w);
        hit = *s && strlen(s) >= n && !g_ascii_strncasecmp(s, lang, n) &&
              (s[n] == '\0' || s[n] == '-' || s[n] == '_');
    }
    g_strfreev(want);
    return hit;
}

/* The languages to download, NULL-terminated, or NULL if cancelled. */
static char **choose(kp_app *app, GPtrArray *subs)
{
    GtkWidget *d = gtk_dialog_new_with_buttons(
        _("Download subtitles from YouTube"), GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
        _("_Download"), GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
    GtkWidget *box = gtk_vbox_new(FALSE, 4);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new(_("Languages available for this video:")),
                       FALSE, FALSE, 0);
    GtkWidget **checks = g_new0(GtkWidget *, subs->len + 1);
    bool any = false;
    for (guint i = 0; i < subs->len; i++) {
        const kp_ytdlp_sub *s = g_ptr_array_index(subs, i);
        char *lang = kp_ytdlp_player_lang(s->lang);
        char *label = s->automatic ? g_strdup_printf(_("%s (automatic)"), s->name ? s->name : lang)
                                   : g_strdup(s->name ? s->name : lang);
        char *full = g_strdup_printf("%s — %s", label, s->lang);
        checks[i] = gtk_check_button_new_with_label(full);
        bool on = preferred(app, lang);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checks[i]), on);
        any = any || on;
        gtk_box_pack_start(GTK_BOX(box), checks[i], FALSE, FALSE, 0);
        g_free(full);
        g_free(label);
        g_free(lang);
    }
    if (!any && subs->len)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checks[0]), TRUE);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_widget_set_size_request(scroll, 380, subs->len > 8 ? 300 : -1);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), scroll, TRUE, TRUE, 0);
    gtk_widget_show_all(d);

    char **out = NULL;
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        GPtrArray *langs = g_ptr_array_new();
        for (guint i = 0; i < subs->len; i++)
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(checks[i])))
                g_ptr_array_add(langs, g_strdup(((kp_ytdlp_sub *)g_ptr_array_index(subs, i))->lang));
        g_ptr_array_add(langs, NULL);
        out = (char **)g_ptr_array_free(langs, FALSE);
    }
    gtk_widget_destroy(d);
    g_free(checks);
    return out;
}

static void listed(void *ud, GPtrArray *subs, const char *err)
{
    job *j = ud;
    kp_app *app = j->app;
    if (!subs) {
        message_box(app, GTK_MESSAGE_ERROR,
                    _("This does not look like an available YouTube video."), err);
        job_free(j);
        return;
    }
    if (!subs->len) {
        message_box(app, GTK_MESSAGE_INFO, _("This video has no subtitles on YouTube."), NULL);
        job_free(j);
        return;
    }
    j->langs = choose(app, subs);
    if (!j->langs || !j->langs[0]) {
        job_free(j);
        return;
    }
    /* Where yt-dlp writes: the video's path without its extension. */
    char *base = g_strdup(j->video);
    char *dot = strrchr(base, '.');
    if (dot && !strchr(dot, '/'))
        *dot = '\0';
    kp_player_message(app, _("Downloading subtitles..."));
    kp_ytdlp_download(j->program, j->id, base, (const char *const *)j->langs, downloaded, j);
    g_free(base);
}

void kp_subdl_start(kp_app *app)
{
    if (busy)
        return;
    char *video = playing_video(app);
    char *id = video ? kp_sidecar_video_id(video) : NULL;
    if (!id) {
        message_box(app, GTK_MESSAGE_INFO, _("Cannot look for subtitles for this file."),
                    _("The file name has to end with the video's YouTube ID in square "
                      "brackets, such as \"Title [dQw4w9WgXcQ].mp4\"."));
        g_free(video);
        return;
    }
    char *err = NULL;
    char *program = kp_ytdlp_program(app->prefs, &err);
    if (!program) {
        message_box(app, GTK_MESSAGE_ERROR, _("yt-dlp is not available."), err);
        g_free(err);
        g_free(video);
        g_free(id);
        return;
    }
    busy = true;
    job *j = g_new0(job, 1);
    *j = (job){app, program, id, video, g_strdup(app->playing.location),
               sidecar_paths(video), NULL};
    kp_player_message(app, _("Looking for subtitles on YouTube..."));
    kp_ytdlp_list(program, id, listed, j);
}
