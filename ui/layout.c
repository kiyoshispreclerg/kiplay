/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gdk/gdkx.h>

#include "app.h"
#include "log.h"

const char *const kp_video_pos_names[] = {"above", "left", "right", NULL};
const char *const kp_controls_pos_names[] = {"bottom", "top", NULL};

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
    if (app->paned) {
        gtk_widget_destroy(app->paned);
        app->paned = NULL;
    }

    GtkWidget *center = app->notebook;
    if (app->show_video) {
        app->paned = app->video_pos == KP_VIDEO_ABOVE ? gtk_vpaned_new() : gtk_hpaned_new();
        bool video_first = app->video_pos != KP_VIDEO_RIGHT;
        GtkWidget *placeholder = gtk_event_box_new();
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
        gtk_container_remove(GTK_CONTAINER(slot), placeholder);
        gtk_widget_reparent(app->video, slot);
        if (video_first)
            gtk_container_child_set(GTK_CONTAINER(slot), app->video, "resize", TRUE,
                                    "shrink", FALSE, NULL);
        gtk_widget_show(app->video);
    } else {
        gtk_box_pack_start(GTK_BOX(app->body), center, TRUE, TRUE, 0);
        gtk_widget_show(center);
    }

    /* Controls above or below the center; the status line stays last. */
    gtk_box_pack_start(GTK_BOX(app->body), app->controls, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(app->body), app->controls,
                          app->controls_pos == KP_CONTROLS_TOP ? 1 : 2);
    gtk_box_reorder_child(GTK_BOX(app->body), center,
                          app->controls_pos == KP_CONTROLS_TOP ? 2 : 1);
    gtk_widget_show(app->controls);

    if (xid_before && gtk_widget_get_realized(app->video))
        kp_log_debug("layout: video XID %lu -> %lu", xid_before,
                     (unsigned long)GDK_WINDOW_XID(app->video->window));
}
