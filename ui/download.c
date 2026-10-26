/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <string.h>

#include "app.h"
#include "i18n.h"
#include "log.h"
#include "ytdl.h"
#include "ytdlp.h"

/* File > Download...: links (one or many), a folder and yt-dlp's options in
 * a modal window. What is chosen becomes the yt-dlp command line; while it
 * runs the choices are locked, the output feeds a progress bar and
 * only Cancel works. The choices are remembered in [download] of
 * kiplay.ini.
 *
 * Memory: one window at a time (`current`), freed when it is destroyed; a
 * running yt-dlp is stopped with it. */

typedef struct {
    kp_app *app;
    char *program;
    GtkWidget *dlg, *inputs, *urls, *dest, *tmpl, *kind, *aformat, *height, *remux;
    GtkWidget *subs, *langs, *auto_subs, *thumb, *restrict_names, *no_playlist, *items;
    GtkWidget *skip, *extra, *log, *bar, *status, *go, *close;
    kp_ytdl *run;
    char **queue;     /* the links, one yt-dlp each */
    int link, links;  /* which one is running, of how many */
    GPtrArray *left;  /* the links not done yet (what the input keeps) */
    int failed;
    char *last_error;
    bool cancelling;
    kp_ytdl_progress prog;
} dl;

static dl *current;

static const char *const audio_formats[] = {"m4a", "mp3", "opus", "flac", "wav", "vorbis", "aac",
                                            "best"};
static const int heights[] = {0, 2160, 1440, 1080, 720, 480, 360};
/* Ready-made file name templates (yt-dlp's -o); the default keeps the
 * "Title [ID].ext" naming that Kiplay's subtitle download understands. */
static const char *const templates[] = {
    "%(title).80s [%(id)s].%(ext)s",
    "%(title)s.%(ext)s",
    "%(id)s.%(ext)s",
    "%(title).220s-%(id)s.%(ext)s",
    "%(playlist_index)s - %(title).220s-%(id)s.%(ext)s",
    "%(uploader)s/%(title).80s.%(ext)s",
    "%(playlist)s/%(playlist_index)s - %(title)s.%(ext)s",
    "%(playlist)s %(playlist_id)s/%(playlist_index)s-%(title).80s-%(id)s.%(ext)s",
};

static void message_box(dl *d, GtkMessageType type, const char *text, const char *detail)
{
    GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(d->dlg), GTK_DIALOG_MODAL, type,
                                          GTK_BUTTONS_CLOSE, "%s", text);
    if (detail)
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(m), "%s", detail);
    gtk_dialog_run(GTK_DIALOG(m));
    gtk_widget_destroy(m);
}

/* ---- reading the controls ------------------------------------------------ */

static char *entry_text(GtkWidget *e)
{
    return g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(e))));
}

static char *combo_entry_text(GtkWidget *c)
{
    return entry_text(gtk_bin_get_child(GTK_BIN(c)));
}

static bool on(GtkWidget *w)
{
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
}

static char *urls_text(dl *d)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->urls));
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    return gtk_text_buffer_get_text(b, &s, &e, FALSE);
}

static bool audio_selected(dl *d)
{
    return gtk_combo_box_get_active(GTK_COMBO_BOX(d->kind)) == 1;
}

/* "~/x" is the home; yt-dlp is not given a shell. g_free it. */
static char *expand_home(const char *path)
{
    return g_str_has_prefix(path, "~/") ? g_build_filename(g_get_home_dir(), path + 2, NULL)
                                        : g_strdup(path);
}

/* The argv for what the window holds, for the links `only` (NULL: the
 * ones typed); NULL with *err. g_strfreev it. */
static char **current_argv(dl *d, const char *program, char *const *only, char **err)
{
    char *text = urls_text(d);
    char **urls = only ? g_strdupv((char **)only) : kp_ytdl_split_urls(text);
    char *dest = entry_text(d->dest), *dest_x = expand_home(dest);
    char *tmpl = combo_entry_text(d->tmpl);
    char *langs = entry_text(d->langs), *items = entry_text(d->items);
    char *extra = entry_text(d->extra);
    int h = gtk_combo_box_get_active(GTK_COMBO_BOX(d->height));
    int f = gtk_combo_box_get_active(GTK_COMBO_BOX(d->aformat));
    kp_ytdl_opts o = {
        .urls = urls,
        .dest = dest_x,
        .tmpl = tmpl,
        .audio_only = audio_selected(d),
        .audio_format = f >= 0 ? audio_formats[f] : "",
        .max_height = h >= 0 ? heights[h] : 0,
        .remux_mp4 = on(d->remux),
        .subs = on(d->subs),
        .sub_langs = langs,
        .auto_subs = on(d->auto_subs),
        .thumbnail = on(d->thumb),
        .restrict_names = on(d->restrict_names),
        .no_playlist = on(d->no_playlist),
        .items = items,
        .skip_download = on(d->skip),
        .extra = extra,
    };
    char **argv = kp_ytdl_argv(program, &o, err);
    g_free(text);
    g_strfreev(urls);
    g_free(dest);
    g_free(dest_x);
    g_free(tmpl);
    g_free(langs);
    g_free(items);
    g_free(extra);
    return argv;
}

/* The options that stay for next time (not the links, not the items). */
static void remember(dl *d)
{
    kp_prefs *p = d->app->prefs;
    char *s = entry_text(d->dest);
    kp_prefs_set_str(p, "download", "dir", s);
    g_free(s);
    s = combo_entry_text(d->tmpl);
    kp_prefs_set_str(p, "download", "template", s);
    g_free(s);
    kp_prefs_set_bool(p, "download", "audio_only", audio_selected(d));
    int i = gtk_combo_box_get_active(GTK_COMBO_BOX(d->aformat));
    kp_prefs_set_str(p, "download", "audio_format", audio_formats[i < 0 ? 0 : i]);
    i = gtk_combo_box_get_active(GTK_COMBO_BOX(d->height));
    kp_prefs_set_int(p, "download", "max_height", heights[i < 0 ? 0 : i]);
    kp_prefs_set_bool(p, "download", "remux_mp4", on(d->remux));
    kp_prefs_set_bool(p, "download", "subtitles", on(d->subs));
    s = entry_text(d->langs);
    kp_prefs_set_str(p, "download", "sub_langs", s);
    g_free(s);
    kp_prefs_set_bool(p, "download", "auto_subs", on(d->auto_subs));
    kp_prefs_set_bool(p, "download", "thumbnail", on(d->thumb));
    kp_prefs_set_bool(p, "download", "restrict_names", on(d->restrict_names));
    kp_prefs_set_bool(p, "download", "no_playlist", on(d->no_playlist));
    s = entry_text(d->extra);
    kp_prefs_set_str(p, "download", "extra", s);
    g_free(s);
    if (kp_prefs_save(p) < 0)
        kp_log_warn(_("could not save the settings: %s"), g_strerror(errno));
}

/* ---- the window follows the choices -------------------------------------- */

static void refresh(dl *d)
{
    bool audio = audio_selected(d);
    gtk_widget_set_sensitive(d->aformat, audio);
    gtk_widget_set_sensitive(d->height, !audio);
    gtk_widget_set_sensitive(d->remux, !audio);
    gtk_widget_set_sensitive(d->langs, on(d->subs));
    gtk_widget_set_sensitive(d->auto_subs, on(d->subs));
}

static void on_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    refresh(data);
}

static void watch(dl *d, GtkWidget *w, const char *signal)
{
    g_signal_connect(w, signal, G_CALLBACK(on_changed), d);
}

static void set_running(dl *d, bool running)
{
    gtk_widget_set_sensitive(d->inputs, !running);
    gtk_widget_set_sensitive(d->go, !running);
    gtk_button_set_label(GTK_BUTTON(d->close), running ? _("_Cancel") : _("_Close"));
}

static void show_progress(dl *d)
{
    double f = kp_ytdl_progress_fraction(&d->prog);
    if (d->links > 1)
        f = (d->link + f) / d->links;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(d->bar), f);
    char *bar = d->prog.items > 0
                    ? g_strdup_printf(_("Item %d of %d — %.0f%%"), d->prog.item, d->prog.items,
                                      d->prog.percent)
                    : g_strdup_printf("%.0f%%", d->prog.percent);
    if (d->links > 1) {
        char *all = g_strdup_printf(_("Link %d of %d — %s"), d->link + 1, d->links, bar);
        g_free(bar);
        bar = all;
    }
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(d->bar), bar);
    char *status = d->prog.file ? g_strdup_printf("%s%s%s", d->prog.file,
                                                  d->prog.detail ? " — " : "",
                                                  d->prog.detail ? d->prog.detail : "")
                                : g_strdup("");
    gtk_label_set_text(GTK_LABEL(d->status), status);
    g_free(bar);
    g_free(status);
}

#define LOG_LINES 1000

/* One more line of yt-dlp's output in the details; the oldest go away. */
static void log_add(dl *d, const char *line)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->log));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(b, &end);
    gtk_text_buffer_insert(b, &end, line, -1);
    gtk_text_buffer_insert(b, &end, "\n", 1);
    if (gtk_text_buffer_get_line_count(b) > LOG_LINES + 200) {
        GtkTextIter start, cut;
        gtk_text_buffer_get_start_iter(b, &start);
        gtk_text_buffer_get_iter_at_line(b, &cut, 200);
        gtk_text_buffer_delete(b, &start, &cut);
    }
    gtk_text_buffer_get_end_iter(b, &end);
    GtkTextMark *m = gtk_text_buffer_get_mark(b, "end");
    gtk_text_buffer_move_mark(b, m, &end);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(d->log), m);
}

static void on_line(void *ud, const char *line)
{
    dl *d = ud;
    log_add(d, line);
    if (kp_ytdl_progress_feed(&d->prog, line))
        show_progress(d);
}

static void next_link(dl *d);

/* The input keeps only the links that are not done. */
static void sync_input(dl *d)
{
    g_ptr_array_add(d->left, NULL);
    char *text = g_strjoinv("\n", (char **)d->left->pdata);
    g_ptr_array_set_size(d->left, d->left->len - 1);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->urls)), text, -1);
    g_free(text);
}

static void queue_free(dl *d)
{
    g_strfreev(d->queue);
    d->queue = NULL;
    if (d->left)
        g_ptr_array_free(d->left, TRUE);
    d->left = NULL;
    g_free(d->last_error);
    d->last_error = NULL;
}

static void all_done(dl *d)
{
    set_running(d, false);
    if (d->failed) {
        char *text = g_strdup_printf(ngettext("%d link failed; it stays in the list.",
                                              "%d links failed; they stay in the list.",
                                              d->failed),
                                     d->failed);
        gtk_label_set_text(GTK_LABEL(d->status), text);
        message_box(d, GTK_MESSAGE_ERROR, text, d->last_error);
        g_free(text);
    } else {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(d->bar), 1);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(d->bar), _("Done"));
        gtk_label_set_text(GTK_LABEL(d->status), _("Download finished."));
        kp_player_message(d->app, _("Download finished"));
    }
    queue_free(d);
}

static void link_failed(dl *d, const char *err)
{
    d->failed++;
    g_free(d->last_error);
    d->last_error = g_strdup(err);
    char *shown = g_strdup_printf("ERROR: %s", err);
    log_add(d, shown);
    g_free(shown);
}

static void on_done(void *ud, bool cancelled, const char *err)
{
    dl *d = ud;
    d->run = NULL;
    d->cancelling = false;
    if (cancelled) {
        set_running(d, false);
        gtk_label_set_text(GTK_LABEL(d->status), _("Cancelled."));
        queue_free(d);
        return;
    }
    if (err) {
        link_failed(d, err);
    } else { /* done: out of the input */
        for (guint i = 0; i < d->left->len; i++)
            if (!strcmp(d->left->pdata[i], d->queue[d->link])) {
                g_ptr_array_remove_index(d->left, i);
                break;
            }
        sync_input(d);
    }
    d->link++;
    next_link(d);
}

/* Starts the next link of the queue, or ends. */
static void next_link(dl *d)
{
    while (d->queue[d->link]) {
        char *one[] = {d->queue[d->link], NULL};
        char *err = NULL;
        char **argv = current_argv(d, d->program, one, &err);
        if (!argv) {
            link_failed(d, err);
            g_free(err);
            d->link++;
            continue;
        }
        kp_ytdl_progress_clear(&d->prog);
        show_progress(d);
        char *line = kp_ytdl_command_line(argv);
        kp_log_debug("yt-dlp: %s", line);
        char *shown = g_strconcat(d->link ? "\n$ " : "$ ", line, NULL);
        log_add(d, shown);
        g_free(shown);
        g_free(line);
        d->run = kp_ytdl_start(argv, on_line, on_done, d, &err);
        g_strfreev(argv);
        if (d->run) {
            gtk_label_set_text(GTK_LABEL(d->status), _("Starting..."));
            return;
        }
        link_failed(d, err);
        g_free(err);
        d->link++;
    }
    all_done(d);
}

static void start(dl *d)
{
    char *err = NULL;
    char **argv = current_argv(d, d->program, NULL, &err); /* are the choices valid? */
    if (!argv) {
        message_box(d, GTK_MESSAGE_WARNING, err, NULL);
        g_free(err);
        return;
    }
    g_strfreev(argv);
    char *dest = entry_text(d->dest), *dest_x = expand_home(dest);
    if (*dest_x && g_mkdir_with_parents(dest_x, 0755) < 0) {
        char *msg = g_strdup_printf(_("Cannot create the folder %s: %s"), dest_x,
                                    g_strerror(errno));
        message_box(d, GTK_MESSAGE_ERROR, msg, NULL);
        g_free(msg);
        g_free(dest);
        g_free(dest_x);
        return;
    }
    g_free(dest);
    g_free(dest_x);
    remember(d);
    char *text = urls_text(d);
    d->queue = kp_ytdl_split_urls(text);
    g_free(text);
    d->links = (int)g_strv_length(d->queue);
    d->link = 0;
    d->failed = 0;
    d->left = g_ptr_array_new_with_free_func(g_free);
    for (char **u = d->queue; *u; u++)
        g_ptr_array_add(d->left, g_strdup(*u));
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->log)), "", 0);
    set_running(d, true);
    next_link(d);
}

static void on_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    dl *d = data;
    if (d->run)
        kp_ytdl_abort(d->run);
    queue_free(d);
    kp_ytdl_progress_clear(&d->prog);
    g_free(d->program);
    g_free(d);
    current = NULL;
}

static void on_response(GtkDialog *dlg, int response, gpointer data)
{
    dl *d = data;
    if (response == GTK_RESPONSE_ACCEPT) {
        start(d);
    } else if (d->run) { /* Cancel, Escape or the close button: stop the download only */
        if (!d->cancelling) {
            d->cancelling = true;
            gtk_label_set_text(GTK_LABEL(d->status), _("Cancelling..."));
            kp_ytdl_cancel(d->run);
        }
    } else {
        gtk_widget_destroy(GTK_WIDGET(dlg));
    }
}

static void on_browse(GtkWidget *b, gpointer data)
{
    (void)b;
    dl *d = data;
    GtkWidget *c = gtk_file_chooser_dialog_new(_("Folder for the downloads"), GTK_WINDOW(d->dlg),
                                               GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                               GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                               GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT, NULL);
    char *cur = entry_text(d->dest), *cur_x = expand_home(cur);
    if (*cur_x && g_file_test(cur_x, G_FILE_TEST_IS_DIR))
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(c), cur_x);
    if (gtk_dialog_run(GTK_DIALOG(c)) == GTK_RESPONSE_ACCEPT) {
        char *f = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(c));
        gtk_entry_set_text(GTK_ENTRY(d->dest), f);
        g_free(f);
    }
    gtk_widget_destroy(c);
    g_free(cur);
    g_free(cur_x);
}

/* ---- building the window ------------------------------------------------- */

static GtkWidget *row(GtkWidget *table, int *r, const char *label, GtkWidget *w)
{
    GtkWidget *l = gtk_label_new_with_mnemonic(label);
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), w);
    gtk_table_attach(GTK_TABLE(table), l, 0, 1, *r, *r + 1, GTK_FILL, GTK_FILL, 0, 0);
    gtk_table_attach(GTK_TABLE(table), w, 1, 2, *r, *r + 1, GTK_FILL | GTK_EXPAND, GTK_FILL, 0, 0);
    (*r)++;
    return w;
}

static GtkWidget *text_entry(const kp_prefs *p, const char *key, const char *def)
{
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e), kp_prefs_get_str(p, "download", key, def));
    return e;
}

static GtkWidget *check(const kp_prefs *p, const char *label, const char *key, bool def)
{
    GtkWidget *c = gtk_check_button_new_with_mnemonic(label);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), kp_prefs_get_bool(p, "download", key, def));
    return c;
}

/* The comma-separated languages the person set for subtitles, as yt-dlp
 * takes them, else "all". g_free it. */
static char *default_langs(const kp_prefs *p)
{
    const char *l = kp_prefs_get_str(p, "subtitles", "languages", "");
    GString *out = g_string_new(NULL);
    for (const char *c = l ? l : ""; *c; c++)
        if (*c != ' ')
            g_string_append_c(out, *c); /* "pt, en": drop the blanks */
    if (!out->len)
        g_string_append(out, "all");
    return g_string_free(out, FALSE);
}

static void build(dl *d)
{
    kp_prefs *p = d->app->prefs;
    d->dlg = gtk_dialog_new_with_buttons(_("Download"), GTK_WINDOW(d->app->window),
                                         GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, NULL);
    d->go = gtk_dialog_add_button(GTK_DIALOG(d->dlg), _("_Download"), GTK_RESPONSE_ACCEPT);
    d->close = gtk_dialog_add_button(GTK_DIALOG(d->dlg), _("_Close"), GTK_RESPONSE_CANCEL);
    gtk_window_set_default_size(GTK_WINDOW(d->dlg), 600, -1);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d->dlg));
    gtk_box_set_spacing(GTK_BOX(box), 6);
    gtk_container_set_border_width(GTK_CONTAINER(d->dlg), 8);

    d->inputs = gtk_vbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(box), d->inputs, TRUE, TRUE, 0);

    GtkWidget *l = gtk_label_new_with_mnemonic(_("_Links (one or more, separated by spaces or lines):"));
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(d->inputs), l, FALSE, FALSE, 0);
    d->urls = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(d->urls), GTK_WRAP_WORD_CHAR);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), d->urls);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), d->urls);
    gtk_widget_set_size_request(sw, -1, 80);
    gtk_box_pack_start(GTK_BOX(d->inputs), sw, TRUE, TRUE, 0);

    GtkWidget *t = gtk_table_new(0, 2, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(t), 4);
    gtk_table_set_col_spacings(GTK_TABLE(t), 8);
    gtk_box_pack_start(GTK_BOX(d->inputs), t, FALSE, FALSE, 0);
    int r = 0;

    /* Destination: the folder and a button to choose it. */
    const char *home_dl = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    GtkWidget *dest_box = gtk_hbox_new(FALSE, 4);
    d->dest = text_entry(p, "dir", home_dl ? home_dl : g_get_home_dir());
    GtkWidget *browse = gtk_button_new_with_mnemonic(_("_Browse..."));
    g_signal_connect(browse, "clicked", G_CALLBACK(on_browse), d);
    gtk_box_pack_start(GTK_BOX(dest_box), d->dest, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(dest_box), browse, FALSE, FALSE, 0);
    row(t, &r, _("_Save in:"), dest_box);

    d->tmpl = gtk_combo_box_text_new_with_entry();
    for (guint i = 0; i < G_N_ELEMENTS(templates); i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->tmpl), templates[i]);
    gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(d->tmpl))),
                       kp_prefs_get_str(p, "download", "template", templates[0]));
    gtk_widget_set_tooltip_text(d->tmpl, _("yt-dlp's output template (-o). Folders in it are "
                                           "created inside the destination."));
    row(t, &r, _("File _name:"), d->tmpl);

    d->kind = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->kind), _("Video"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->kind), _("Audio only"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(d->kind),
                             kp_prefs_get_bool(p, "download", "audio_only", false) ? 1 : 0);
    row(t, &r, _("_Type:"), d->kind);

    d->height = gtk_combo_box_text_new();
    int want_h = kp_prefs_get_int(p, "download", "max_height", 0, 0, 100000), active_h = 0;
    for (guint i = 0; i < G_N_ELEMENTS(heights); i++) {
        char *s = heights[i] ? g_strdup_printf(_("Up to %dp"), heights[i])
                             : g_strdup(_("Best available"));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->height), s);
        g_free(s);
        if (heights[i] == want_h)
            active_h = (int)i;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(d->height), active_h);
    row(t, &r, _("_Quality:"), d->height);

    d->aformat = gtk_combo_box_text_new();
    const char *want_f = kp_prefs_get_str(p, "download", "audio_format", "m4a");
    int active_f = 0;
    for (guint i = 0; i < G_N_ELEMENTS(audio_formats); i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->aformat),
                                       !strcmp(audio_formats[i], "best") ? _("Keep the original")
                                                                          : audio_formats[i]);
        if (!strcmp(audio_formats[i], want_f))
            active_f = (int)i;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(d->aformat), active_f);
    row(t, &r, _("Audio _format:"), d->aformat);

    d->langs = text_entry(p, "sub_langs", "");
    if (!*gtk_entry_get_text(GTK_ENTRY(d->langs))) {
        char *def = default_langs(p);
        gtk_entry_set_text(GTK_ENTRY(d->langs), def);
        g_free(def);
    }
    gtk_widget_set_tooltip_text(d->langs, _("Language codes separated by commas (pt,en), or all. "
                                            "all includes the live chat of past streams."));
    d->items = gtk_entry_new();
    gtk_widget_set_tooltip_text(d->items, _("Only these items of a playlist: 1:3 (1 to 3) or "
                                            "1,2,5. Empty: all of them."));
    row(t, &r, _("Playlist _items:"), d->items);

    d->extra = text_entry(p, "extra", "");
    gtk_widget_set_tooltip_text(d->extra, _("More yt-dlp arguments, as typed in a shell, e.g. "
                                            "--limit-rate 1M"));
    row(t, &r, _("E_xtra arguments:"), d->extra);

    GtkWidget *opts = gtk_table_new(0, 2, TRUE);
    gtk_table_set_col_spacings(GTK_TABLE(opts), 8);
    d->remux = check(p, _("Convert the video to _mp4"), "remux_mp4", false);
    d->thumb = check(p, _("Save the _thumbnail"), "thumbnail", false);
    d->subs = check(p, _("Download _subtitles"), "subtitles", false);
    d->auto_subs = check(p, _("_Automatic captions too"), "auto_subs", false);
    d->restrict_names = check(p, _("Simple file names (no _special characters)"),
                              "restrict_names", false);
    d->no_playlist = check(p, _("_Only the video, if the link is in a playlist"), "no_playlist",
                           false);
    d->skip = gtk_check_button_new_with_mnemonic(_("Do not download the _video (only the rest)"));
    GtkWidget *cells[] = {d->subs, d->auto_subs, d->remux, d->thumb,
                          d->restrict_names, d->no_playlist, d->skip};
    for (guint i = 0; i < G_N_ELEMENTS(cells); i++)
        gtk_table_attach(GTK_TABLE(opts), cells[i], i % 2, i % 2 + 1, i / 2, i / 2 + 1,
                         GTK_FILL | GTK_EXPAND, GTK_FILL, 0, 0);
    gtk_box_pack_start(GTK_BOX(d->inputs), opts, FALSE, FALSE, 0);
    row(t, &r, _("Subtitle _languages:"), d->langs);

    d->bar = gtk_progress_bar_new();
    d->status = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(d->status), 0, 0.5);
    gtk_label_set_ellipsize(GTK_LABEL(d->status), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(box), d->bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), d->status, FALSE, FALSE, 0);

    /* The details: what yt-dlp prints, as it prints it. */
    GtkWidget *more = gtk_expander_new_with_mnemonic(_("Show _details"));
    d->log = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(d->log), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(d->log), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(d->log), GTK_WRAP_WORD_CHAR);
    PangoFontDescription *mono = pango_font_description_from_string("monospace 9");
    gtk_widget_modify_font(d->log, mono);
    pango_font_description_free(mono);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->log)), &end);
    gtk_text_buffer_create_mark(gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->log)), "end", &end,
                                FALSE);
    GtkWidget *logsw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(logsw), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(logsw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(logsw), d->log);
    gtk_widget_set_size_request(logsw, -1, 160);
    gtk_container_add(GTK_CONTAINER(more), logsw);
    gtk_box_pack_start(GTK_BOX(box), more, FALSE, FALSE, 0);

    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->urls));
    watch(d, d->kind, "changed");
    watch(d, d->aformat, "changed");
    watch(d, d->height, "changed");
    watch(d, d->tmpl, "changed");
    watch(d, d->dest, "changed");
    watch(d, d->langs, "changed");
    watch(d, d->items, "changed");
    watch(d, d->extra, "changed");
    GtkWidget *toggles[] = {d->remux, d->thumb, d->subs, d->auto_subs, d->restrict_names,
                            d->no_playlist, d->skip};
    for (guint i = 0; i < G_N_ELEMENTS(toggles); i++)
        watch(d, toggles[i], "toggled");
    g_signal_connect(buf, "changed", G_CALLBACK(on_changed), d);

    g_signal_connect(d->dlg, "response", G_CALLBACK(on_response), d);
    g_signal_connect(d->dlg, "destroy", G_CALLBACK(on_destroy), d);
    gtk_dialog_set_default_response(GTK_DIALOG(d->dlg), GTK_RESPONSE_ACCEPT);
    gtk_widget_show_all(d->dlg);
    refresh(d);
}

void kp_download_dialog(kp_app *app)
{
    if (current) {
        gtk_window_present(GTK_WINDOW(current->dlg));
        return;
    }
    char *err = NULL;
    char *program = kp_ytdlp_program(app->prefs, &err);
    if (!program) {
        GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                                              GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s",
                                              _("yt-dlp is not available."));
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(m), "%s", err);
        gtk_dialog_run(GTK_DIALOG(m));
        gtk_widget_destroy(m);
        g_free(err);
        return;
    }
    dl *d = g_new0(dl, 1);
    d->app = app;
    d->program = program;
    current = d;
    build(d);
}
