/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <string.h>

#include "app.h"
#include "i18n.h"
#include "log.h"

/* Options > Preferences: kiplay.ini in tabs, as SMPlayer does. Each
 * control is bound to one key; OK writes them all and then applies what
 * changed (the layout through the View menu's own items, the engines by
 * restarting them, MPRIS and the tray). Cancel leaves everything as it
 * was. Keys the dialog doesn't know are left alone, and a value it has no
 * option for (an mpv "--vo" typed by hand) shows as one more choice. */

typedef enum { B_CHECK, B_CHOICE, B_ENTRY, B_SPIN } bind_kind;

typedef struct {
    bind_kind kind;
    GtkWidget *w;
    const char *section, *key;
    GPtrArray *names; /* B_CHOICE: the value of each row (owned) */
    const char *def;  /* the default as written ("true", "video", "0"...) */
} binding;

typedef struct {
    kp_app *app;
    GArray *bindings; /* binding */
    GtkWidget *video_pos, *controls_pos, *show_video, *columns[KP_COLUMN_COUNT];
    GtkWidget *tray, *close_to_tray;
    GtkWidget *shortcuts;
} dialog;

/* ---- building ------------------------------------------------------------ */

static GtkWidget *page(GtkWidget *notebook, const char *title)
{
    GtkWidget *box = gtk_vbox_new(FALSE, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), box, gtk_label_new(title));
    return box;
}

/* A bold heading with an indented table under it, for label + control rows. */
static GtkWidget *section(GtkWidget *page_box, const char *title)
{
    GtkWidget *box = gtk_vbox_new(FALSE, 6);
    char *markup = g_markup_printf_escaped("<b>%s</b>", title);
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    GtkWidget *align = gtk_alignment_new(0, 0, 1, 1);
    gtk_alignment_set_padding(GTK_ALIGNMENT(align), 0, 0, 12, 0);
    GtkWidget *table = gtk_table_new(1, 2, FALSE); /* grows as rows are attached */
    gtk_table_set_row_spacings(GTK_TABLE(table), 6);
    gtk_table_set_col_spacings(GTK_TABLE(table), 12);
    gtk_container_add(GTK_CONTAINER(align), table);
    gtk_box_pack_start(GTK_BOX(box), align, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page_box), box, FALSE, FALSE, 0);
    return table;
}

/* The table's next free row (counted on the table). */
static guint next_row(GtkWidget *table)
{
    guint r = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(table), "kp-rows"));
    g_object_set_data(G_OBJECT(table), "kp-rows", GUINT_TO_POINTER(r + 1));
    return r;
}

/* A control spanning the row, or after its label; the label's mnemonic
 * focuses target (the control itself if NULL). */
static void row_for(GtkWidget *table, const char *label, GtkWidget *control, GtkWidget *target)
{
    guint r = next_row(table);
    if (!label) {
        gtk_table_attach(GTK_TABLE(table), control, 0, 2, r, r + 1, GTK_FILL | GTK_EXPAND, 0, 0, 0);
        return;
    }
    GtkWidget *l = gtk_label_new_with_mnemonic(label);
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), target ? target : control);
    gtk_table_attach(GTK_TABLE(table), l, 0, 1, r, r + 1, GTK_FILL, 0, 0, 0);
    gtk_table_attach(GTK_TABLE(table), control, 1, 2, r, r + 1, GTK_FILL | GTK_EXPAND, 0, 0, 0);
}

static void row(GtkWidget *table, const char *label, GtkWidget *control)
{
    row_for(table, label, control, NULL);
}

/* A grey, wrapped explanation under the rows so far. */
static void note(GtkWidget *table, const char *text)
{
    GtkWidget *l = gtk_label_new(NULL);
    char *markup = g_markup_printf_escaped("<small>%s</small>", text);
    gtk_label_set_markup(GTK_LABEL(l), markup);
    g_free(markup);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_widget_set_size_request(l, 420, -1);
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    gtk_widget_set_sensitive(l, FALSE);
    row(table, NULL, l);
}

static void bind(dialog *d, bind_kind kind, GtkWidget *w, const char *sec, const char *key,
                 GPtrArray *names, const char *def)
{
    binding b = {kind, w, sec, key, names, def ? def : ""};
    g_array_append_val(d->bindings, b);
}

static GtkWidget *check(dialog *d, GtkWidget *table, const char *label, const char *sec,
                        const char *key, bool def)
{
    GtkWidget *w = gtk_check_button_new_with_mnemonic(label);
    row(table, NULL, w);
    if (key) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w),
                                     kp_prefs_get_bool(d->app->prefs, sec, key, def));
        bind(d, B_CHECK, w, sec, key, NULL, def ? "true" : "false");
    }
    return w;
}

/* names[i] is stored for labels[i] (N_() strings); def is a name. A value
 * in the file that isn't among them becomes one more row. */
static GtkWidget *choice(dialog *d, GtkWidget *table, const char *label, const char *sec,
                         const char *key, const char *const names[], const char *const labels[],
                         const char *def)
{
    GtkWidget *w = gtk_combo_box_text_new();
    GPtrArray *values = g_ptr_array_new_with_free_func(g_free);
    const char *now = key ? kp_prefs_get_str(d->app->prefs, sec, key, def) : def;
    int active = -1;
    for (int i = 0; names[i]; i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w), _(labels[i]));
        g_ptr_array_add(values, g_strdup(names[i]));
        if (!g_ascii_strcasecmp(names[i], now ? now : ""))
            active = i;
    }
    if (active < 0 && now && *now) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w), now);
        g_ptr_array_add(values, g_strdup(now));
        active = (int)values->len - 1;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(w), active < 0 ? 0 : active);
    row(table, label, w);
    if (key)
        bind(d, B_CHOICE, w, sec, key, values, def);
    else
        g_ptr_array_free(values, TRUE);
    return w;
}

static GtkWidget *entry(dialog *d, GtkWidget *table, const char *label, const char *sec,
                        const char *key, const char *def)
{
    GtkWidget *w = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(w), kp_prefs_get_str(d->app->prefs, sec, key, def ? def : ""));
    gtk_entry_set_activates_default(GTK_ENTRY(w), TRUE);
    row(table, label, w);
    bind(d, B_ENTRY, w, sec, key, NULL, def);
    return w;
}

static GtkWidget *spin(dialog *d, GtkWidget *table, const char *label, const char *sec,
                       const char *key, int def, int min, int max)
{
    GtkWidget *w = gtk_spin_button_new_with_range(min, max, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w),
                              kp_prefs_get_int(d->app->prefs, sec, key, def, min, max));
    gtk_entry_set_activates_default(GTK_ENTRY(w), TRUE);
    GtkWidget *box = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), w, FALSE, FALSE, 0);
    row_for(table, label, box, w);
    char num[16];
    g_snprintf(num, sizeof num, "%d", def);
    bind(d, B_SPIN, w, sec, key, NULL, g_intern_string(num));
    return w;
}

/* ---- the tabs ------------------------------------------------------------ */

static void on_tray_toggled(GtkToggleButton *b, gpointer data)
{
    dialog *d = data;
    gtk_widget_set_sensitive(d->close_to_tray, gtk_toggle_button_get_active(b));
}

static void general_tab(dialog *d, GtkWidget *nb)
{
    GtkWidget *p = page(nb, _("General"));
    GtkWidget *t = section(p, _("Files"));
    static const char *const resume[] = {"off", "video", "all", NULL};
    static const char *const resume_labels[] = {N_("Never"), N_("Videos"),
                                                N_("Videos and music")};
    choice(d, t, _("_Resume where it stopped:"), "files", "resume", resume, resume_labels,
           "video");
    check(d, t, _("Remember the _tracks, delays and aspect ratio of each file"), "files",
          "remember", true);
    note(t, _("Kept in ~/.local/share/kiplay/files, one small file per media file."));

    t = section(p, _("Desktop"));
    check(d, t, _("Let the desktop and other programs control playback (_MPRIS)"), "general",
          "mpris", true);
    d->tray = check(d, t, _("Show an icon in the _system tray"), "general", "tray", false);
    d->close_to_tray = check(d, t, _("_Closing the window leaves Kiplay in the tray"), "general",
                             "close_to_tray", false);
    gtk_widget_set_sensitive(d->close_to_tray,
                             gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->tray)));
    g_signal_connect(d->tray, "toggled", G_CALLBACK(on_tray_toggled), d);
}

static void interface_tab(dialog *d, GtkWidget *nb)
{
    kp_app *app = d->app;
    GtkWidget *p = page(nb, _("Interface"));
    GtkWidget *t = section(p, _("Layout"));
    static const char *const video_labels[] = {N_("Above the playlists"), N_("On the left"),
                                               N_("On the right")};
    d->video_pos = choice(d, t, _("_Video:"), "layout", NULL, kp_video_pos_names, video_labels,
                          kp_video_pos_names[app->video_pos]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(d->video_pos), app->video_pos);
    static const char *const controls_labels[] = {N_("At the bottom"), N_("At the top")};
    d->controls_pos = choice(d, t, _("C_ontrols:"), "layout", NULL, kp_controls_pos_names,
                             controls_labels, kp_controls_pos_names[app->controls_pos]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(d->controls_pos), app->controls_pos);
    choice(d, t, _("Controls in _fullscreen:"), "layout", "fullscreen_controls",
           kp_controls_pos_names, controls_labels, kp_controls_pos_names[KP_CONTROLS_BOTTOM]);
    d->show_video = check(d, t, _("_Always show the video area"), "layout", NULL, false);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->show_video), app->video_forced);
    note(t, _("Without it, the video area shows up only while a video plays."));

    t = section(p, _("Playlist columns"));
    static const char *const labels[] = {N_("_Number"), N_("_Artist"), N_("Al_bum"),
                                         N_("_Length"), N_("_Path")};
    for (int i = 0; i < KP_COLUMN_COUNT; i++) {
        d->columns[i] = check(d, t, _(labels[i]), "playlists", NULL, false);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->columns[i]), app->columns & (1u << i));
    }
}

static void video_tab(dialog *d, GtkWidget *nb)
{
    GtkWidget *p = page(nb, _("Video"));
    GtkWidget *t = section(p, _("Decoding and output (mpv)"));
    static const char *const hwdec[] = {"", "auto-safe", "vaapi", "vdpau", NULL};
    static const char *const hwdec_labels[] = {N_("Off (mpv's default)"), N_("Automatic, safe"),
                                               N_("VA-API"), N_("VDPAU")};
    choice(d, t, _("_Hardware decoding:"), "video", "hwdec", hwdec, hwdec_labels, "");
    static const char *const vo[] = {"", "gpu", "xv", "x11", NULL};
    static const char *const vo_labels[] = {N_("mpv's default"), N_("gpu (OpenGL)"),
                                            N_("xv (old graphics cards)"),
                                            N_("x11 (software, slowest)")};
    choice(d, t, _("Video _output:"), "video", "vo", vo, vo_labels, "");
    note(t, _("Changes here restart mpv; the video goes on where it was."));
}

static void audio_tab(dialog *d, GtkWidget *nb)
{
    GtkWidget *p = page(nb, _("Audio"));
    GtkWidget *t = section(p, _("Music"));
    static const char *const engines[] = {"auto", "mpv", NULL};
    static const char *const engine_labels[] = {N_("kiplay-audio (light, gapless)"),
                                                N_("mpv, like the videos")};
    choice(d, t, _("Play music _with:"), "playback", "engine", engines, engine_labels, "auto");
    GtkWidget *dev = entry(d, t, _("Output _device:"), "playback", "device", "default");
    gtk_widget_set_tooltip_text(dev, _("For kiplay-audio: default (PulseAudio or PipeWire; ALSA "
                                       "when there is no sound server) or an ALSA device: hw:1, "
                                       "plughw:0,0..."));

    t = section(p, _("Videos (mpv)"));
    static const char *const ao[] = {"", "pulse", "pipewire", "alsa", "jack", NULL};
    static const char *const ao_labels[] = {N_("mpv's default"), N_("PulseAudio"),
                                            N_("PipeWire"), N_("ALSA"), N_("JACK")};
    choice(d, t, _("Audio _output:"), "audio", "ao", ao, ao_labels, "");
    GtkWidget *lang = entry(d, t, _("Preferred _languages:"), "audio", "languages", "");
    gtk_widget_set_tooltip_text(lang, _("Language codes in order, e.g. ja,en"));
}

static void subtitles_tab(dialog *d, GtkWidget *nb)
{
    GtkWidget *p = page(nb, _("Subtitles"));
    GtkWidget *t = section(p, _("Subtitles (mpv)"));
    GtkWidget *lang = entry(d, t, _("Preferred _languages:"), "subtitles", "languages", "");
    gtk_widget_set_tooltip_text(lang, _("Language codes in order, e.g. pt-BR,pt,en"));
    static const char *const autos[] = {"exact", "fuzzy", "no", NULL};
    static const char *const auto_labels[] = {N_("With the video's exact name"),
                                              N_("With names containing the video's"),
                                              N_("None")};
    choice(d, t, _("Load subtitle _files:"), "subtitles", "auto", autos, auto_labels, "exact");
    GtkWidget *font = entry(d, t, _("_Font:"), "subtitles", "font", "");
    gtk_widget_set_tooltip_text(font, _("A font family, e.g. DejaVu Sans; empty: mpv's default"));
    spin(d, t, _("Font _size:"), "subtitles", "font_size", 0, 0, 200);
    note(t, _("Size 0 is mpv's default (55, scaled to a 720-line screen). Styled subtitles (ASS) "
              "keep their own fonts."));
    GtkWidget *yt = section(p, _("YouTube captions (srv3, json3)"));
    static const char *const edges[] = {"youtube", "strong", NULL};
    static const char *const edge_labels[] = {N_("Thin, as on YouTube"),
                                              N_("Thick and blurred (easier to read)")};
    choice(d, yt, _("Text _outline:"), "subtitles", "youtube_edge", edges, edge_labels, "youtube");
    note(yt, _("Captions saved with their style are drawn as they look on YouTube, wherever "
               "the video's file name matches. Takes effect on the next file."));
}

static void set_cache_label(GtkWidget *label)
{
    char *size = g_format_size(kp_img_cache_size());
    char *text = g_strdup_printf(_("Picture cache: %s"), size);
    gtk_label_set_text(GTK_LABEL(label), text);
    g_free(text);
    g_free(size);
}

static void on_clear_cache(GtkButton *b, gpointer label)
{
    (void)b;
    kp_img_cache_clear();
    set_cache_label(label);
}

static void chat_tab(dialog *d, GtkWidget *nb)
{
    GtkWidget *p = page(nb, _("Chat"));
    GtkWidget *t = section(p, _("Recorded live chat"));
    static const char *const filters[] = {"all", "paid", "members", NULL};
    static const char *const filter_labels[] = {N_("All messages"),
                                                N_("Super Chats and new members only"),
                                                N_("Super Chats, members, moderators and the "
                                                   "channel")};
    check(d, t, _("Show the chat beside the video"), "chat", "show", true);
    spin(d, t, _("Panel _width:"), "chat", "width", 320, 160, 1000);
    choice(d, t, _("_Show:"), "chat", "filter", filters, filter_labels, "all");
    note(t, _("The chat comes from the file next to the video (Title [ID].live_chat.json, "
              "saved by yt-dlp with --sub-langs live_chat). Channel emoji and avatars are "
              "downloaded once with curl and kept in ~/.cache/kiplay/chat."));

    t = section(p, _("Pictures"));
    GtkWidget *size = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(size), 0, 0.5);
    set_cache_label(size);
    row(t, NULL, size);
    GtkWidget *clear = gtk_button_new_with_mnemonic(_("C_lear the picture cache"));
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_cache), size);
    GtkWidget *box = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), clear, FALSE, FALSE, 0);
    row(t, NULL, box);
}

static void shortcuts_tab(dialog *d, GtkWidget *nb)
{
    d->shortcuts = kp_shortcuts_editor_new(d->app);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), d->shortcuts, gtk_label_new(_("Shortcuts")));
}

static void advanced_tab(dialog *d, GtkWidget *nb)
{
    GtkWidget *p = page(nb, _("Advanced"));
    GtkWidget *t = section(p, _("mpv"));
    entry(d, t, _("_Program:"), "mpv", "program", "mpv");
    check(d, t, _("Also read my mpv _configuration (~/.config/mpv/mpv.conf)"), "mpv",
          "use_config", false);
    GtkWidget *extra = entry(d, t, _("_Extra options:"), "mpv", "extra", "");
    gtk_widget_set_tooltip_text(extra,
                                _("Passed to mpv as typed, e.g. --deband --scale=ewa_lanczos"));
    note(t, _("Kiplay controls mpv's window, keys and on-screen controls; options that change "
              "those can break it."));
}

/* ---- applying ------------------------------------------------------------ */

/* What a control holds, as kiplay.ini writes it. g_free it. */
static char *value_of(const binding *b)
{
    switch (b->kind) {
    case B_CHECK:
        return g_strdup(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(b->w)) ? "true" : "false");
    case B_CHOICE: {
        int a = gtk_combo_box_get_active(GTK_COMBO_BOX(b->w));
        return g_strdup(a >= 0 && (guint)a < b->names->len ? b->names->pdata[a] : "");
    }
    case B_ENTRY:
        return g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(b->w))));
    case B_SPIN:
        return g_strdup_printf("%d", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(b->w)));
    }
    return g_strdup("");
}

/* A value equal to the default goes away from the file, so a default that
 * changes in a later version reaches whoever never chose otherwise. */
static void write_bindings(dialog *d)
{
    kp_prefs *prefs = d->app->prefs;
    for (guint i = 0; i < d->bindings->len; i++) {
        const binding *b = &g_array_index(d->bindings, binding, i);
        char *v = value_of(b);
        if (!g_ascii_strcasecmp(v, b->def))
            kp_prefs_unset(prefs, b->section, b->key);
        else
            kp_prefs_set_str(prefs, b->section, b->key, v);
        g_free(v);
    }
}

/* The layout through the View menu's items, whose handlers do the rest. */
static void apply_layout(dialog *d)
{
    kp_app *app = d->app;
    int v = gtk_combo_box_get_active(GTK_COMBO_BOX(d->video_pos));
    if (v >= 0 && v < KP_VIDEO_POS_COUNT)
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app->video_pos_items[v]), TRUE);
    int c = gtk_combo_box_get_active(GTK_COMBO_BOX(d->controls_pos));
    if (c >= 0 && c < KP_CONTROLS_POS_COUNT)
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app->controls_pos_items[c]), TRUE);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app->show_video_item),
                                   gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->show_video)));
    for (int i = 0; i < KP_COLUMN_COUNT; i++)
        gtk_check_menu_item_set_active(
            GTK_CHECK_MENU_ITEM(app->column_items[i]),
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->columns[i])));
}

static void apply(dialog *d)
{
    kp_app *app = d->app;
    write_bindings(d);
    apply_layout(d);
    kp_shortcuts_editor_apply(app, d->shortcuts);
    kp_player_prefs_changed(app);
    kp_desktop_prefs_changed(app);
    kp_chat_prefs_changed(app);
    if (kp_prefs_save(app->prefs) < 0)
        kp_log_warn(_("could not save the settings: %s"), g_strerror(errno));
}

void kp_prefs_dialog(kp_app *app)
{
    static bool open;
    if (open)
        return; /* the key again while it shows */
    open = true;
    dialog d = {.app = app, .bindings = g_array_new(FALSE, TRUE, sizeof(binding))};
    GtkWidget *dlg = gtk_dialog_new_with_buttons(_("Preferences"), GTK_WINDOW(app->window),
                                                 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                 GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                                 GTK_STOCK_OK, GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 560, 480);
    GtkWidget *nb = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(nb), 6);
    general_tab(&d, nb);
    interface_tab(&d, nb);
    video_tab(&d, nb);
    audio_tab(&d, nb);
    subtitles_tab(&d, nb);
    chat_tab(&d, nb);
    shortcuts_tab(&d, nb);
    advanced_tab(&d, nb);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), nb, TRUE, TRUE, 0);
    gtk_widget_show_all(nb);
    gtk_notebook_set_current_page(
        GTK_NOTEBOOK(nb), kp_prefs_get_int(app->prefs, "window", "prefs_tab", 0, 0, 20));

    int response = gtk_dialog_run(GTK_DIALOG(dlg));
    kp_prefs_set_int(app->prefs, "window", "prefs_tab",
                     gtk_notebook_get_current_page(GTK_NOTEBOOK(nb)));
    if (response == GTK_RESPONSE_OK)
        apply(&d);
    gtk_widget_destroy(dlg);
    for (guint i = 0; i < d.bindings->len; i++)
        if (g_array_index(d.bindings, binding, i).names)
            g_ptr_array_free(g_array_index(d.bindings, binding, i).names, TRUE);
    g_array_free(d.bindings, TRUE);
    open = false;
}
