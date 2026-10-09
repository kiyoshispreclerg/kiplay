/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "window.h"

#include <errno.h>
#include <gdk/gdkkeysyms.h>
#include <glib-unix.h>
#include <signal.h>
#include <string.h>

#include "app.h"
#include "i18n.h"
#include "log.h"
#include "util.h"

typedef struct {
    kp_app app;
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

static void accel(GtkWidget *it, kp_app *app, guint key, GdkModifierType mods)
{
    gtk_widget_add_accelerator(it, "activate", app->accel, key, mods, GTK_ACCEL_VISIBLE);
}

static GtkWidget *submenu(GtkWidget *bar, const char *label, kp_app *app)
{
    GtkWidget *top = gtk_menu_item_new_with_mnemonic(label);
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_set_accel_group(GTK_MENU(menu), app->accel);
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
    app->show_video = gtk_check_menu_item_get_active(it);
    kp_prefs_set_bool(app->prefs, "layout", "show_video", app->show_video);
    kp_layout_apply(app);
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
                        int current, GCallback cb)
{
    GSList *group = NULL;
    for (int i = 0; i < n; i++) {
        GtkWidget *it = gtk_radio_menu_item_new_with_mnemonic(group, _(labels[i]));
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(it));
        g_object_set_data(G_OBJECT(it), "kp-value", GINT_TO_POINTER(i));
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(it), i == current);
        g_signal_connect(it, "toggled", cb, app);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    }
}

/* ---- the rest ------------------------------------------------------------ */

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
    kp_playlists_save(app);
    kp_layout_save(app);
    kp_prefs_set_int(app->prefs, "window", "width", w->width);
    kp_prefs_set_int(app->prefs, "window", "height", w->height);
    kp_prefs_set_bool(app->prefs, "window", "maximized", w->maximized);
    if (kp_prefs_save(app->prefs) < 0)
        kp_log_warn(_("could not save the settings: %s"), g_strerror(errno));
    gtk_widget_destroy(app->window);
}

static void on_quit(kp_app *app)
{
    /* app is the first member of kp_window. */
    close_window((kp_window *)app);
}

static GtkWidget *menu_bar(kp_app *app)
{
    GtkWidget *bar = gtk_menu_bar_new();

    GtkWidget *m = submenu(bar, _("_File"), app);
    accel(item(m, _("_Add files..."), G_CALLBACK(kp_playlists_add_files), app), app, GDK_o,
          GDK_CONTROL_MASK);
    separator(m);
    accel(item(m, _("_Quit"), G_CALLBACK(on_quit), app), app, GDK_q, GDK_CONTROL_MASK);

    m = submenu(bar, _("_Playlist"), app);
    accel(item(m, _("_New"), G_CALLBACK(kp_playlists_new), app), app, GDK_t, GDK_CONTROL_MASK);
    accel(item(m, _("_Rename..."), G_CALLBACK(kp_playlists_rename), app), app, GDK_F2, 0);
    accel(item(m, _("_Close"), G_CALLBACK(kp_playlists_close), app), app, GDK_w,
          GDK_CONTROL_MASK);
    separator(m);
    item(m, _("Remove _selected"), G_CALLBACK(kp_playlists_remove_selected), app);

    m = submenu(bar, _("_View"), app);
    GtkWidget *show = gtk_check_menu_item_new_with_mnemonic(_("_Video area"));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(show), app->show_video);
    g_signal_connect(show, "toggled", G_CALLBACK(on_show_video), app);
    accel(show, app, GDK_v, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), show);
    separator(m);
    static const char *const video_labels[] = {N_("Video _above the playlists"),
                                               N_("Video on the _left"),
                                               N_("Video on the _right")};
    radio_group(m, app, video_labels, KP_VIDEO_POS_COUNT, app->video_pos,
                G_CALLBACK(on_video_pos));
    separator(m);
    static const char *const controls_labels[] = {N_("Controls at the _bottom"),
                                                  N_("Controls at the _top")};
    radio_group(m, app, controls_labels, KP_CONTROLS_POS_COUNT, app->controls_pos,
                G_CALLBACK(on_controls_pos));

    m = submenu(bar, _("_Help"), app);
    item(m, _("_About"), G_CALLBACK(on_about), app);
    return bar;
}

/* Alt+1..9: go to that tab. */
static gboolean on_key(GtkWidget *widget, GdkEventKey *ev, gpointer data)
{
    (void)widget;
    kp_app *app = data;
    if ((ev->state & gtk_accelerator_get_default_mod_mask()) != GDK_MOD1_MASK)
        return FALSE;
    if (ev->keyval >= GDK_1 && ev->keyval <= GDK_9) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), (int)(ev->keyval - GDK_1));
        return TRUE;
    }
    return FALSE;
}

static gboolean on_configure(GtkWidget *widget, GdkEventConfigure *ev, gpointer data)
{
    (void)widget;
    kp_window *w = data;
    if (!w->maximized) {
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
    close_window(data);
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
    app->show_video = kp_prefs_get_bool(app->prefs, "layout", "show_video", false);

    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "Kiplay");
    gtk_window_set_icon_name(GTK_WINDOW(app->window), "kiplay");
    w.width = kp_prefs_get_int(app->prefs, "window", "width", 760, 300, 20000);
    w.height = kp_prefs_get_int(app->prefs, "window", "height", 520, 200, 20000);
    gtk_window_set_default_size(GTK_WINDOW(app->window), w.width, w.height);
    if (kp_prefs_get_bool(app->prefs, "window", "maximized", false))
        gtk_window_maximize(GTK_WINDOW(app->window));
    app->accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(app->window), app->accel);

    app->body = gtk_vbox_new(FALSE, 0);
    gtk_container_add(GTK_CONTAINER(app->window), app->body);
    gtk_box_pack_start(GTK_BOX(app->body), menu_bar(app), FALSE, FALSE, 0);

    /* The video area: black, and later the backend's window (--wid). */
    app->video = gtk_drawing_area_new();
    GdkColor black = {0, 0, 0, 0};
    gtk_widget_modify_bg(app->video, GTK_STATE_NORMAL, &black);
    gtk_widget_set_size_request(app->video, 160, 90);
    g_object_ref_sink(app->video);

    kp_playlists_init(app);
    app->controls = kp_controls_new(app);
    app->status = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(app->status), 0.0, 0.5);
    gtk_misc_set_padding(GTK_MISC(app->status), 6, 2);
    gtk_box_pack_end(GTK_BOX(app->body), app->status, FALSE, FALSE, 0);

    gtk_widget_show_all(app->body);
    kp_layout_apply(app);
    kp_playlists_update_status(app);

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

    g_source_remove(sig_int);
    g_source_remove(sig_term);
    g_object_unref(app->video);
    g_object_unref(app->notebook);
    g_object_unref(app->controls);
    kp_plset_free(app->playlists);
    kp_prefs_free(app->prefs);
    return 0;
}
