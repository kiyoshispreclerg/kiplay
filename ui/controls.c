/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "app.h"
#include "i18n.h"

/* The transport bar: previous, play, pause, stop, next, repeat, shuffle,
 * the seek bar with the time, and the volume. Nothing plays yet (phase 2
 * brings the backend), so the playback buttons are insensitive; repeat and
 * shuffle already remember their state. */

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
    kp_prefs_set_int(data, "playback", "volume", (int)(v * 100 + 0.5));
}

GtkWidget *kp_controls_new(kp_app *app)
{
    GtkWidget *bar = gtk_hbox_new(FALSE, 2);
    gtk_container_set_border_width(GTK_CONTAINER(bar), 2);

    static const struct {
        const char *stock;
        const char *tip;
    } transport[] = {
        {GTK_STOCK_MEDIA_PREVIOUS, N_("Previous")}, {GTK_STOCK_MEDIA_PLAY, N_("Play")},
        {GTK_STOCK_MEDIA_PAUSE, N_("Pause")},       {GTK_STOCK_MEDIA_STOP, N_("Stop")},
        {GTK_STOCK_MEDIA_NEXT, N_("Next")},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(transport); i++) {
        GtkWidget *b = icon_button(transport[i].stock, _(transport[i].tip), false);
        gtk_widget_set_sensitive(b, FALSE);
        gtk_box_pack_start(GTK_BOX(bar), b, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(bar), pref_toggle(app, "media-playlist-repeat", _("Repeat"), "repeat"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar),
                       pref_toggle(app, "media-playlist-shuffle", _("Shuffle"), "shuffle"), FALSE,
                       FALSE, 0);

    GtkWidget *seek = gtk_hscale_new_with_range(0, 1, 0.001);
    gtk_scale_set_draw_value(GTK_SCALE(seek), FALSE);
    gtk_widget_set_sensitive(seek, FALSE);
    gtk_box_pack_start(GTK_BOX(bar), seek, TRUE, TRUE, 6);

    GtkWidget *time = gtk_label_new("--:-- / --:--");
    gtk_box_pack_start(GTK_BOX(bar), time, FALSE, FALSE, 4);

    GtkWidget *vol = gtk_volume_button_new();
    gtk_scale_button_set_value(GTK_SCALE_BUTTON(vol),
                               kp_prefs_get_int(app->prefs, "playback", "volume", 100, 0, 100) / 100.0);
    g_signal_connect(vol, "value-changed", G_CALLBACK(on_volume), app->prefs);
    gtk_box_pack_start(GTK_BOX(bar), vol, FALSE, FALSE, 0);

    gtk_widget_show_all(bar);
    g_object_ref_sink(bar); /* layout.c moves it around */
    return bar;
}
