/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The backend's --wid: a plain X window of ours filling the video area,
 * a child of the area's own X window, instead of that GdkWindow itself.
 * GDK doesn't know about it, so it can be resized, redirected or moved
 * around without upsetting GDK's bookkeeping of its windows; mpv follows
 * its --wid's size by itself.
 *
 * At a density (X-DENSITY: the compositor shows a denser picture of the
 * window than its real size), the window is redirected (Composite,
 * manual) and made density times the area's size: mpv renders that many
 * pixels, which a compositor can sample sharply, and nothing of it shows
 * by itself. What the area shows, the real 1x window that recorders and
 * screenshots see, is a scaled-down copy made here (Render) whenever the
 * window changes (Damage). The window is clipped to the area, so the part
 * beyond it takes no clicks either.
 *
 * It selects no input, so clicks and the wheel go on to the area, as they
 * did when mpv's window sat right in it. The X server destroys it (and
 * the damage and pictures on it) with the area's window; we only forget
 * the XIDs then. */
#include <math.h>
#include <stdlib.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <gdk/gdkx.h>

#include "app.h"
#include "log.h"

/* A density beyond this is not a zoom anyone asked for. */
#define MAX_DENSITY 8.0

static struct {
    GtkWidget *video;
    Window win;
    int w, h;       /* the window's size now: the area's, times the density */
    double density; /* 0: not redirected */
    bool usable;    /* the server has Composite and Damage */
    int damage_event;
    Damage damage;
    Picture src, dst; /* the window, the area */
    guint copy_idle;
} vw;

static Display *dpy(void)
{
    return GDK_WINDOW_XDISPLAY(vw.video->window);
}

/* The window's contents, scaled down, into the area. */
static void copy(void)
{
    if (!vw.damage)
        return;
    int w = vw.video->allocation.width, h = vw.video->allocation.height;
    XRenderComposite(dpy(), PictOpSrc, vw.src, None, vw.dst, 0, 0, 0, 0, 0, 0,
                     (unsigned)MAX(w, 1), (unsigned)MAX(h, 1));
}

static gboolean copy_later(gpointer data)
{
    (void)data;
    vw.copy_idle = 0;
    copy();
    return G_SOURCE_REMOVE;
}

/* The source picture maps area pixels to window pixels. */
static void set_transform(void)
{
    double s = vw.density > 0 ? vw.density : 1;
    XTransform t = {{{XDoubleToFixed(s), 0, 0},
                     {0, XDoubleToFixed(s), 0},
                     {0, 0, XDoubleToFixed(1)}}};
    XRenderSetPictureTransform(dpy(), vw.src, &t);
    XRenderSetPictureFilter(dpy(), vw.src, s == 1 ? FilterNearest : FilterGood, NULL, 0);
}

static void fit(void)
{
    if (!vw.win || !gtk_widget_get_realized(vw.video))
        return;
    double s = vw.density > 0 ? vw.density : 1;
    int w = MAX((int)ceil(vw.video->allocation.width * s), 1);
    int h = MAX((int)ceil(vw.video->allocation.height * s), 1);
    if (w == vw.w && h == vw.h)
        return;
    vw.w = w;
    vw.h = h;
    XResizeWindow(dpy(), vw.win, (unsigned)w, (unsigned)h);
}

static void on_allocate(GtkWidget *video, GtkAllocation *a, gpointer data)
{
    (void)video;
    (void)a;
    (void)data;
    fit();
}

/* Whatever GTK draws or the server clears in the area stays out of the
 * window (children clip it), but an area exposed with nothing new from
 * mpv (paused, no compositor) needs the copy again. */
static gboolean on_expose(GtkWidget *video, GdkEventExpose *ev, gpointer data)
{
    (void)video;
    (void)ev;
    (void)data;
    if (vw.damage && !vw.copy_idle)
        vw.copy_idle = g_idle_add(copy_later, NULL);
    return FALSE;
}

static void on_unrealize(GtkWidget *video, gpointer data)
{
    (void)video;
    (void)data;
    vw.win = 0;
    vw.damage = 0;
    vw.src = vw.dst = 0;
    if (vw.copy_idle) {
        g_source_remove(vw.copy_idle);
        vw.copy_idle = 0;
    }
}

static GdkFilterReturn filter(GdkXEvent *xev, GdkEvent *ev, gpointer data)
{
    (void)ev;
    (void)data;
    XEvent *e = xev;
    if (vw.damage && e->type == vw.damage_event + XDamageNotify &&
        ((XDamageNotifyEvent *)e)->damage == vw.damage) {
        XDamageSubtract(e->xany.display, vw.damage, None, None);
        copy();
        return GDK_FILTER_REMOVE;
    }
    return GDK_FILTER_CONTINUE;
}

static void redirect(void)
{
    Display *d = dpy();
    Window area = GDK_WINDOW_XID(vw.video->window);
    XWindowAttributes wa;
    XGetWindowAttributes(d, vw.win, &wa);
    XRenderPictFormat *fmt = XRenderFindVisualFormat(d, wa.visual);
    if (!fmt) {
        kp_log_debug("videowin: no Render format for the window's visual");
        return;
    }

    gdk_error_trap_push();
    XCompositeRedirectWindow(d, vw.win, CompositeRedirectManual);
    gdk_flush();
    if (gdk_error_trap_pop()) {
        /* Somebody else redirects it already. */
        kp_log_debug("videowin: can't redirect 0x%lx", (unsigned long)vw.win);
        return;
    }
    /* Both through the children: mpv's window is in ours, ours in the
     * area. */
    XRenderPictureAttributes pa = {.subwindow_mode = IncludeInferiors};
    vw.src = XRenderCreatePicture(d, vw.win, fmt, CPSubwindowMode, &pa);
    vw.dst = XRenderCreatePicture(d, area, fmt, CPSubwindowMode, &pa);
    vw.damage = XDamageCreate(d, vw.win, XDamageReportNonEmpty);
}

static void unredirect(void)
{
    Display *d = dpy();
    if (!vw.damage)
        return;
    gdk_error_trap_push();
    XDamageDestroy(d, vw.damage);
    XRenderFreePicture(d, vw.src);
    XRenderFreePicture(d, vw.dst);
    XCompositeUnredirectWindow(d, vw.win, CompositeRedirectManual);
    gdk_flush();
    (void)gdk_error_trap_pop();
    vw.damage = 0;
    vw.src = vw.dst = 0;
    if (vw.copy_idle) {
        g_source_remove(vw.copy_idle);
        vw.copy_idle = 0;
    }
}

void kp_videowin_set_density(double density)
{
    if (density < 1)
        density = 0;
    density = MIN(density, MAX_DENSITY);
    if (density > 0 && !vw.usable)
        density = 0;
    if (!vw.win) {
        vw.density = density; /* for when it is made */
        return;
    }
    if (density == vw.density)
        return;

    if (density > 0 && !vw.damage)
        redirect();
    else if (density == 0)
        unredirect();
    vw.density = vw.damage ? density : 0;
    if (vw.damage)
        set_transform();
    fit();
    if (vw.damage)
        copy();
    gdk_flush();
    kp_log_debug("videowin: density %g, %dx%d", vw.density, vw.w, vw.h);
}

unsigned long kp_videowin_xid(kp_app *app)
{
    if (vw.win)
        return vw.win;
    vw.video = app->video;
    gtk_widget_realize(vw.video);
    Display *d = dpy();

    static bool initialized;
    if (!initialized) {
        initialized = true;
        int major = 0, minor = 2, event_base, error_base;
        vw.usable = XCompositeQueryExtension(d, &event_base, &error_base) &&
                    XCompositeQueryVersion(d, &major, &minor) &&
                    (major > 0 || minor >= 2) &&
                    XDamageQueryExtension(d, &vw.damage_event, &error_base);
        g_signal_connect_after(vw.video, "size-allocate", G_CALLBACK(on_allocate), NULL);
        g_signal_connect_after(vw.video, "expose-event", G_CALLBACK(on_expose), NULL);
        g_signal_connect(vw.video, "unrealize", G_CALLBACK(on_unrealize), NULL);
        gdk_window_add_filter(NULL, filter, NULL);
        /* KP_VIDEO_DENSITY=S: redirected at density S from the start, to
         * try the window without a compositor asking (1: redirected at
         * the area's size). */
        const char *env = g_getenv("KP_VIDEO_DENSITY");
        if (env && *env)
            vw.density = g_ascii_strtod(env, NULL);
    }

    vw.w = MAX(vw.video->allocation.width, 1);
    vw.h = MAX(vw.video->allocation.height, 1);
    /* Same visual and depth as the area (CopyFromParent), black under
     * whatever mpv hasn't drawn yet. */
    vw.win = XCreateSimpleWindow(d, GDK_WINDOW_XID(vw.video->window), 0, 0, (unsigned)vw.w,
                                 (unsigned)vw.h, 0, 0, BlackPixel(d, DefaultScreen(d)));
    XMapWindow(d, vw.win);
    kp_log_debug("videowin: 0x%lx in the area's 0x%lx, %dx%d", (unsigned long)vw.win,
                 (unsigned long)GDK_WINDOW_XID(vw.video->window), vw.w, vw.h);

    double wanted = vw.density;
    vw.density = 0;
    kp_videowin_set_density(wanted);
    gdk_flush();
    return vw.win;
}

unsigned long kp_videowin_peek(void)
{
    return vw.win;
}
