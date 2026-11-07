/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "app.h"

/* The meters on the transport bar, each in View and off by default:
 *
 * - VU meter: two bars, left and right, the loudness (RMS) of what is
 *   sounding in dB (-40..0): a peak meter would sit at the top with any
 *   loud master;
 * - Spectrum: 12 bars, lows on the left, highs on the right, each the
 *   loudness of its band in dB (-40..0).
 *
 * Both fall back slowly. Hidden, they cost nothing: the engine isn't asked
 * for levels and no timer runs. Shown, one timer reads the music engine's
 * shared memory (kp_backend_levels; the engine does the FFT) while that
 * engine plays, and an area is drawn only when one of its bars moved by a
 * pixel (docs/BENCH.md has the cost). Plain GDK rectangles: no Cairo, no
 * Pango, drawn through the normal double-buffered expose. The video engines (mpv, MPlayer) give no levels, so by default
 * ([layout] meters_autohide) the meters go away while something plays on
 * one of them, and come back with the music. */

#define FPS 30
#define FLOOR_DB -40.0

#define VU_W 72
#define VU_BAR 7
#define VU_GAP 2
#define VU_FALL_DB (24.0 / FPS) /* dB per frame */

#define SP_BAR 5 /* width of a bar */
#define SP_GAP 1
#define SP_H 18
#define SP_W (KP_SPECTRUM_BANDS * (SP_BAR + SP_GAP) + SP_GAP)
#define SP_FALL_DB (45.0 / FPS)

static GtkWidget *box, *vu_area, *sp_area; /* box: hidden with both */
static guint timer;
static double vu_shown[2], sp_shown[KP_SPECTRUM_BANDS];
static int vu_drawn[2], sp_drawn[KP_SPECTRUM_BANDS]; /* pixels */

unsigned kp_meters_wanted(kp_app *app)
{
    return (kp_prefs_get_bool(app->prefs, "layout", "vu", false) ? KP_LEVELS_VU : 0) |
           (kp_prefs_get_bool(app->prefs, "layout", "spectrum", false) ? KP_LEVELS_SPECTRUM : 0);
}

/* Shown or not, as View and the autohide say. */
static void apply_visibility(kp_app *app)
{
    const kp_playing *p = &app->playing;
    unsigned what = kp_meters_wanted(app);
    if (p->backend && p->backend != p->audio &&
        kp_prefs_get_bool(app->prefs, "layout", "meters_autohide", true))
        what = 0;
    gtk_widget_set_visible(box, what != 0);
    gtk_widget_set_visible(vu_area, what & KP_LEVELS_VU);
    gtk_widget_set_visible(sp_area, what & KP_LEVELS_SPECTRUM);
}

static int to_px(double db, int size)
{
    double f = (db - FLOOR_DB) / -FLOOR_DB;
    return (int)(CLAMP(f, 0, 1) * size + 0.5);
}

/* Each paints its whole area, opaque. */
static gboolean on_vu_expose(GtkWidget *w, GdkEventExpose *ev, gpointer data)
{
    (void)ev;
    (void)data;
    GtkStyle *st = gtk_widget_get_style(w);
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    int y0 = (a.height - 2 * VU_BAR - VU_GAP) / 2;
    gdk_draw_rectangle(w->window, st->bg_gc[GTK_STATE_NORMAL], TRUE, 0, 0, a.width, a.height);
    for (int c = 0; c < 2; c++) {
        int y = y0 + c * (VU_BAR + VU_GAP);
        gdk_draw_rectangle(w->window, st->dark_gc[GTK_STATE_NORMAL], TRUE, 0, y, VU_W, VU_BAR);
        if (vu_drawn[c] > 0)
            gdk_draw_rectangle(w->window, st->bg_gc[GTK_STATE_SELECTED], TRUE, 1, y + 1,
                               vu_drawn[c], VU_BAR - 2);
    }
    return TRUE;
}

static gboolean on_sp_expose(GtkWidget *w, GdkEventExpose *ev, gpointer data)
{
    (void)ev;
    (void)data;
    GtkStyle *st = gtk_widget_get_style(w);
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    int y0 = (a.height - SP_H) / 2;
    gdk_draw_rectangle(w->window, st->bg_gc[GTK_STATE_NORMAL], TRUE, 0, 0, a.width, a.height);
    gdk_draw_rectangle(w->window, st->dark_gc[GTK_STATE_NORMAL], TRUE, 0, y0, SP_W, SP_H);
    for (int i = 0; i < KP_SPECTRUM_BANDS; i++)
        if (sp_drawn[i] > 0)
            gdk_draw_rectangle(w->window, st->bg_gc[GTK_STATE_SELECTED], TRUE,
                               SP_GAP + i * (SP_BAR + SP_GAP), y0 + SP_H - 1 - sp_drawn[i], SP_BAR,
                               sp_drawn[i]);
    return TRUE;
}

/* Through the normal expose path, double buffered: drawing straight on
 * the X window (as an earlier version did, to save work) keeps a
 * density-scaling toolkit (gtk2-ng) from rendering the whole window denser. */
static void redraw(GtkWidget *w)
{
    if (gtk_widget_is_drawable(w))
        gtk_widget_queue_draw(w);
}

/* One bar: the new level or the shown one falling; true if it moved a
 * pixel. *moving: it is still above the floor. */
static bool step(double db, double fall, int size, double *shown, int *drawn, bool *moving)
{
    *shown = MAX(db, *shown - fall);
    if (*shown > FLOOR_DB)
        *moving = true;
    int px = to_px(*shown, size);
    if (px == *drawn)
        return false;
    *drawn = px;
    return true;
}

static bool shown_now(GtkWidget *w)
{
    return gtk_widget_get_visible(w) && gtk_widget_get_mapped(w);
}

static gboolean on_tick(gpointer data)
{
    kp_app *app = data;
    kp_backend *b = app->playing.backend;
    bool vu = shown_now(vu_area), sp = shown_now(sp_area);
    kp_levels lv;
    bool live = b && (vu || sp) && kp_backend_levels(b, &lv);
    bool moving = false, changed = false;
    for (int c = 0; vu && c < 2; c++) {
        double db = live ? lv.vu[c] / 4.0 - 64 : FLOOR_DB;
        changed |= step(db, VU_FALL_DB, VU_W - 2, &vu_shown[c], &vu_drawn[c], &moving);
    }
    if (changed)
        redraw(vu_area);
    changed = false;
    for (int i = 0; sp && i < KP_SPECTRUM_BANDS; i++) {
        double db = live ? lv.band[i] / 4.0 - 64 : FLOOR_DB;
        changed |= step(db, SP_FALL_DB, SP_H - 2, &sp_shown[i], &sp_drawn[i], &moving);
    }
    if (changed)
        redraw(sp_area);
    if (live || moving)
        return G_SOURCE_CONTINUE;
    timer = 0;
    return G_SOURCE_REMOVE;
}

void kp_meters_update(kp_app *app)
{
    if (!vu_area)
        return;
    apply_visibility(app);
    if (timer || !kp_meters_wanted(app))
        return;
    const kp_playing *p = &app->playing;
    if (p->loaded && !p->paused && p->backend && p->backend == p->audio)
        timer = g_timeout_add(1000 / FPS, on_tick, app);
}

void kp_meters_changed(kp_app *app)
{
    if (!vu_area)
        return;
    apply_visibility(app);
    if (app->playing.audio)
        kp_backend_set_levels(app->playing.audio, kp_meters_wanted(app));
    kp_meters_update(app);
}

static GtkWidget *meter_area(int w, int h, GCallback expose)
{
    GtkWidget *a = gtk_drawing_area_new();
    gtk_widget_set_no_show_all(a, TRUE);
    gtk_widget_set_size_request(a, w, h);
    g_signal_connect(a, "expose-event", expose, NULL);
    return a;
}

GtkWidget *kp_meters_new(kp_app *app)
{
    vu_shown[0] = vu_shown[1] = FLOOR_DB;
    for (int i = 0; i < KP_SPECTRUM_BANDS; i++)
        sp_shown[i] = FLOOR_DB;
    box = gtk_hbox_new(FALSE, 6);
    gtk_widget_set_no_show_all(box, TRUE);
    vu_area = meter_area(VU_W, 2 * VU_BAR + VU_GAP, G_CALLBACK(on_vu_expose));
    sp_area = meter_area(SP_W, SP_H, G_CALLBACK(on_sp_expose));
    gtk_box_pack_start(GTK_BOX(box), vu_area, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), sp_area, FALSE, FALSE, 0);
    apply_visibility(app);
    return box;
}
