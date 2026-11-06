/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>

#include "app.h"

/* The level meter on the transport bar: two thin bars, left and right,
 * the peak of what is sounding in dB (-40..0), falling back slowly.
 *
 * View > VU meter shows it (the [layout] vu pref). Hidden, it costs
 * nothing: the engine isn't asked for levels. Shown, it reads the music
 * engine's shared memory (kp_backend_levels) on a timer that runs only
 * while that engine plays, and draws only when a bar moved by a pixel
 * (about 0.4 points of CPU more, docs/BENCH.md). Plain GDK rectangles:
 * no Cairo, no Pango. The video engine (mpv) gives no levels. */

#define VU_W 72
#define VU_BAR 7
#define VU_GAP 2
#define VU_FPS 30
#define VU_FLOOR_DB -40.0
#define VU_FALL_DB (24.0 / VU_FPS) /* dB per frame */

static GtkWidget *area;
static guint timer;
static double shown[2] = {VU_FLOOR_DB, VU_FLOOR_DB};
static int drawn[2]; /* pixels */

static int bar_px(double db)
{
    double f = (db - VU_FLOOR_DB) / -VU_FLOOR_DB;
    return (int)(CLAMP(f, 0, 1) * (VU_W - 2) + 0.5);
}

static gboolean on_expose(GtkWidget *w, GdkEventExpose *ev, gpointer data)
{
    (void)ev;
    (void)data;
    GtkStyle *st = gtk_widget_get_style(w);
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    int y0 = (a.height - 2 * VU_BAR - VU_GAP) / 2;
    /* No double buffer: paint the whole background, the gap included. */
    gdk_draw_rectangle(w->window, st->bg_gc[GTK_STATE_NORMAL], TRUE, 0, 0, a.width, a.height);
    for (int c = 0; c < 2; c++) {
        int y = y0 + c * (VU_BAR + VU_GAP);
        gdk_draw_rectangle(w->window, st->dark_gc[GTK_STATE_NORMAL], TRUE, 0, y, VU_W, VU_BAR);
        if (drawn[c] > 0)
            gdk_draw_rectangle(w->window, st->bg_gc[GTK_STATE_SELECTED], TRUE, 1, y + 1, drawn[c],
                               VU_BAR - 2);
    }
    return TRUE;
}

static gboolean on_tick(gpointer data)
{
    kp_app *app = data;
    kp_backend *b = app->playing.backend;
    int peak[2];
    bool live = b && gtk_widget_get_mapped(area) && kp_backend_levels(b, peak);
    bool moving = false;
    for (int c = 0; c < 2; c++) {
        double db = live && peak[c] > 0 ? 20 * log10(peak[c] / 255.0) : VU_FLOOR_DB;
        shown[c] = MAX(db, shown[c] - VU_FALL_DB);
        if (shown[c] > VU_FLOOR_DB)
            moving = true;
        int px = bar_px(shown[c]);
        if (px != drawn[c]) {
            drawn[c] = px;
            gtk_widget_queue_draw(area);
        }
    }
    if (live || moving)
        return G_SOURCE_CONTINUE;
    timer = 0;
    return G_SOURCE_REMOVE;
}

void kp_vumeter_update(kp_app *app)
{
    if (!area || timer || !gtk_widget_get_visible(area))
        return;
    const kp_playing *p = &app->playing;
    if (p->loaded && !p->paused && p->backend && p->backend == p->audio)
        timer = g_timeout_add(1000 / VU_FPS, on_tick, app);
}

void kp_vumeter_set_shown(kp_app *app, bool on)
{
    if (!area)
        return;
    gtk_widget_set_visible(area, on);
    if (app->playing.audio)
        kp_backend_set_levels(app->playing.audio, on);
    kp_vumeter_update(app);
}

GtkWidget *kp_vumeter_new(kp_app *app)
{
    area = gtk_drawing_area_new();
    gtk_widget_set_no_show_all(area, TRUE);
    if (kp_prefs_get_bool(app->prefs, "layout", "vu", false))
        gtk_widget_show(area);
    gtk_widget_set_size_request(area, VU_W, 2 * VU_BAR + VU_GAP);
    /* Opaque rectangles: no flicker, and no offscreen pixmap per frame. */
    gtk_widget_set_double_buffered(area, FALSE);
    g_signal_connect(area, "expose-event", G_CALLBACK(on_expose), NULL);
    return area;
}
