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
 * The density is the one the toolkit (gtk2-ng) renders the main window at
 * and announces there, _X_DENSITY_SCALE [num, den]. For the compositor to
 * sample the video window, we announce it on the main window too:
 *
 *   _X_DENSITY_SOURCES  CARDINAL[5n]  [x, y, width, height, window]...
 *
 * "In this rectangle (the main window's logical coordinates) show this
 * redirected window, rendered at _X_DENSITY_SCALE: its pixel (0, 0) at the
 * rectangle's corner, clipped to it." The compositor follows its contents
 * by Damage on that window, as with any window. It is written once the
 * window has been drawn at a new size or density (contents first,
 * announcement after) and deleted when the video isn't dense or shown.
 *
 * It selects no input, so clicks and the wheel go on to the area, as they
 * did when mpv's window sat right in it. The X server destroys it (and
 * the damage and pictures on it) with the area's window; we only forget
 * the XIDs then. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xatom.h>
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
    kp_app *app;
    GtkWidget *video;
    Window win;
    int w, h;       /* the window's size now: the area's, times the density */
    double density; /* 0: not redirected */
    bool usable;    /* the server has Composite and Damage */
    bool forced;    /* KP_VIDEO_DENSITY: the main window's density is ignored */
    int damage_event;
    Damage damage;
    Picture src, dst; /* the window, the area */
    guint copy_idle;
    Atom scale_atom, sources_atom;
    bool announce;  /* write _X_DENSITY_SOURCES after the next frame */
    bool announced; /* it is on the main window */
    long sources[5];
} vw;

static Display *dpy(void)
{
    return GDK_WINDOW_XDISPLAY(vw.video->window);
}

static Window main_xid(void)
{
    GdkWindow *w = vw.app->window->window;
    return w ? GDK_WINDOW_XID(w) : None;
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

/* _X_DENSITY_SOURCES: the area and the window while dense and shown,
 * nothing otherwise. */
static void update_sources(void)
{
    Window top = main_xid();
    if (!top)
        return;
    Display *d = dpy();
    /* Not while layout.c parks the area in its hidden holder, nor in its
     * new place before it gets a size there (GTK's 1x1 at -1, -1). */
    const GtkAllocation *a = &vw.video->allocation;
    bool wanted = vw.damage && gtk_widget_get_mapped(vw.video) &&
                  gtk_widget_get_toplevel(vw.video) == vw.app->window && a->width > 1 &&
                  a->height > 1;
    long v[5] = {0};
    if (wanted) {
        int x, y;
        Window child;
        if (!XTranslateCoordinates(d, GDK_WINDOW_XID(vw.video->window), top, 0, 0, &x, &y,
                                   &child))
            wanted = false;
        v[0] = x;
        v[1] = y;
        v[2] = a->width;
        v[3] = a->height;
        v[4] = (long)vw.win;
    }
    if (!wanted) {
        if (vw.announced)
            XDeleteProperty(d, top, vw.sources_atom);
        vw.announced = false;
        return;
    }
    if (vw.announced && memcmp(v, vw.sources, sizeof v) == 0)
        return;
    XChangeProperty(d, top, vw.sources_atom, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)v, 5);
    memcpy(vw.sources, v, sizeof v);
    vw.announced = true;
    kp_log_debug("videowin: sources %ld,%ld %ldx%ld 0x%lx", v[0], v[1], v[2], v[3],
                 (unsigned long)v[4]);
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

/* Whether the window changed size. */
static bool fit(void)
{
    if (!vw.win || !gtk_widget_get_realized(vw.video))
        return false;
    double s = vw.density > 0 ? vw.density : 1;
    int w = MAX((int)ceil(vw.video->allocation.width * s), 1);
    int h = MAX((int)ceil(vw.video->allocation.height * s), 1);
    if (w == vw.w && h == vw.h)
        return false;
    vw.w = w;
    vw.h = h;
    XResizeWindow(dpy(), vw.win, (unsigned)w, (unsigned)h);
    return true;
}

static void on_allocate(GtkWidget *video, GtkAllocation *a, gpointer data)
{
    (void)video;
    (void)a;
    (void)data;
    /* A new size is announced once mpv has drawn at it; a move right
     * away. */
    if (fit() && vw.damage)
        vw.announce = true;
    else if (!vw.announce)
        update_sources();
}

static void on_map_changed(GtkWidget *video, gpointer data)
{
    (void)video;
    (void)data;
    if (vw.win)
        update_sources();
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
    vw.w = vw.h = 0;
    vw.density = 0;
    vw.announce = false;
    if (vw.copy_idle) {
        g_source_remove(vw.copy_idle);
        vw.copy_idle = 0;
    }
    if (vw.announced && main_xid())
        XDeleteProperty(dpy(), main_xid(), vw.sources_atom);
    vw.announced = false;
}

/* _X_DENSITY_SCALE on the main window, 0 when absent or 1/1. */
static double main_density(void)
{
    Window top = main_xid();
    if (!top)
        return 0;
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    double s = 0;
    gdk_error_trap_push();
    if (XGetWindowProperty(dpy(), top, vw.scale_atom, 0, 2, False, XA_CARDINAL, &type, &format,
                           &n, &after, &data) == Success &&
        type == XA_CARDINAL && format == 32 && n == 2) {
        /* format 32 comes back as longs */
        long num = ((long *)data)[0], den = ((long *)data)[1];
        if (num > den && den > 0)
            s = (double)num / (double)den;
    }
    if (data)
        XFree(data);
    (void)gdk_error_trap_pop();
    return s;
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
        if (vw.announce) {
            vw.announce = false;
            update_sources();
        }
        return GDK_FILTER_REMOVE;
    }
    if (e->type == PropertyNotify && !vw.forced && vw.win && e->xproperty.atom == vw.scale_atom &&
        e->xproperty.window == main_xid())
        kp_videowin_set_density(main_density());
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
    if (vw.damage) {
        copy();
        vw.announce = true; /* once mpv has drawn at the new size */
    } else {
        vw.announce = false;
        update_sources(); /* gone */
    }
    gdk_flush();
    kp_log_debug("videowin: density %g, %dx%d", vw.density, vw.w, vw.h);
    /* The chat drawn over the video in fullscreen is in this window's pixels. */
    if (vw.app)
        kp_chat_overlay_update(vw.app);
}

double kp_videowin_scale(void)
{
    return vw.win && vw.density > 0 ? vw.density : 1.0;
}

unsigned long kp_videowin_xid(kp_app *app)
{
    if (vw.win)
        return vw.win;
    vw.app = app;
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
        vw.scale_atom = XInternAtom(d, "_X_DENSITY_SCALE", False);
        vw.sources_atom = XInternAtom(d, "_X_DENSITY_SOURCES", False);
        g_signal_connect_after(vw.video, "size-allocate", G_CALLBACK(on_allocate), NULL);
        g_signal_connect_after(vw.video, "expose-event", G_CALLBACK(on_expose), NULL);
        g_signal_connect_after(vw.video, "map", G_CALLBACK(on_map_changed), NULL);
        g_signal_connect_after(vw.video, "unmap", G_CALLBACK(on_map_changed), NULL);
        g_signal_connect(vw.video, "unrealize", G_CALLBACK(on_unrealize), NULL);
        gdk_window_add_filter(NULL, filter, NULL);
        /* KP_VIDEO_DENSITY=S: redirected at density S, whatever the main
         * window is at, to try it without a compositor asking (1:
         * redirected at the area's size). */
        const char *env = g_getenv("KP_VIDEO_DENSITY");
        if (env && *env) {
            vw.forced = true;
            vw.density = g_ascii_strtod(env, NULL);
        }
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

    double wanted = vw.forced ? vw.density : main_density();
    vw.density = 0;
    kp_videowin_set_density(wanted);
    gdk_flush();
    return vw.win;
}

unsigned long kp_videowin_peek(void)
{
    return vw.win;
}
