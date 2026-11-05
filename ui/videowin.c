/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The backend's --wid: a plain X window of ours filling the video area,
 * a child of the area's own X window, instead of that GdkWindow itself.
 * GDK doesn't know about it, so it can be resized, redirected or moved
 * around (for X-DENSITY, later) without upsetting GDK's bookkeeping of its
 * windows; mpv follows its --wid's size by itself.
 *
 * It selects no input, so clicks and the wheel go on to the area, as they
 * did when mpv's window sat right in it. The X server destroys it with
 * the area's window; we only forget its XID then. */
#include <X11/Xlib.h>
#include <gdk/gdkx.h>

#include "app.h"
#include "log.h"

static Window win;
static int win_w, win_h;

static void fit(GtkWidget *video)
{
    if (!win || !gtk_widget_get_realized(video))
        return;
    int w = MAX(video->allocation.width, 1), h = MAX(video->allocation.height, 1);
    if (w == win_w && h == win_h)
        return;
    win_w = w;
    win_h = h;
    XResizeWindow(GDK_WINDOW_XDISPLAY(video->window), win, (unsigned)w, (unsigned)h);
}

static void on_allocate(GtkWidget *video, GtkAllocation *a, gpointer data)
{
    (void)a;
    (void)data;
    fit(video);
}

static void on_unrealize(GtkWidget *video, gpointer data)
{
    (void)video;
    (void)data;
    win = 0;
}

unsigned long kp_videowin_xid(kp_app *app)
{
    if (win)
        return win;
    GtkWidget *video = app->video;
    gtk_widget_realize(video);
    Display *dpy = GDK_WINDOW_XDISPLAY(video->window);
    win_w = MAX(video->allocation.width, 1);
    win_h = MAX(video->allocation.height, 1);
    /* Same visual and depth as the area (CopyFromParent), black under
     * whatever mpv hasn't drawn yet. */
    win = XCreateSimpleWindow(dpy, GDK_WINDOW_XID(video->window), 0, 0, (unsigned)win_w,
                              (unsigned)win_h, 0, 0,
                              BlackPixel(dpy, DefaultScreen(dpy)));
    XMapWindow(dpy, win);
    gdk_flush();

    static bool connected;
    if (!connected) {
        connected = true;
        g_signal_connect_after(video, "size-allocate", G_CALLBACK(on_allocate), NULL);
        g_signal_connect(video, "unrealize", G_CALLBACK(on_unrealize), NULL);
    }
    kp_log_debug("videowin: 0x%lx in the area's 0x%lx, %dx%d", (unsigned long)win,
                 (unsigned long)GDK_WINDOW_XID(video->window), win_w, win_h);
    return win;
}

unsigned long kp_videowin_peek(void)
{
    return win;
}
