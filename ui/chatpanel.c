/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>
#include <string.h>

#include "app.h"
#include "chatlog.h"
#include "i18n.h"
#include "log.h"
#include "sidecar.h"

/* The recorded live chat as a panel beside the video: the messages whose
 * time has come, newest at the bottom, drawn with Pango and Cairo (channel
 * emoji inline as pictures, Super Chats and new members as colored cards,
 * a round avatar). It follows the player's clock; a seek (or a jump of the
 * clock) starts again from the index, taking the last messages before the
 * new time.
 *
 * Memory: the panel owns the log (a mapping and an index), and the last
 * MAX_ITEMS messages decoded, nothing more. */

#define MAX_ITEMS 300
#define BACKFILL 120      /* lines taken before the time on a (re)start */
#define JUMP_MS 15000     /* a clock jump this big is a seek */
#define AVATAR_PX 24
#define PAD 6
/* New messages and pictures are drawn at most this often, on a steady
 * beat: a redraw per message lands at random times next to the video's
 * frames, and a compositor that has to blend the two (the panel and mpv's
 * window share the screen) shows the video's frames out of time. */
#define REDRAW_MS 250

typedef struct {
    kp_app *app;
    GtkWidget *area;
    kp_chatlog *log;
    GCancellable *opening;
    char *path;          /* the chat file being read */
    GPtrArray *items;    /* kp_chat_item, oldest first */
    size_t cursor;       /* next line to decode */
    int64_t last_ms;
    bool have_time;
    int filter;          /* kp_chat_item_shown */
    int width;
    guint redraw_timer;
    gint64 last_redraw_us; /* when the last paced redraw was asked */
} panel;

static panel *the_panel;

/* ---- redrawing ------------------------------------------------------------ */

static gboolean redraw_now(gpointer data)
{
    panel *p = data;
    p->redraw_timer = 0;
    p->last_redraw_us = g_get_monotonic_time();
    gtk_widget_queue_draw(p->area);
    return G_SOURCE_REMOVE;
}

/* Something changed: drawn on the next beat (REDRAW_MS after the last
 * one), however many changes come before it. */
static void want_redraw(panel *p)
{
    if (p->redraw_timer)
        return;
    gint64 wait = p->last_redraw_us + REDRAW_MS * 1000 - g_get_monotonic_time();
    p->redraw_timer = g_timeout_add(wait > 0 ? (guint)(wait / 1000) : 0, redraw_now, p);
}

/* ---- the timeline --------------------------------------------------------- */

static void clear_items(panel *p)
{
    g_ptr_array_set_size(p->items, 0);
}

static void trim(panel *p)
{
    if (p->items->len > MAX_ITEMS)
        g_ptr_array_remove_range(p->items, 0, p->items->len - MAX_ITEMS);
}

/* Decodes lines [from, to) into the items (what the filter lets pass). */
static void take(panel *p, size_t from, size_t to)
{
    GPtrArray *got = g_ptr_array_new();
    for (size_t i = from; i < to; i++)
        kp_chatlog_decode(p->log, i, got);
    for (guint i = 0; i < got->len; i++) {
        kp_chat_item *it = got->pdata[i];
        if (kp_chat_item_shown(it, p->filter))
            g_ptr_array_add(p->items, it);
        else
            kp_chat_item_free(it);
    }
    g_ptr_array_free(got, TRUE);
}

/* The panel at ms: the next messages, or from the index after a seek. */
static void advance(panel *p, int64_t ms)
{
    if (!p->log)
        return;
    size_t n = kp_chatlog_count(p->log);
    if (!p->have_time || ms < p->last_ms || ms - p->last_ms > JUMP_MS) {
        clear_items(p);
        size_t end = kp_chatlog_after(p->log, ms);
        take(p, end > BACKFILL ? end - BACKFILL : 0, end);
        p->cursor = end;
    } else {
        size_t end = p->cursor;
        while (end < n && kp_chatlog_time(p->log, end) <= ms)
            end++;
        if (end == p->cursor)
            return;
        take(p, p->cursor, end);
        p->cursor = end;
    }
    p->have_time = true;
    p->last_ms = ms;
    trim(p);
    want_redraw(p);
}

void kp_chat_set_time(kp_app *app, double seconds)
{
    panel *p = the_panel;
    if (!p || p->app != app)
        return;
    int64_t ms = (int64_t)(seconds * 1000.0);
    if (p->have_time && ms == p->last_ms)
        return;
    advance(p, ms);
}

/* ---- drawing -------------------------------------------------------------- */

typedef struct {
    const char *url; /* the picture, owned by the item */
    int size;
} emote_ref;

/* Pango calls this for each emoji placeholder, with the current point at
 * the shape's origin (on the baseline). */
static void draw_emote(cairo_t *cr, PangoAttrShape *attr, gboolean do_path, gpointer data)
{
    (void)data;
    if (do_path)
        return;
    const emote_ref *r = attr->data;
    GdkPixbuf *pb = kp_img_get(r->url);
    if (!pb)
        return;
    double x, y;
    cairo_get_current_point(cr, &x, &y);
    cairo_save(cr);
    cairo_translate(cr, x, y - r->size * 0.8);
    cairo_scale(cr, (double)r->size / gdk_pixbuf_get_width(pb),
                (double)r->size / gdk_pixbuf_get_height(pb));
    gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

typedef struct {
    GtkWidget *w;
    int line_h;
    GPtrArray *refs; /* emote_ref, freed after the frame */
} frame;

static void set_color(PangoAttrList *l, guint start, guint end, const GdkColor *c, bool bold)
{
    PangoAttribute *a = pango_attr_foreground_new(c->red, c->green, c->blue);
    a->start_index = start;
    a->end_index = end;
    pango_attr_list_insert(l, a);
    if (bold) {
        a = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
        a->start_index = start;
        a->end_index = end;
        pango_attr_list_insert(l, a);
    }
}

static void gdk_color_rgb(GdkColor *c, unsigned rgb)
{
    c->red = (guint16)(((rgb >> 16) & 0xFF) * 257);
    c->green = (guint16)(((rgb >> 8) & 0xFF) * 257);
    c->blue = (guint16)((rgb & 0xFF) * 257);
}

/* A message as a layout: "name text", with the channel emoji as pictures. */
static PangoLayout *build(cairo_t *cr, frame *f, const kp_chat_item *it, int width,
                          const GdkColor *text_color)
{
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_cairo_context_set_shape_renderer(pango_layout_get_context(layout), draw_emote, NULL,
                                           NULL);
    pango_layout_set_font_description(layout, f->w->style->font_desc);
    pango_layout_set_width(layout, width * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);

    GString *s = g_string_new(NULL);
    PangoAttrList *attrs = pango_attr_list_new();
    GdkColor c;
    bool card = it->kind != KP_CHAT_TEXT;
    if (it->kind == KP_CHAT_PAID && it->amount) {
        g_string_append_printf(s, "%s  ", it->amount);
        set_color(attrs, 0, (guint)s->len, text_color, true);
    }
    guint name_start = (guint)s->len;
    g_string_append(s, it->name);
    if (card) {
        c = *text_color;
    } else if (it->badges & KP_CHAT_OWNER) {
        gdk_color_rgb(&c, 0xB8860B);
    } else if (it->badges & KP_CHAT_MOD) {
        gdk_color_rgb(&c, 0x3A5FCD);
    } else if (it->badges & KP_CHAT_MEMBERBADGE) {
        gdk_color_rgb(&c, 0x1E8E3E);
    } else {
        gdk_color_rgb(&c, 0x707070);
    }
    set_color(attrs, name_start, (guint)s->len, &c, true);
    g_string_append(s, card ? "\n" : "  ");

    /* The text, each emote's :name: swapped for an object replacement
     * character that Pango hands to draw_emote. */
    size_t at = 0;
    for (int i = 0; i < it->n_emotes; i++) {
        const kp_chat_emote *e = &it->emotes[i];
        if (e->start < at || e->start + e->len > strlen(it->text))
            continue;
        g_string_append_len(s, it->text + at, (gssize)(e->start - at));
        guint start = (guint)s->len;
        g_string_append(s, "\xEF\xBF\xBC");
        emote_ref *r = g_new0(emote_ref, 1);
        r->url = e->url;
        r->size = f->line_h;
        g_ptr_array_add(f->refs, r);
        PangoRectangle rect = {0, (int)(-f->line_h * 0.8 * PANGO_SCALE),
                               f->line_h * PANGO_SCALE, f->line_h * PANGO_SCALE};
        PangoAttribute *a = pango_attr_shape_new_with_data(&rect, &rect, r, NULL, NULL);
        a->start_index = start;
        a->end_index = (guint)s->len;
        pango_attr_list_insert(attrs, a);
        at = e->start + e->len;
    }
    g_string_append(s, it->text + at);

    pango_layout_set_text(layout, s->str, (int)s->len);
    pango_layout_set_attributes(layout, attrs);
    pango_attr_list_unref(attrs);
    g_string_free(s, TRUE);
    return layout;
}

static void circle_picture(cairo_t *cr, GdkPixbuf *pb, double x, double y, int size)
{
    cairo_save(cr);
    cairo_arc(cr, x + size / 2.0, y + size / 2.0, size / 2.0, 0, 2 * M_PI);
    if (pb) {
        cairo_clip(cr);
        cairo_translate(cr, x, y);
        cairo_scale(cr, (double)size / gdk_pixbuf_get_width(pb),
                    (double)size / gdk_pixbuf_get_height(pb));
        gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
        cairo_paint(cr);
    } else {
        cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.35);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

static gboolean on_expose(GtkWidget *w, GdkEventExpose *ev, gpointer data)
{
    panel *p = data;
    (void)ev;
    cairo_t *cr = gdk_cairo_create(w->window);
    int width = w->allocation.width, height = w->allocation.height;
    gdk_cairo_set_source_color(cr, &w->style->base[GTK_STATE_NORMAL]);
    cairo_paint(cr);

    frame f = {.w = w, .refs = g_ptr_array_new_with_free_func(g_free)};
    PangoLayout *probe = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(probe, w->style->font_desc);
    pango_layout_set_text(probe, "Ag", -1);
    int tw;
    pango_layout_get_pixel_size(probe, &tw, &f.line_h);
    g_object_unref(probe);

    int text_x = PAD + AVATAR_PX + PAD;
    int text_w = width - text_x - PAD;
    if (text_w > 40) {
        double y = height - PAD;
        for (guint i = p->items->len; i > 0 && y > 0; i--) {
            const kp_chat_item *it = p->items->pdata[i - 1];
            bool card = it->kind != KP_CHAT_TEXT;
            GdkColor fg = w->style->text[GTK_STATE_NORMAL];
            double bg_r = 0, bg_g = 0, bg_b = 0;
            if (card) {
                unsigned rgb = it->kind == KP_CHAT_PAID ? 0xE8A317 : 0x0F9D58;
                bg_r = ((rgb >> 16) & 0xFF) / 255.0;
                bg_g = ((rgb >> 8) & 0xFF) / 255.0;
                bg_b = (rgb & 0xFF) / 255.0;
                gdk_color_rgb(&fg, it->kind == KP_CHAT_PAID ? 0x1A1A1A : 0xFFFFFF);
            }
            PangoLayout *layout = build(cr, &f, it, text_w - (card ? 2 * PAD : 0), &fg);
            int lw, lh;
            pango_layout_get_pixel_size(layout, &lw, &lh);
            int box_h = card ? lh + 2 * PAD : lh;
            if (box_h < AVATAR_PX)
                box_h = AVATAR_PX;
            y -= box_h;
            if (card) {
                cairo_set_source_rgb(cr, bg_r, bg_g, bg_b);
                cairo_rectangle(cr, text_x - PAD / 2.0, y, text_w + PAD, box_h);
                cairo_fill(cr);
            }
            circle_picture(cr, it->avatar ? kp_img_get(it->avatar) : NULL, PAD, y, AVATAR_PX);
            gdk_cairo_set_source_color(cr, card ? &fg : &w->style->text[GTK_STATE_NORMAL]);
            cairo_move_to(cr, text_x + (card ? PAD / 2.0 : 0), y + (card ? PAD : 0));
            pango_cairo_show_layout(cr, layout);
            g_object_unref(layout);
            y -= 4;
        }
    }
    g_ptr_array_free(f.refs, TRUE);
    cairo_destroy(cr);
    return TRUE;
}

static void on_picture(void *ud)
{
    want_redraw(ud);
}

/* ---- the file ------------------------------------------------------------- */

bool kp_chat_wanted(const kp_app *app)
{
    return app->chat && app->chat_available && app->chat_shown;
}

static void update_item(kp_app *app)
{
    if (app->chat_item)
        gtk_widget_set_sensitive(app->chat_item, app->chat_available);
}

static void close_log(panel *p)
{
    if (p->opening) {
        g_cancellable_cancel(p->opening);
        g_clear_object(&p->opening);
    }
    kp_chatlog_free(p->log);
    p->log = NULL;
    g_free(p->path);
    p->path = NULL;
    clear_items(p);
    p->have_time = false;
    p->cursor = 0;
}

static void opened(kp_chatlog *log, const char *err, void *ud)
{
    panel *p = ud;
    /* A cancelled or older open: the log is not wanted. */
    if (!log || !p->opening || g_cancellable_is_cancelled(p->opening)) {
        if (!log && err && p->opening && !g_cancellable_is_cancelled(p->opening))
            kp_log_warn(_("cannot read the chat %s: %s"), p->path ? p->path : "", err);
        kp_chatlog_free(log);
        return;
    }
    g_clear_object(&p->opening);
    p->log = log;
    p->app->chat_available = kp_chatlog_count(log) > 0;
    kp_log_debug("chat: %zu lines with messages", kp_chatlog_count(log));
    update_item(p->app);
    if (p->app->chat_available) {
        advance(p, (int64_t)(p->app->playing.position * 1000.0));
        kp_layout_update_chat(p->app);
    }
}

void kp_chat_file_changed(kp_app *app, const char *path)
{
    panel *p = the_panel;
    if (!p)
        return;
    close_log(p);
    bool was = app->chat_available;
    app->chat_available = false;
    update_item(app);
    if (was)
        kp_layout_update_chat(app);
    if (!path)
        return;
    GPtrArray *found = kp_sidecars_find(path);
    for (guint i = 0; i < found->len; i++) {
        const kp_sidecar *sc = g_ptr_array_index(found, i);
        if (sc->kind == KP_SIDECAR_CHAT) {
            p->path = g_strdup(sc->path);
            p->opening = g_cancellable_new();
            kp_chatlog_open_async(sc->path, p->opening, opened, p);
        }
    }
    g_ptr_array_free(found, TRUE);
}

/* ---- the panel ------------------------------------------------------------- */

void kp_chat_prefs_changed(kp_app *app)
{
    panel *p = the_panel;
    if (!p)
        return;
    p->width = kp_prefs_get_int(app->prefs, "chat", "width", 320, 160, 1000);
    static const char *const filters[] = {"all", "paid", "members", NULL};
    int filter = kp_prefs_get_choice(app->prefs, "chat", "filter", filters, 0);
    gtk_widget_set_size_request(p->area, p->width, -1);
    bool show = kp_prefs_get_bool(app->prefs, "chat", "show", true);
    if (show != app->chat_shown && app->chat_item)
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app->chat_item), show);
    if (filter != p->filter) {
        p->filter = filter;
        /* What is on screen was chosen with the old filter. */
        p->have_time = false;
        if (p->log)
            advance(p, (int64_t)(app->playing.position * 1000.0));
    }
}

void kp_chat_toggle(kp_app *app)
{
    if (app->chat_item)
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(app->chat_item), !app->chat_shown);
}

void kp_chat_init(kp_app *app)
{
    panel *p = g_new0(panel, 1);
    p->app = app;
    p->items = g_ptr_array_new_with_free_func((GDestroyNotify)kp_chat_item_free);
    p->area = gtk_drawing_area_new();
    g_signal_connect(p->area, "expose-event", G_CALLBACK(on_expose), p);
    app->chat = p->area;
    g_object_ref_sink(app->chat); /* layout.c takes it in and out of containers */
    app->chat_shown = kp_prefs_get_bool(app->prefs, "chat", "show", true);
    the_panel = p;
    kp_img_init(on_picture, p);
    kp_chat_prefs_changed(app);
}
