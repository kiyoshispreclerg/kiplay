/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <X11/Xlib.h>
#include <gdk/gdkx.h>

#include "app.h"
#include "log.h"

const char *const kp_video_pos_names[] = {"above", "left", "right", NULL};
const char *const kp_controls_pos_names[] = {"bottom", "top", NULL};

/* Whether the chat panel is in the layout now. */
static bool chat_in_layout;

/* Takes w out of its parent, keeping it alive. */
static void detach(GtkWidget *w)
{
    GtkWidget *parent = gtk_widget_get_parent(w);
    if (parent)
        gtk_container_remove(GTK_CONTAINER(parent), w);
}

static const char *paned_key(const kp_app *app)
{
    return app->video_pos == KP_VIDEO_ABOVE ? "split_above" : "split_side";
}

void kp_layout_save(kp_app *app)
{
    if (app->paned)
        kp_prefs_set_int(app->prefs, "layout", paned_key(app),
                         gtk_paned_get_position(GTK_PANED(app->paned)));
}

/* mpv 0.34 watches the --wid window and takes any MapNotify it sees there
 * as its own window being mapped. Moving the video area unmaps and maps
 * that window: if mpv had just created its window, it never maps it and
 * the video stays black. The area's children are the backend's windows,
 * and a backend always wants them shown, so they are mapped here. */
static void map_backend_windows(GtkWidget *video)
{
    if (!gtk_widget_get_realized(video))
        return;
    Display *dpy = GDK_WINDOW_XDISPLAY(video->window);
    Window root, parent, *children = NULL;
    unsigned n = 0;
    gdk_error_trap_push();
    if (XQueryTree(dpy, GDK_WINDOW_XID(video->window), &root, &parent, &children, &n)) {
        for (unsigned i = 0; i < n; i++)
            XMapWindow(dpy, children[i]);
        if (children)
            XFree(children);
    }
    gdk_flush();
    gdk_error_trap_pop();
}

static gboolean map_backend_windows_later(gpointer data)
{
    map_backend_windows(data);
    return G_SOURCE_REMOVE;
}

/* Controls right above or below the center (the paned, or the notebook
 * alone). The status line is packed at the end and stays last. */
static void pack_controls(kp_app *app)
{
    GtkWidget *center = app->paned ? app->paned : app->notebook;
    gint pos = 0;
    gtk_container_child_get(GTK_CONTAINER(app->body), center, "position", &pos, NULL);
    gtk_box_pack_start(GTK_BOX(app->body), app->controls, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(app->body), app->controls,
                          app->controls_pos == KP_CONTROLS_TOP ? pos : pos + 1);
    gtk_widget_show(app->controls);
}

void kp_layout_apply(kp_app *app)
{
    unsigned long xid_before = gtk_widget_get_realized(app->video)
                                   ? GDK_WINDOW_XID(app->video->window) : 0;

    /* The video moves with gtk_widget_reparent, which keeps its X window
     * (gtk_container_remove would unrealize it and the backend's --wid
     * would point at nothing). Park it in a hidden holder until its new
     * place exists. */
    static GtkWidget *holder;
    if (!holder) {
        holder = gtk_window_new(GTK_WINDOW_POPUP);
        gtk_widget_realize(holder);
    }
    GtkWidget *parent = gtk_widget_get_parent(app->video);
    if (!parent)
        gtk_container_add(GTK_CONTAINER(holder), app->video);
    else if (parent != holder)
        gtk_widget_reparent(app->video, holder);

    detach(app->notebook);
    detach(app->controls);
    if (app->chat)
        detach(app->chat);
    if (app->paned) {
        gtk_widget_destroy(app->paned);
        app->paned = NULL;
    }

    GtkWidget *center = app->notebook;
    if (app->show_video) {
        app->paned = app->video_pos == KP_VIDEO_ABOVE ? gtk_vpaned_new() : gtk_hpaned_new();
        bool video_first = app->video_pos != KP_VIDEO_RIGHT;
        /* With a chat the video's slot is a box: video, then the panel. */
        bool with_chat = kp_chat_wanted(app);
        GtkWidget *placeholder = with_chat ? gtk_hbox_new(FALSE, 0) : gtk_event_box_new();
        if (video_first) {
            gtk_paned_pack1(GTK_PANED(app->paned), placeholder, TRUE, FALSE);
            gtk_paned_pack2(GTK_PANED(app->paned), app->notebook, FALSE, FALSE);
        } else {
            gtk_paned_pack1(GTK_PANED(app->paned), app->notebook, FALSE, FALSE);
            gtk_paned_pack2(GTK_PANED(app->paned), placeholder, TRUE, FALSE);
        }
        int def = app->video_pos == KP_VIDEO_ABOVE ? 320 : 560;
        gtk_paned_set_position(GTK_PANED(app->paned),
                               kp_prefs_get_int(app->prefs, "layout", paned_key(app), def, 50,
                                                10000));
        center = app->paned;
        /* Swap the placeholder for the video once the paned is realized. */
        gtk_box_pack_start(GTK_BOX(app->body), center, TRUE, TRUE, 0);
        gtk_widget_show_all(center);
        GtkWidget *slot = gtk_widget_get_parent(placeholder);
        GtkWidget *occupant = app->video;
        if (with_chat) {
            gtk_widget_reparent(app->video, placeholder);
            gtk_box_pack_end(GTK_BOX(placeholder), app->chat, FALSE, FALSE, 0);
            gtk_widget_show(app->chat);
            occupant = placeholder;
        } else {
            gtk_container_remove(GTK_CONTAINER(slot), placeholder);
            gtk_widget_reparent(app->video, slot);
        }
        chat_in_layout = with_chat;
        if (video_first)
            gtk_container_child_set(GTK_CONTAINER(slot), occupant, "resize", TRUE,
                                    "shrink", FALSE, NULL);
        gtk_widget_show(app->video);
        /* Now, and once more for a window mpv was creating meanwhile. */
        map_backend_windows(app->video);
        g_timeout_add_full(G_PRIORITY_DEFAULT, 250, map_backend_windows_later,
                           g_object_ref(app->video), g_object_unref);
    } else {
        chat_in_layout = false;
        gtk_box_pack_start(GTK_BOX(app->body), center, TRUE, TRUE, 0);
        gtk_widget_show(center);
    }

    if (app->fullscreen)
        kp_fsbar_attach(app);
    else
        pack_controls(app);
    if (app->compact || app->fullscreen || !app->playlists_shown)
        gtk_widget_hide(app->notebook);

    if (xid_before && gtk_widget_get_realized(app->video))
        kp_log_debug("layout: video XID %lu -> %lu", xid_before,
                     (unsigned long)GDK_WINDOW_XID(app->video->window));
}

void kp_layout_update_chat(kp_app *app)
{
    if (!app->show_video || kp_chat_wanted(app) == chat_in_layout)
        return;
    kp_layout_save(app);
    kp_layout_apply(app);
}

void kp_layout_update_video(kp_app *app)
{
    bool want = app->video_forced || app->playing.has_video || app->fullscreen;
    if (want == app->show_video) {
        kp_layout_update_chat(app);
        return;
    }
    kp_layout_save(app);
    app->show_video = want;
    kp_layout_apply(app);
    if (app->compact && !want) {
        int w, h;
        gtk_window_get_size(GTK_WINDOW(app->window), &w, &h);
        gtk_window_resize(GTK_WINDOW(app->window), w, 1);
    }
}

/* Fullscreen hides all but the video (the controls float, fsbar.c),
 * compact mode all but the video and the controls; the paned gives the video all the room when the
 * other side is hidden. */
static void show_parts(kp_app *app)
{
    bool others = !app->fullscreen && !app->compact;
    gtk_widget_set_visible(app->menubar, others);
    gtk_widget_set_visible(app->notebook, others && app->playlists_shown);
    gtk_widget_set_visible(app->status, others);
}

void kp_layout_set_fullscreen(kp_app *app, bool on)
{
    if (on == app->fullscreen)
        return;
    app->fullscreen = on;
    if (!on)
        kp_fsbar_detach(app);
    kp_layout_update_video(app); /* may lay it all out again */
    if (on)
        kp_fsbar_attach(app);
    else if (!gtk_widget_get_parent(app->controls))
        pack_controls(app);
    show_parts(app);
    if (on)
        gtk_window_fullscreen(GTK_WINDOW(app->window));
    else
        gtk_window_unfullscreen(GTK_WINDOW(app->window));
}

void kp_layout_set_compact(kp_app *app, bool on)
{
    static int height; /* the window's before, to come back to */
    if (on == app->compact)
        return;
    app->compact = on;
    int w, h;
    gtk_window_get_size(GTK_WINDOW(app->window), &w, &h);
    if (on)
        height = h;
    show_parts(app);
    /* Without a video only the controls are left: the window shrinks to
     * them (GTK takes 1 as "as small as it can be"). */
    if (on && !app->show_video)
        gtk_window_resize(GTK_WINDOW(app->window), w, 1);
    else if (!on && height > 0 && !app->show_video)
        gtk_window_resize(GTK_WINDOW(app->window), w, height);
}

void kp_layout_set_playlists(kp_app *app, bool on)
{
    static int height; /* the window's before, to come back to */
    if (on == app->playlists_shown)
        return;
    app->playlists_shown = on;
    int w, h;
    gtk_window_get_size(GTK_WINDOW(app->window), &w, &h);
    if (!on)
        height = h;
    show_parts(app);
    /* Without a video only the controls are left: the window shrinks to
     * them, and grows back (GTK takes 1 as "as small as it can be"). */
    if (app->fullscreen || app->compact || app->show_video)
        return;
    if (!on)
        gtk_window_resize(GTK_WINDOW(app->window), w, 1);
    else if (height > 0)
        gtk_window_resize(GTK_WINDOW(app->window), w, height);
}
