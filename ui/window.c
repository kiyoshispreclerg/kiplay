/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "window.h"

#include <errno.h>
#include <gdk/gdkkeysyms.h>
#include <glib-unix.h>
#include <signal.h>
#include <string.h>

#include "app.h"
#include "instance.h"
#include "i18n.h"
#include "log.h"
#include "util.h"

typedef struct {
    kp_app app;
    kp_instance *instance;
    /* Last unmaximized size, for the prefs. */
    int width, height;
    bool maximized;
    const kp_window_opts *opts;
    gulong first_expose;
} kp_window;

static GtkWidget *item(GtkWidget *menu, const char *label, GCallback cb, gpointer data)
{
    GtkWidget *it = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect_swapped(it, "activate", cb, data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    return it;
}

/* An item that runs a keyboard command (shortcuts.c), showing its key. */
static GtkWidget *command(GtkWidget *menu, const char *label, GCallback cb, kp_app *app,
                          const char *action)
{
    GtkWidget *it = item(menu, label, cb, app);
    kp_shortcuts_attach(it, action);
    return it;
}

static GtkWidget *submenu(GtkWidget *bar, const char *label)
{
    GtkWidget *top = gtk_menu_item_new_with_mnemonic(label);
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), top);
    return menu;
}

static void separator(GtkWidget *menu)
{
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

/* ---- View menu ----------------------------------------------------------- */

static void on_show_video(GtkCheckMenuItem *it, gpointer data)
{
    kp_app *app = data;
    kp_layout_save(app);
    app->video_forced = gtk_check_menu_item_get_active(it);
    kp_prefs_set_bool(app->prefs, "layout", "show_video", app->video_forced);
    kp_layout_update_video(app);
}

static void on_video_pos(GtkCheckMenuItem *it, gpointer data)
{
    kp_app *app = data;
    if (!gtk_check_menu_item_get_active(it))
        return;
    kp_layout_save(app); /* the divider of the layout we leave */
    app->video_pos = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(it), "kp-value"));
    kp_prefs_set_str(app->prefs, "layout", "video", kp_video_pos_names[app->video_pos]);
    kp_layout_apply(app);
}

static void on_controls_pos(GtkCheckMenuItem *it, gpointer data)
{
    kp_app *app = data;
    if (!gtk_check_menu_item_get_active(it))
        return;
    kp_layout_save(app);
    app->controls_pos = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(it), "kp-value"));
    kp_prefs_set_str(app->prefs, "layout", "controls", kp_controls_pos_names[app->controls_pos]);
    kp_layout_apply(app);
}

/* A group of radio items; the one equal to current starts active. */
static void radio_group(GtkWidget *menu, kp_app *app, const char *const labels[], int n,
                        int current, GCallback cb, GtkWidget **items)
{
    GSList *group = NULL;
    for (int i = 0; i < n; i++) {
        GtkWidget *it = gtk_radio_menu_item_new_with_mnemonic(group, _(labels[i]));
        items[i] = it;
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(it));
        g_object_set_data(G_OBJECT(it), "kp-value", GINT_TO_POINTER(i));
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(it), i == current);
        g_signal_connect(it, "toggled", cb, app);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    }
}

static void on_column(GtkCheckMenuItem *it, gpointer data)
{
    kp_app *app = data;
    unsigned bit = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(it), "kp-value"));
    kp_playlists_set_columns(app, gtk_check_menu_item_get_active(it) ? app->columns | bit
                                                                     : app->columns & ~bit);
}

static void columns_menu(GtkWidget *menu, kp_app *app)
{
    static const char *const labels[] = {N_("_Number"), N_("_Artist"), N_("Al_bum"),
                                         N_("_Length"), N_("_Path")};
    for (int i = 0; i < KP_COLUMN_COUNT; i++) {
        GtkWidget *it = gtk_check_menu_item_new_with_mnemonic(_(labels[i]));
        app->column_items[i] = it;
        g_object_set_data(G_OBJECT(it), "kp-value", GUINT_TO_POINTER(1u << i));
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(it), app->columns & (1u << i));
        g_signal_connect(it, "toggled", G_CALLBACK(on_column), app);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    }
}

/* ---- the rest ------------------------------------------------------------ */

static void on_stop_after(GtkCheckMenuItem *it, gpointer data)
{
    kp_player_set_stop_after(data, gtk_check_menu_item_get_active(it));
}

static void on_compact(GtkCheckMenuItem *it, gpointer data)
{
    kp_layout_set_compact(data, gtk_check_menu_item_get_active(it));
}

static void toggle_fullscreen(kp_app *app)
{
    kp_layout_set_fullscreen(app, !app->fullscreen);
}

static void toggle_compact(kp_app *app)
{
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app->compact_item), !app->compact);
}

static void on_about(kp_app *app)
{
    const char *authors[] = {"Kiyoshi Spreclerg", NULL};
    gtk_show_about_dialog(GTK_WINDOW(app->window), "program-name", "Kiplay", "version",
                          KP_VERSION, "comments",
                          _("Light audio and video player for KiDesktop, with YouTube's styled "
                            "captions and recorded live chat."),
                          "license", "GPL-3.0-or-later", "authors", authors, "logo-icon-name",
                          "kiplay", NULL);
}

/* Everything is saved before the window goes: once destroy starts, the
 * notebook and the paned have no pages or position left to read. */
static void close_window(kp_window *w)
{
    kp_app *app = &w->app;
    /* The divider and the size to keep are the windowed ones. */
    kp_layout_set_fullscreen(app, false);
    kp_layout_set_compact(app, false);
    kp_desktop_free(app);
    kp_player_free(app);
    kp_tracks_free();
    kp_playlists_save(app);
    kp_playlists_free(app);
    kp_layout_save(app);
    kp_prefs_set_int(app->prefs, "window", "width", w->width);
    kp_prefs_set_int(app->prefs, "window", "height", w->height);
    kp_prefs_set_bool(app->prefs, "window", "maximized", w->maximized);
    if (kp_prefs_save(app->prefs) < 0)
        kp_log_warn(_("could not save the settings: %s"), g_strerror(errno));
    gtk_widget_destroy(app->window);
}

void kp_window_quit(kp_app *app)
{
    /* app is the first member of kp_window. */
    close_window((kp_window *)app);
}

static GtkWidget *menu_bar(kp_app *app)
{
    GtkWidget *bar = gtk_menu_bar_new();

    GtkWidget *m = submenu(bar, _("_File"));
    command(m, _("_Add files..."), G_CALLBACK(kp_playlists_add_files), app, "add_files");
    command(m, _("Add _folder..."), G_CALLBACK(kp_playlists_add_folder), app, "add_folder");
    separator(m);
    item(m, _("_Import playlist..."), G_CALLBACK(kp_playlists_import), app);
    item(m, _("_Export playlist..."), G_CALLBACK(kp_playlists_export), app);
    separator(m);
    command(m, _("_Quit"), G_CALLBACK(kp_window_quit), app, "quit");

    m = submenu(bar, _("_Playlist"));
    command(m, _("_New"), G_CALLBACK(kp_playlists_new), app, "new_playlist");
    command(m, _("_Rename..."), G_CALLBACK(kp_playlists_rename), app, "rename_playlist");
    command(m, _("_Close"), G_CALLBACK(kp_playlists_close), app, "close_playlist");
    separator(m);
    command(m, _("_Jump to file..."), G_CALLBACK(kp_jump_dialog), app, "jump");
    command(m, _("_Queue / unqueue"), G_CALLBACK(kp_playlists_toggle_queue), app, "queue");
    command(m, _("Remove _selected"), G_CALLBACK(kp_playlists_remove_selected), app, "remove");

    m = submenu(bar, _("Play_back"));
    command(m, _("_Play / Pause"), G_CALLBACK(kp_player_toggle_pause), app, "play_pause");
    command(m, _("_Stop"), G_CALLBACK(kp_player_stop), app, "stop");
    command(m, _("P_revious"), G_CALLBACK(kp_player_prev), app, "previous");
    command(m, _("_Next"), G_CALLBACK(kp_player_next), app, "next");
    app->stop_after_item = gtk_check_menu_item_new_with_mnemonic(_("Stop _after this track"));
    g_signal_connect(app->stop_after_item, "toggled", G_CALLBACK(on_stop_after), app);
    kp_shortcuts_attach(app->stop_after_item, "stop_after");
    gtk_menu_shell_append(GTK_MENU_SHELL(m), app->stop_after_item);

    kp_tracks_menus(app, bar);

    m = submenu(bar, _("_View"));
    app->show_video_item = gtk_check_menu_item_new_with_mnemonic(_("_Video area"));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app->show_video_item), app->video_forced);
    g_signal_connect(app->show_video_item, "toggled", G_CALLBACK(on_show_video), app);
    kp_shortcuts_attach(app->show_video_item, "video_area");
    gtk_menu_shell_append(GTK_MENU_SHELL(m), app->show_video_item);
    app->compact_item = gtk_check_menu_item_new_with_mnemonic(_("C_ompact mode"));
    g_signal_connect(app->compact_item, "toggled", G_CALLBACK(on_compact), app);
    kp_shortcuts_attach(app->compact_item, "compact");
    gtk_menu_shell_append(GTK_MENU_SHELL(m), app->compact_item);
    separator(m);
    static const char *const video_labels[] = {N_("Video _above the playlists"),
                                               N_("Video on the _left"),
                                               N_("Video on the _right")};
    radio_group(m, app, video_labels, KP_VIDEO_POS_COUNT, app->video_pos,
                G_CALLBACK(on_video_pos), app->video_pos_items);
    separator(m);
    static const char *const controls_labels[] = {N_("Controls at the _bottom"),
                                                  N_("Controls at the _top")};
    radio_group(m, app, controls_labels, KP_CONTROLS_POS_COUNT, app->controls_pos,
                G_CALLBACK(on_controls_pos), app->controls_pos_items);
    separator(m);
    GtkWidget *cols = gtk_menu_item_new_with_mnemonic(_("Playlist _columns"));
    GtkWidget *cols_menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(cols), cols_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), cols);
    columns_menu(cols_menu, app);

    m = submenu(bar, _("_Options"));
    command(m, _("_Preferences..."), G_CALLBACK(kp_prefs_dialog), app, "preferences");

    m = submenu(bar, _("_Help"));
    item(m, _("_About"), G_CALLBACK(on_about), app);
    return bar;
}

/* Right click on the video or the controls: what is needed without the
 * menu bar (compact mode, fullscreen). */
static void context_menu(kp_app *app, GdkEventButton *ev)
{
    GtkWidget *m = gtk_menu_new();
    command(m, _("_Play / Pause"), G_CALLBACK(kp_player_toggle_pause), app, "play_pause");
    command(m, _("_Stop"), G_CALLBACK(kp_player_stop), app, "stop");
    command(m, _("P_revious"), G_CALLBACK(kp_player_prev), app, "previous");
    command(m, _("_Next"), G_CALLBACK(kp_player_next), app, "next");
    separator(m);
    command(m, app->fullscreen ? _("Leave _fullscreen") : _("_Fullscreen"),
            G_CALLBACK(toggle_fullscreen), app, "fullscreen");
    command(m, app->compact ? _("Leave c_ompact mode") : _("C_ompact mode"),
            G_CALLBACK(toggle_compact), app, "compact");
    separator(m);
    command(m, _("_Preferences..."), G_CALLBACK(kp_prefs_dialog), app, "preferences");
    command(m, _("_Quit"), G_CALLBACK(kp_window_quit), app, "quit");
    gtk_widget_show_all(m);
    g_signal_connect(m, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup(GTK_MENU(m), NULL, NULL, NULL, NULL, ev ? ev->button : 0,
                   ev ? ev->time : gtk_get_current_event_time());
}

static gboolean on_controls_click(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
        return FALSE;
    context_menu(data, ev);
    return TRUE;
}

/* The commands (shortcuts.c), Alt+1..9 for the tabs, and the media keys
 * while the window has the focus. */
static gboolean on_key(GtkWidget *widget, GdkEventKey *ev, gpointer data)
{
    (void)widget;
    kp_app *app = data;
    GdkModifierType mods = ev->state & gtk_accelerator_get_default_mod_mask();
    if (mods == GDK_MOD1_MASK && ev->keyval >= GDK_1 && ev->keyval <= GDK_9) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), (int)(ev->keyval - GDK_1));
        return TRUE;
    }
    switch (ev->keyval) {
    case GDK_AudioPlay:
    case GDK_AudioPause:
        kp_player_toggle_pause(app);
        return TRUE;
    case GDK_AudioStop:
        kp_player_stop(app);
        return TRUE;
    case GDK_AudioNext:
        kp_player_next(app);
        return TRUE;
    case GDK_AudioPrev:
        kp_player_prev(app);
        return TRUE;
    case GDK_AudioMute:
        kp_player_toggle_mute(app);
        return TRUE;
    default:
        break;
    }
    if (kp_shortcuts_key(app, ev))
        return TRUE;
    if (mods != 0)
        return FALSE;
    switch (ev->keyval) {
    case GDK_Escape:
        if (app->fullscreen) {
            kp_layout_set_fullscreen(app, false);
            return TRUE;
        }
        return FALSE;
    case GDK_Up:
    case GDK_Down:
        /* In fullscreen there is no list to move in: volume. */
        if (app->fullscreen) {
            kp_controls_change_volume(app, ev->keyval == GDK_Up ? 5 : -5);
            return TRUE;
        }
        return FALSE;
    default:
        return FALSE;
    }
}

static gboolean on_video_click(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w;
    kp_app *app = data;
    if (ev->type == GDK_2BUTTON_PRESS && ev->button == 1)
        toggle_fullscreen(app);
    else if (ev->type == GDK_BUTTON_PRESS && ev->button == 2)
        kp_player_toggle_pause(app);
    else if (ev->type == GDK_BUTTON_PRESS && ev->button == 3)
        context_menu(app, ev);
    return TRUE;
}

static gboolean on_video_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer data)
{
    (void)w;
    kp_controls_change_volume(data, ev->direction == GDK_SCROLL_UP ? 5 : -5);
    return TRUE;
}

static gboolean on_configure(GtkWidget *widget, GdkEventConfigure *ev, gpointer data)
{
    (void)widget;
    kp_window *w = data;
    /* The size to keep is the normal one (compact mode shrinks it). */
    if (!w->maximized && !w->app.fullscreen && !w->app.compact) {
        w->width = ev->width;
        w->height = ev->height;
    }
    return FALSE;
}

static gboolean on_state(GtkWidget *widget, GdkEventWindowState *ev, gpointer data)
{
    (void)widget;
    kp_window *w = data;
    w->maximized = ev->new_window_state & GDK_WINDOW_STATE_MAXIMIZED;
    return FALSE;
}

static gboolean on_delete(GtkWidget *widget, GdkEvent *ev, gpointer data)
{
    (void)widget;
    (void)ev;
    kp_window *w = data;
    if (!kp_desktop_close_to_tray(&w->app))
        close_window(w);
    return TRUE;
}

static void on_destroy(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    gtk_main_quit();
}

static gboolean on_signal(gpointer data)
{
    kp_window *w = data;
    kp_log_debug("signal: closing");
    close_window(w);
    /* Removed by kp_window_run once the main loop ends. */
    return G_SOURCE_CONTINUE;
}

static gboolean quit_idle(gpointer data)
{
    close_window(data);
    return G_SOURCE_REMOVE;
}

static gboolean on_first_expose(GtkWidget *widget, GdkEventExpose *ev, gpointer data)
{
    (void)ev;
    kp_window *w = data;
    g_signal_handler_disconnect(widget, w->first_expose);
    kp_log_debug("%.1f ms: first draw",
                 (double)(g_get_monotonic_time() - w->opts->start_us) / 1000.0);
    if (w->opts->exit_after_map)
        g_idle_add(quit_idle, w);
    return FALSE;
}

static void on_instance_files(void *ud, char **locations, bool enqueue)
{
    kp_desktop_present(ud);
    kp_player_open(ud, locations, enqueue);
}

int kp_window_run(const kp_window_opts *opts)
{
    kp_window w = {.opts = opts};
    kp_app *app = &w.app;
    app->prefs = kp_prefs_load(opts->prefs_path);
    if (!app->prefs) {
        kp_log_error(_("out of memory"));
        return 1;
    }
    char dir[KP_PATH_MAX];
    if (!kp_config_path(dir, sizeof dir, "playlists")) {
        kp_log_error(_("cannot find the settings folder (is $HOME set?)"));
        kp_prefs_free(app->prefs);
        return 1;
    }
    app->playlists =
        kp_plset_load(dir, kp_prefs_get_str(app->prefs, "playlists", "order", NULL));
    kp_log_debug("%.1f ms: %zu playlists", (double)(g_get_monotonic_time() - opts->start_us) / 1000.0,
                 kp_plset_count(app->playlists));

    app->video_pos = kp_prefs_get_choice(app->prefs, "layout", "video", kp_video_pos_names,
                                         KP_VIDEO_ABOVE);
    app->controls_pos = kp_prefs_get_choice(app->prefs, "layout", "controls",
                                            kp_controls_pos_names, KP_CONTROLS_BOTTOM);
    app->video_forced = kp_prefs_get_bool(app->prefs, "layout", "show_video", false);
    app->show_video = app->video_forced;

    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "Kiplay");
    gtk_window_set_icon_name(GTK_WINDOW(app->window), "kiplay");
    w.width = kp_prefs_get_int(app->prefs, "window", "width", 760, 300, 20000);
    w.height = kp_prefs_get_int(app->prefs, "window", "height", 520, 200, 20000);
    gtk_window_set_default_size(GTK_WINDOW(app->window), w.width, w.height);
    if (kp_prefs_get_bool(app->prefs, "window", "maximized", false))
        gtk_window_maximize(GTK_WINDOW(app->window));

    app->body = gtk_vbox_new(FALSE, 0);
    gtk_container_add(GTK_CONTAINER(app->window), app->body);
    /* Before the menus: they show the columns it reads, and the keys. */
    kp_playlists_init(app);
    kp_shortcuts_load(app);
    app->menubar = menu_bar(app);
    gtk_box_pack_start(GTK_BOX(app->body), app->menubar, FALSE, FALSE, 0);

    /* The video area: black, and later the backend's window (--wid). */
    app->video = gtk_drawing_area_new();
    GdkColor black = {0, 0, 0, 0};
    gtk_widget_modify_bg(app->video, GTK_STATE_NORMAL, &black);
    gtk_widget_set_size_request(app->video, 160, 90);
    g_object_ref_sink(app->video);
    gtk_widget_add_events(app->video, GDK_BUTTON_PRESS_MASK | GDK_SCROLL_MASK);
    g_signal_connect(app->video, "button-press-event", G_CALLBACK(on_video_click), app);
    g_signal_connect(app->video, "scroll-event", G_CALLBACK(on_video_scroll), app);

    app->controls = kp_controls_new(app);
    g_signal_connect(app->controls, "button-press-event", G_CALLBACK(on_controls_click), app);
    app->status = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(app->status), 0.0, 0.5);
    gtk_misc_set_padding(GTK_MISC(app->status), 6, 2);
    gtk_box_pack_end(GTK_BOX(app->body), app->status, FALSE, FALSE, 0);

    gtk_widget_show_all(app->body);
    kp_layout_apply(app);
    kp_playlists_update_status(app);
    kp_player_init(app);
    kp_desktop_init(app);
    if (opts->instance_socket) {
        char *err = NULL;
        w.instance = kp_instance_listen(opts->instance_socket, on_instance_files, app, &err);
        if (!w.instance) {
            kp_log_debug("instance: %s", err);
            g_free(err);
        }
    }
    if (opts->files && opts->files[0])
        kp_player_open(app, opts->files, opts->enqueue);

    g_signal_connect(app->window, "key-press-event", G_CALLBACK(on_key), app);
    g_signal_connect(app->window, "configure-event", G_CALLBACK(on_configure), &w);
    g_signal_connect(app->window, "window-state-event", G_CALLBACK(on_state), &w);
    g_signal_connect(app->window, "delete-event", G_CALLBACK(on_delete), &w);
    g_signal_connect(app->window, "destroy", G_CALLBACK(on_destroy), &w);
    w.first_expose = g_signal_connect(app->window, "expose-event", G_CALLBACK(on_first_expose), &w);
    guint sig_int = g_unix_signal_add(SIGINT, on_signal, &w);
    guint sig_term = g_unix_signal_add(SIGTERM, on_signal, &w);

    gtk_widget_show(app->window);
    gtk_main();

    kp_instance_free(w.instance);
    g_source_remove(sig_int);
    g_source_remove(sig_term);
    g_object_unref(app->video);
    g_object_unref(app->notebook);
    g_object_unref(app->controls);
    g_array_free(app->queue, TRUE);
    kp_plset_free(app->playlists);
    kp_prefs_free(app->prefs);
    return 0;
}
