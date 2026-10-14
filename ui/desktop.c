/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "app.h"
#include "i18n.h"
#include "mpris.h"

/* Kiplay seen from the rest of the desktop: MPRIS on the session bus
 * (src/mpris.c; [general] mpris, on by default) and an optional icon in
 * the tray ([general] tray, close_to_tray). Both follow what player.c
 * tells kp_desktop_update. One of each per process. */

static kp_mpris *mpris;
static GtkStatusIcon *tray;
static const char *last_location; /* the entry MPRIS last heard of */

/* ---- MPRIS --------------------------------------------------------------- */

static void op_play(void *ud) { kp_player_play(ud); }
static void op_pause(void *ud) { kp_player_pause(ud); }
static void op_play_pause(void *ud) { kp_player_toggle_pause(ud); }
static void op_stop(void *ud) { kp_player_stop(ud); }
static void op_next(void *ud) { kp_player_next(ud); }
static void op_previous(void *ud) { kp_player_prev(ud); }
static void op_seek(void *ud, double offset) { kp_player_seek_relative(ud, offset); }
static void op_set_position(void *ud, double s) { kp_player_seek(ud, s); }
static void op_raise(void *ud) { kp_desktop_present(ud); }

static void op_set_volume(void *ud, double v)
{
    kp_controls_set_volume(ud, (int)(v * 100 + 0.5));
}

static void op_set_repeat(void *ud, bool on)
{
    kp_app *app = ud;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->repeat_btn), on);
}

static void op_set_shuffle(void *ud, bool on)
{
    kp_app *app = ud;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->shuffle_btn), on);
}

static gboolean quit_idle(gpointer data)
{
    kp_window_quit(data);
    return G_SOURCE_REMOVE;
}

/* Not from inside the bus call: quitting takes MPRIS off the bus. */
static void op_quit(void *ud)
{
    g_idle_add(quit_idle, ud);
}

static void op_open_uri(void *ud, const char *uri)
{
    char *locations[] = {(char *)uri, NULL};
    kp_player_open(ud, locations, false);
}

static double op_position(void *ud)
{
    kp_app *app = ud;
    return app->playing.position;
}

static const kp_mpris_ops mpris_ops = {
    .play = op_play,
    .pause = op_pause,
    .play_pause = op_play_pause,
    .stop = op_stop,
    .next = op_next,
    .previous = op_previous,
    .seek = op_seek,
    .set_position = op_set_position,
    .set_volume = op_set_volume,
    .set_repeat = op_set_repeat,
    .set_shuffle = op_set_shuffle,
    .raise = op_raise,
    .quit = op_quit,
    .open_uri = op_open_uri,
    .position = op_position,
};

static const kp_entry *playing_entry(kp_app *app)
{
    kp_playlist *pl = kp_playlists_get(app, app->playing.playlist_id);
    return pl && app->playing.index >= 0 ? kp_playlist_get(pl, (size_t)app->playing.index) : NULL;
}

/* What to call the playing entry: its title, or what the player says, or
 * the file's name. g_free it. */
static char *title_of(kp_app *app, const kp_entry *e)
{
    if (e && e->title)
        return g_strdup(e->title);
    if (app->playing.title)
        return g_strdup(app->playing.title);
    return e ? g_path_get_basename(e->location) : NULL;
}

static void update_mpris(kp_app *app, const kp_entry *e)
{
    const kp_playing *p = &app->playing;
    kp_mpris_set_status(mpris, !p->loaded ? KP_MPRIS_STOPPED
                               : p->paused ? KP_MPRIS_PAUSED
                                           : KP_MPRIS_PLAYING);
    if (e && p->location) {
        char *title = title_of(app, e);
        char *url = e->location[0] == '/' ? g_filename_to_uri(e->location, NULL, NULL)
                                          : g_strdup(e->location);
        kp_mpris_track t = {title, e->artist, e->album, url,
                            p->duration > 0 ? p->duration
                            : e->length_ms > 0 ? (double)e->length_ms / 1000
                                                : 0};
        kp_mpris_set_track(mpris, &t, p->location != last_location);
        g_free(title);
        g_free(url);
    } else {
        kp_mpris_set_track(mpris, NULL, false);
    }
    last_location = p->location;
    kp_mpris_set_volume(mpris, p->muted ? 0
                              : kp_prefs_get_int(app->prefs, "playback", "volume", 100, 0, 100) /
                                    100.0);
    kp_mpris_set_repeat(mpris, kp_prefs_get_bool(app->prefs, "playback", "repeat", false));
    kp_mpris_set_shuffle(mpris, kp_prefs_get_bool(app->prefs, "playback", "shuffle", false));
    kp_mpris_set_seekable(mpris, p->loaded && p->duration > 0);
}

/* ---- the tray ------------------------------------------------------------ */

void kp_desktop_present(kp_app *app)
{
    gtk_widget_show(app->window);
    gtk_window_present(GTK_WINDOW(app->window));
}

static void toggle_window(kp_app *app)
{
    if (gtk_widget_get_visible(app->window) &&
        gtk_window_is_active(GTK_WINDOW(app->window)))
        gtk_widget_hide(app->window);
    else
        kp_desktop_present(app);
}

static void on_tray_activate(GtkStatusIcon *icon, gpointer data)
{
    (void)icon;
    toggle_window(data);
}

static GtkWidget *tray_item(GtkWidget *menu, const char *label, void (*cb)(kp_app *), kp_app *app)
{
    GtkWidget *it = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect_swapped(it, "activate", G_CALLBACK(cb), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    return it;
}

static void on_tray_menu(GtkStatusIcon *icon, guint button, guint time, gpointer data)
{
    kp_app *app = data;
    GtkWidget *m = gtk_menu_new();
    tray_item(m, _("_Play / Pause"), kp_player_toggle_pause, app);
    tray_item(m, _("_Stop"), kp_player_stop, app);
    tray_item(m, _("P_revious"), kp_player_prev, app);
    tray_item(m, _("_Next"), kp_player_next, app);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    tray_item(m, gtk_widget_get_visible(app->window) ? _("_Hide window") : _("_Show window"),
              toggle_window, app);
    tray_item(m, _("_Quit"), kp_window_quit, app);
    gtk_widget_show_all(m);
    g_signal_connect(m, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup(GTK_MENU(m), NULL, NULL, gtk_status_icon_position_menu, icon, button, time);
}

static gboolean on_tray_button(GtkStatusIcon *icon, GdkEventButton *ev, gpointer data)
{
    (void)icon;
    if (ev->button != 2)
        return FALSE;
    kp_player_toggle_pause(data);
    return TRUE;
}

static gboolean on_tray_scroll(GtkStatusIcon *icon, GdkEventScroll *ev, gpointer data)
{
    (void)icon;
    kp_controls_change_volume(data, ev->direction == GDK_SCROLL_UP ? 5 : -5);
    return TRUE;
}

static void update_tray(kp_app *app)
{
    const char *title = gtk_window_get_title(GTK_WINDOW(app->window));
    gtk_status_icon_set_tooltip_text(tray, title ? title : "Kiplay");
}

/* ---- the rest ------------------------------------------------------------ */

void kp_desktop_update(kp_app *app)
{
    if (mpris)
        update_mpris(app, playing_entry(app));
    if (tray)
        update_tray(app);
}

void kp_desktop_seeked(kp_app *app, double seconds)
{
    (void)app;
    if (mpris)
        kp_mpris_seeked(mpris, seconds);
}

void kp_desktop_prefs_changed(kp_app *app)
{
    bool want_mpris = kp_prefs_get_bool(app->prefs, "general", "mpris", true);
    if (want_mpris && !mpris) {
        mpris = kp_mpris_new(&mpris_ops, app);
        last_location = NULL;
    } else if (!want_mpris && mpris) {
        kp_mpris_free(mpris);
        mpris = NULL;
    }
    bool want_tray = kp_prefs_get_bool(app->prefs, "general", "tray", false);
    if (want_tray && !tray) {
        tray = gtk_status_icon_new_from_icon_name("kiplay");
        g_signal_connect(tray, "activate", G_CALLBACK(on_tray_activate), app);
        g_signal_connect(tray, "popup-menu", G_CALLBACK(on_tray_menu), app);
        g_signal_connect(tray, "button-press-event", G_CALLBACK(on_tray_button), app);
        g_signal_connect(tray, "scroll-event", G_CALLBACK(on_tray_scroll), app);
    } else if (!want_tray && tray) {
        g_object_unref(tray);
        tray = NULL;
        /* Nowhere to come back from. */
        kp_desktop_present(app);
    }
    kp_desktop_update(app);
}

void kp_desktop_init(kp_app *app)
{
    kp_desktop_prefs_changed(app);
}

void kp_desktop_free(kp_app *app)
{
    (void)app;
    kp_mpris_free(mpris);
    mpris = NULL;
    if (tray)
        g_object_unref(tray);
    tray = NULL;
}

bool kp_desktop_close_to_tray(kp_app *app)
{
    if (!tray || !gtk_status_icon_is_embedded(tray) ||
        !kp_prefs_get_bool(app->prefs, "general", "close_to_tray", false))
        return false;
    gtk_widget_hide(app->window);
    return true;
}
