/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "app.h"
#include "i18n.h"
#include "util.h"

/* The transport bar: previous, play/pause, stop, next, repeat, shuffle,
 * the seek bar with the time, and the volume. Repeat, shuffle and the
 * volume are remembered in the prefs. */

/* stock: a GTK stock id, or (for the toggles) a themed icon name. */
static GtkWidget *icon_button(const char *stock, const char *tip, bool toggle)
{
    GtkWidget *b = toggle ? gtk_toggle_button_new() : gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_button_set_focus_on_click(GTK_BUTTON(b), FALSE);
    GtkWidget *img = g_str_has_prefix(stock, "gtk-")
                         ? gtk_image_new_from_stock(stock, GTK_ICON_SIZE_BUTTON)
                         : gtk_image_new_from_icon_name(stock, GTK_ICON_SIZE_BUTTON);
    gtk_container_add(GTK_CONTAINER(b), img);
    gtk_widget_set_tooltip_text(b, tip);
    return b;
}

static void on_toggle(GtkToggleButton *b, gpointer data)
{
    kp_app *app = data;
    const char *key = g_object_get_data(G_OBJECT(b), "kp-key");
    kp_prefs_set_bool(app->prefs, "playback", key, gtk_toggle_button_get_active(b));
    kp_player_requeue(app);
    kp_desktop_update(app);
}

static GtkWidget *pref_toggle(kp_app *app, const char *stock, const char *tip, const char *key)
{
    GtkWidget *b = icon_button(stock, tip, true);
    g_object_set_data(G_OBJECT(b), "kp-key", (gpointer)key);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b),
                                 kp_prefs_get_bool(app->prefs, "playback", key, false));
    g_signal_connect(b, "toggled", G_CALLBACK(on_toggle), app);
    return b;
}

static void on_volume(GtkScaleButton *b, gdouble v, gpointer data)
{
    (void)b;
    kp_player_set_volume(data, (int)(v * 100 + 0.5));
}

/* The seek bar follows the playback unless the user holds it; the jump
 * happens when it is let go (or clicked), not on every pixel of a drag. */
static gboolean on_seek_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w;
    kp_app *app = data;
    /* GTK 2 moves the slider to the click only with the middle button; the
     * left one would step a page. Players jump to where you click. */
    if (ev->button == 1)
        ev->button = 2;
    app->seek_dragging = true;
    return FALSE;
}

static gboolean on_seek_release(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    kp_app *app = data;
    if (ev->button == 1)
        ev->button = 2;
    app->seek_dragging = false;
    kp_player_seek(app, gtk_range_get_value(GTK_RANGE(w)));
    return FALSE;
}

static gboolean on_seek_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer data)
{
    (void)w;
    kp_player_seek_relative(data, ev->direction == GDK_SCROLL_UP ? 5 : -5);
    return TRUE;
}

static void update_time_label(kp_app *app, double position)
{
    char pos[32], dur[32], text[80];
    const kp_playing *p = &app->playing;
    if (!p->loaded) {
        gtk_label_set_text(GTK_LABEL(app->time_label), "--:-- / --:--");
        return;
    }
    kp_format_duration(pos, sizeof pos, (int64_t)(position * 1000));
    kp_format_duration(dur, sizeof dur, p->duration > 0 ? (int64_t)(p->duration * 1000) : -1);
    g_snprintf(text, sizeof text, "%s / %s", pos, dur);
    gtk_label_set_text(GTK_LABEL(app->time_label), text);
}

/* While dragging, the label shows where the drop will land. */
static void on_seek_moved(GtkRange *r, gpointer data)
{
    kp_app *app = data;
    if (app->seek_dragging)
        update_time_label(app, gtk_range_get_value(r));
}

void kp_controls_update(kp_app *app)
{
    const kp_playing *p = &app->playing;
    bool seekable = p->loaded && p->duration > 0;
    gtk_widget_set_sensitive(app->seek, seekable);
    if (!app->seek_dragging) {
        GtkAdjustment *adj = gtk_range_get_adjustment(GTK_RANGE(app->seek));
        double upper = seekable ? p->duration : 1;
        if (gtk_adjustment_get_upper(adj) != upper)
            gtk_range_set_range(GTK_RANGE(app->seek), 0, upper);
        gtk_range_set_value(GTK_RANGE(app->seek), seekable ? p->position : 0);
        update_time_label(app, p->position);
    }
}

void kp_controls_set_volume(kp_app *app, int volume)
{
    /* The button tells the player through its value-changed signal. */
    gtk_scale_button_set_value(GTK_SCALE_BUTTON(app->volume), CLAMP(volume, 0, 100) / 100.0);
}

void kp_controls_change_volume(kp_app *app, int delta)
{
    int v = (int)(gtk_scale_button_get_value(GTK_SCALE_BUTTON(app->volume)) * 100 + 0.5);
    kp_controls_set_volume(app, v + delta);
}

static GtkWidget *action_button(kp_app *app, const char *stock, const char *tip,
                                void (*action)(kp_app *))
{
    GtkWidget *b = icon_button(stock, tip, false);
    g_signal_connect_swapped(b, "clicked", G_CALLBACK(action), app);
    return b;
}

GtkWidget *kp_controls_new(kp_app *app)
{
    GtkWidget *bar = gtk_hbox_new(FALSE, 2);
    gtk_container_set_border_width(GTK_CONTAINER(bar), 2);

    gtk_box_pack_start(GTK_BOX(bar),
                       action_button(app, GTK_STOCK_MEDIA_PREVIOUS, _("Previous"), kp_player_prev),
                       FALSE, FALSE, 0);
    app->play_btn = action_button(app, GTK_STOCK_MEDIA_PLAY, _("Play"), kp_player_play);
    app->pause_btn = action_button(app, GTK_STOCK_MEDIA_PAUSE, _("Pause"), kp_player_pause);
    gtk_box_pack_start(GTK_BOX(bar), app->play_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), app->pause_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar),
                       action_button(app, GTK_STOCK_MEDIA_STOP, _("Stop"), kp_player_stop), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar),
                       action_button(app, GTK_STOCK_MEDIA_NEXT, _("Next"), kp_player_next), FALSE,
                       FALSE, 0);
    app->repeat_btn = pref_toggle(app, "media-playlist-repeat", _("Repeat"), "repeat");
    gtk_box_pack_start(GTK_BOX(bar), app->repeat_btn, FALSE, FALSE, 0);
    app->shuffle_btn = pref_toggle(app, "media-playlist-shuffle", _("Shuffle"), "shuffle");
    gtk_box_pack_start(GTK_BOX(bar), app->shuffle_btn, FALSE, FALSE, 0);

    app->seek = gtk_hscale_new_with_range(0, 1, 1);
    gtk_scale_set_draw_value(GTK_SCALE(app->seek), FALSE);
    gtk_range_set_update_policy(GTK_RANGE(app->seek), GTK_UPDATE_DISCONTINUOUS);
    g_signal_connect(app->seek, "button-press-event", G_CALLBACK(on_seek_press), app);
    g_signal_connect(app->seek, "button-release-event", G_CALLBACK(on_seek_release), app);
    g_signal_connect(app->seek, "scroll-event", G_CALLBACK(on_seek_scroll), app);
    g_signal_connect(app->seek, "value-changed", G_CALLBACK(on_seek_moved), app);
    gtk_box_pack_start(GTK_BOX(bar), app->seek, TRUE, TRUE, 6);

    app->time_label = gtk_label_new("--:-- / --:--");
    gtk_box_pack_start(GTK_BOX(bar), app->time_label, FALSE, FALSE, 4);

    app->volume = gtk_volume_button_new();
    gtk_scale_button_set_value(GTK_SCALE_BUTTON(app->volume),
                               kp_prefs_get_int(app->prefs, "playback", "volume", 100, 0, 100) / 100.0);
    g_signal_connect(app->volume, "value-changed", G_CALLBACK(on_volume), app);
    gtk_box_pack_start(GTK_BOX(bar), app->volume, FALSE, FALSE, 0);

    /* In an event box: a right click on the bar opens the window's menu. */
    GtkWidget *box = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(box), FALSE);
    gtk_container_add(GTK_CONTAINER(box), bar);
    gtk_widget_show_all(box);
    gtk_widget_set_no_show_all(app->pause_btn, TRUE);
    gtk_widget_set_no_show_all(app->play_btn, TRUE);
    gtk_widget_hide(app->pause_btn);
    g_object_ref_sink(box); /* layout.c moves it around */
    return box;
}
