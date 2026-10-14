/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "app.h"

/* The controls in fullscreen: the same transport bar (app->controls),
 * moved into a popup window along the bottom or top edge of the monitor
 * ([layout] fullscreen_controls). It shows when the mouse moves and goes
 * away, with the pointer, after HIDE_MS still outside it. GTK 2 has no
 * overlay and the video is another process's X window, so a popup
 * (override-redirect, above the fullscreen window) is what can float on
 * top of it. The popup is created once and kept. */

#define HIDE_MS 2000

static GtkWidget *popup;
static guint hide_timer;
static gint last_x = -1, last_y = -1; /* root coordinates of the last motion */

static bool pointer_inside(GtkWidget *w)
{
    if (!w || !gtk_widget_get_visible(w) || !w->window)
        return false;
    int x, y, ww, wh;
    gdk_window_get_pointer(w->window, &x, &y, NULL);
    gdk_drawable_get_size(w->window, &ww, &wh);
    return x >= 0 && y >= 0 && x < ww && y < wh;
}

static void set_cursor_hidden(kp_app *app, bool hidden)
{
    static GdkCursor *blank;
    if (!app->window->window)
        return;
    if (hidden && !blank)
        blank = gdk_cursor_new(GDK_BLANK_CURSOR);
    /* The video area and mpv's window have no cursor of their own: they
     * take the toplevel's. */
    gdk_window_set_cursor(app->window->window, hidden ? blank : NULL);
}

static gboolean on_hide_timer(gpointer data)
{
    kp_app *app = data;
    GtkWidget *vol = gtk_scale_button_get_popup(GTK_SCALE_BUTTON(app->volume));
    /* Kept while it is being used: pointer on it, seek bar held, volume
     * slider open. */
    if (pointer_inside(popup) || app->seek_dragging ||
        (vol && gtk_widget_get_visible(gtk_widget_get_toplevel(vol))))
        return G_SOURCE_CONTINUE;
    hide_timer = 0;
    gtk_widget_hide(popup);
    set_cursor_hidden(app, true);
    return G_SOURCE_REMOVE;
}

static void place(kp_app *app)
{
    GdkScreen *screen = gtk_window_get_screen(GTK_WINDOW(app->window));
    int mon = app->window->window
                  ? gdk_screen_get_monitor_at_window(screen, app->window->window)
                  : 0;
    GdkRectangle r;
    gdk_screen_get_monitor_geometry(screen, mon, &r);
    GtkRequisition req;
    gtk_widget_size_request(popup, &req);
    bool top = kp_prefs_get_choice(app->prefs, "layout", "fullscreen_controls",
                                   kp_controls_pos_names, KP_CONTROLS_BOTTOM) == KP_CONTROLS_TOP;
    gtk_window_resize(GTK_WINDOW(popup), r.width, req.height);
    gtk_window_move(GTK_WINDOW(popup), r.x, top ? r.y : r.y + r.height - req.height);
}

/* Shows the bar (if in fullscreen) and starts the countdown again. */
static void show(kp_app *app)
{
    if (!app->fullscreen || !popup)
        return;
    set_cursor_hidden(app, false);
    if (!gtk_widget_get_visible(popup)) {
        place(app);
        gtk_widget_show(popup);
        gdk_window_raise(popup->window);
    }
    if (hide_timer)
        g_source_remove(hide_timer);
    hide_timer = g_timeout_add(HIDE_MS, on_hide_timer, app);
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data)
{
    (void)w;
    gint x = (gint)ev->x_root, y = (gint)ev->y_root;
    /* Windows mapped or unmapped under a still pointer can report it. */
    if (x == last_x && y == last_y)
        return FALSE;
    last_x = x;
    last_y = y;
    show(data);
    return FALSE;
}

void kp_fsbar_init(kp_app *app)
{
    /* Motion anywhere in the window (the video area and mpv's window
     * don't take it, so it reaches the toplevel) and on the bar itself. */
    gtk_widget_add_events(app->window, GDK_POINTER_MOTION_MASK);
    g_signal_connect(app->window, "motion-notify-event", G_CALLBACK(on_motion), app);
}

void kp_fsbar_attach(kp_app *app)
{
    if (!popup) {
        popup = gtk_window_new(GTK_WINDOW_POPUP);
        gtk_window_set_transient_for(GTK_WINDOW(popup), GTK_WINDOW(app->window));
        gtk_widget_add_events(popup, GDK_POINTER_MOTION_MASK);
        g_signal_connect(popup, "motion-notify-event", G_CALLBACK(on_motion), app);
    }
    if (gtk_widget_get_parent(app->controls) == popup)
        return;
    GtkWidget *parent = gtk_widget_get_parent(app->controls);
    if (parent)
        gtk_container_remove(GTK_CONTAINER(parent), app->controls);
    gtk_container_add(GTK_CONTAINER(popup), app->controls);
    gtk_widget_show(app->controls);
    show(app);
}

void kp_fsbar_detach(kp_app *app)
{
    if (hide_timer) {
        g_source_remove(hide_timer);
        hide_timer = 0;
    }
    set_cursor_hidden(app, false);
    if (!popup)
        return;
    gtk_widget_hide(popup);
    if (gtk_widget_get_parent(app->controls) == popup)
        gtk_container_remove(GTK_CONTAINER(popup), app->controls);
}
