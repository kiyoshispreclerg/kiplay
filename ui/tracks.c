/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>
#include <string.h>

#include "app.h"
#include "i18n.h"

/* The Video, Audio and Subtitles menus: the playing file's tracks (from
 * KP_EV_TRACKS), the delays and the aspect ratio. What is chosen here goes
 * to the backend now and into app->playing.file, which player.c keeps per
 * file (kp_file_settings), so the file opens the same way next time.
 *
 * Only an engine with KP_CAP_TRACKS (mpv) has any of this; with
 * kiplay-audio the menus show no tracks.
 *
 * Memory: the track list is copied here (the event's is only valid during
 * the callback) and replaced on every update. */

#define MAX_LABEL 60

static const char *const aspects[] = {"", "4:3", "16:9", "1.85:1", "2.35:1", "1:1"};

typedef struct {
    kp_track t;
    char *label;
} track_item;

static GArray *tracks;      /* track_item */
static GtkWidget *track_menus[3]; /* submenu per kp_track_type */
static GtkWidget *aspect_items[G_N_ELEMENTS(aspects)];
static GtkWidget *mute_item;
static GSList *engine_items; /* do nothing without KP_CAP_TRACKS: greyed */
static bool updating;       /* menus being set from the state: no commands */
static int last_sid;        /* to show the subtitles again after hiding them */

static kp_backend *tracks_backend(kp_app *app)
{
    kp_backend *b = app->playing.backend;
    return b && app->playing.loaded && (kp_backend_caps(b) & KP_CAP_TRACKS) ? b : NULL;
}

/* "2: English (en) — ass", short enough for a menu. */
static char *track_label(const kp_track *t)
{
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "%d", t->id);
    /* An external file's title is its whole file name: the language says it. */
    const char *title = t->title && !(t->external && t->lang) ? t->title : NULL;
    if (title)
        g_string_append_printf(s, ": %s", title);
    if (t->lang)
        g_string_append_printf(s, title ? " (%s)" : ": %s", t->lang);
    if (t->external)
        g_string_append_printf(s, " %s", _("(file)"));
    if (t->codec)
        g_string_append_printf(s, " — %s", t->codec);
    if (g_utf8_strlen(s->str, -1) > MAX_LABEL) {
        char *end = g_utf8_offset_to_pointer(s->str, MAX_LABEL - 1);
        g_string_truncate(s, (gsize)(end - s->str));
        g_string_append(s, "…");
    }
    return g_string_free(s, FALSE);
}

static void clear_tracks(void)
{
    if (!tracks)
        tracks = g_array_new(FALSE, FALSE, sizeof(track_item));
    for (guint i = 0; i < tracks->len; i++) {
        track_item *it = &g_array_index(tracks, track_item, i);
        g_free((char *)it->t.title);
        g_free((char *)it->t.lang);
        g_free((char *)it->t.codec);
        g_free(it->label);
    }
    g_array_set_size(tracks, 0);
}

/* The selected track of a type; 0 if none. */
static int selected(kp_track_type type)
{
    for (guint i = 0; tracks && i < tracks->len; i++) {
        const kp_track *t = &g_array_index(tracks, track_item, i).t;
        if (t->type == type && t->selected)
            return t->id;
    }
    return 0;
}

static void set_track(kp_app *app, kp_track_type type, int id)
{
    kp_backend *b = tracks_backend(app);
    if (!b)
        return;
    kp_backend_set_track(b, type, id);
    if (type == KP_TRACK_AUDIO)
        app->playing.file.aid = id;
    else if (type == KP_TRACK_SUB)
        app->playing.file.sid = id;
    if (type == KP_TRACK_SUB && id > 0)
        last_sid = id;
}

static void on_track(GtkCheckMenuItem *it, gpointer data)
{
    if (updating || !gtk_check_menu_item_get_active(it))
        return;
    set_track(data, (kp_track_type)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(it), "kp-type")),
              GPOINTER_TO_INT(g_object_get_data(G_OBJECT(it), "kp-id")));
}

static GtkWidget *radio(GSList **group, const char *label, kp_track_type type, int id,
                        bool active, kp_app *app)
{
    GtkWidget *it = gtk_radio_menu_item_new_with_label(*group, label);
    *group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(it));
    g_object_set_data(G_OBJECT(it), "kp-type", GINT_TO_POINTER(type));
    g_object_set_data(G_OBJECT(it), "kp-id", GINT_TO_POINTER(id));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(it), active);
    g_signal_connect(it, "toggled", G_CALLBACK(on_track), app);
    return it;
}

static void rebuild(kp_app *app, kp_track_type type)
{
    GtkWidget *menu = track_menus[type];
    GList *children = gtk_container_get_children(GTK_CONTAINER(menu));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(children);

    updating = true;
    GSList *group = NULL;
    int n = 0, sel = selected(type);
    if (type == KP_TRACK_SUB && tracks_backend(app))
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              radio(&group, _("None"), type, 0, sel == 0, app));
    for (guint i = 0; tracks && i < tracks->len; i++) {
        const track_item *ti = &g_array_index(tracks, track_item, i);
        if (ti->t.type != type || ti->t.albumart)
            continue;
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              radio(&group, ti->label, type, ti->t.id, ti->t.selected, app));
        n++;
    }
    if (n == 0) {
        GtkWidget *none = gtk_menu_item_new_with_label(_("No tracks"));
        gtk_widget_set_sensitive(none, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), none);
    }
    gtk_widget_show_all(menu);
    updating = false;
}

void kp_tracks_update(kp_app *app, const kp_track *t, int n)
{
    clear_tracks();
    for (int i = 0; i < n; i++) {
        track_item it = {t[i], track_label(&t[i])};
        it.t.title = g_strdup(t[i].title);
        it.t.lang = g_strdup(t[i].lang);
        it.t.codec = g_strdup(t[i].codec);
        g_array_append_val(tracks, it);
    }
    int sid = selected(KP_TRACK_SUB);
    if (sid)
        last_sid = sid;
    kp_tracks_refresh(app);
}

void kp_tracks_refresh(kp_app *app)
{
    for (int type = 0; type < 3; type++)
        if (track_menus[type])
            rebuild(app, (kp_track_type)type);
    bool can = tracks_backend(app) != NULL;
    for (GSList *l = engine_items; l; l = l->next)
        gtk_widget_set_sensitive(l->data, can);
    for (size_t i = 0; i < G_N_ELEMENTS(aspects); i++)
        if (aspect_items[i])
            gtk_widget_set_sensitive(aspect_items[i], can);
}

void kp_tracks_file_changed(kp_app *app)
{
    last_sid = 0;
    kp_tracks_update(app, NULL, 0);
    updating = true;
    for (size_t i = 0; i < G_N_ELEMENTS(aspects); i++)
        if (aspect_items[i])
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(aspect_items[i]),
                                           !strcmp(aspects[i], app->playing.file.aspect));
    updating = false;
}

void kp_tracks_mute_changed(kp_app *app)
{
    if (!mute_item)
        return;
    updating = true;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(mute_item), app->playing.muted);
    updating = false;
}

void kp_tracks_cycle(kp_app *app, kp_track_type type)
{
    if (!tracks_backend(app))
        return;
    /* The tracks of that type, in order; after the last one comes none
     * (subtitles) or the first again. */
    const track_item *list[64];
    int n = 0, at = -1, sel = selected(type);
    for (guint i = 0; tracks && i < tracks->len && n < 64; i++) {
        const track_item *ti = &g_array_index(tracks, track_item, i);
        if (ti->t.type != type || ti->t.albumart)
            continue;
        if (ti->t.id == sel)
            at = n;
        list[n++] = ti;
    }
    if (n == 0)
        return;
    const track_item *next = at + 1 < n ? list[at + 1] : type == KP_TRACK_SUB ? NULL : list[0];
    if ((next ? next->t.id : 0) == sel)
        return;
    set_track(app, type, next ? next->t.id : 0);
    char *msg = g_strdup_printf(type == KP_TRACK_SUB ? _("Subtitles: %s") : _("Audio: %s"),
                                next ? next->label : _("none"));
    kp_player_message(app, msg);
    g_free(msg);
}

void kp_tracks_toggle_subs(kp_app *app)
{
    if (!tracks_backend(app))
        return;
    int sel = selected(KP_TRACK_SUB);
    if (sel) {
        set_track(app, KP_TRACK_SUB, 0);
        kp_player_message(app, _("Subtitles hidden"));
        return;
    }
    int id = last_sid;
    for (guint i = 0; !id && tracks && i < tracks->len; i++)
        if (g_array_index(tracks, track_item, i).t.type == KP_TRACK_SUB)
            id = g_array_index(tracks, track_item, i).t.id;
    if (id) {
        set_track(app, KP_TRACK_SUB, id);
        kp_player_message(app, _("Subtitles shown"));
    }
}

void kp_tracks_delay(kp_app *app, kp_track_type type, double delta)
{
    kp_backend *b = tracks_backend(app);
    if (!b)
        return;
    double *d = type == KP_TRACK_SUB ? &app->playing.file.sub_delay
                                     : &app->playing.file.audio_delay;
    /* Whole milliseconds: 0.1 steps add up to 0.30000000000000004. */
    *d = delta == 0 ? 0 : round((*d + delta) * 1000) / 1000;
    kp_backend_set_delay(b, type, *d);
    char *msg = g_strdup_printf(type == KP_TRACK_SUB ? _("Subtitle delay: %d ms")
                                                     : _("Audio delay: %d ms"),
                                (int)lround(*d * 1000));
    kp_player_message(app, msg);
    g_free(msg);
}

static void set_aspect(kp_app *app, size_t i)
{
    kp_backend *b = tracks_backend(app);
    if (!b)
        return;
    g_strlcpy(app->playing.file.aspect, aspects[i], sizeof app->playing.file.aspect);
    kp_backend_set_aspect(b, aspects[i]);
    char *msg = g_strdup_printf(_("Aspect ratio: %s"), *aspects[i] ? aspects[i] : _("auto"));
    kp_player_message(app, msg);
    g_free(msg);
    updating = true;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(aspect_items[i]), TRUE);
    updating = false;
}

void kp_tracks_cycle_aspect(kp_app *app)
{
    size_t now = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(aspects); i++)
        if (!strcmp(aspects[i], app->playing.file.aspect))
            now = i;
    set_aspect(app, (now + 1) % G_N_ELEMENTS(aspects));
}

/* ---- the menus ----------------------------------------------------------- */

static void on_aspect(GtkCheckMenuItem *it, gpointer data)
{
    if (updating || !gtk_check_menu_item_get_active(it))
        return;
    set_aspect(data, (size_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(it), "kp-value")));
}

static void on_mute(GtkCheckMenuItem *it, gpointer data)
{
    kp_app *app = data;
    if (!updating && gtk_check_menu_item_get_active(it) != app->playing.muted)
        kp_player_toggle_mute(app);
}

static void audio_later(kp_app *app) { kp_tracks_delay(app, KP_TRACK_AUDIO, 0.1); }
static void audio_sooner(kp_app *app) { kp_tracks_delay(app, KP_TRACK_AUDIO, -0.1); }
static void audio_reset(kp_app *app) { kp_tracks_delay(app, KP_TRACK_AUDIO, 0); }
static void sub_later(kp_app *app) { kp_tracks_delay(app, KP_TRACK_SUB, 0.1); }
static void sub_sooner(kp_app *app) { kp_tracks_delay(app, KP_TRACK_SUB, -0.1); }
static void sub_reset(kp_app *app) { kp_tracks_delay(app, KP_TRACK_SUB, 0); }

/* An item for what only an engine with tracks does. */
static GtkWidget *item(GtkWidget *menu, const char *label, void (*cb)(kp_app *), kp_app *app,
                       const char *action)
{
    GtkWidget *it = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect_swapped(it, "activate", G_CALLBACK(cb), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    if (action)
        kp_shortcuts_attach(it, action);
    engine_items = g_slist_prepend(engine_items, it);
    return it;
}

static GtkWidget *track_submenu(GtkWidget *menu, const char *label, kp_track_type type)
{
    GtkWidget *top = gtk_menu_item_new_with_mnemonic(label);
    track_menus[type] = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), track_menus[type]);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), top);
    return top;
}

static GtkWidget *top_menu(GtkWidget *bar, const char *label)
{
    GtkWidget *top = gtk_menu_item_new_with_mnemonic(label);
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), top);
    return menu;
}

static void separator(GtkWidget *menu)
{
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

static void fullscreen(kp_app *app)
{
    kp_layout_set_fullscreen(app, !app->fullscreen);
}

void kp_tracks_menus(kp_app *app, GtkWidget *bar)
{
    GtkWidget *m = top_menu(bar, _("_Video"));
    track_submenu(m, _("_Track"), KP_TRACK_VIDEO);
    GtkWidget *top = gtk_menu_item_new_with_mnemonic(_("_Aspect ratio"));
    GtkWidget *sub = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), top);
    GSList *group = NULL;
    for (size_t i = 0; i < G_N_ELEMENTS(aspects); i++) {
        GtkWidget *it = gtk_radio_menu_item_new_with_mnemonic(group, *aspects[i] ? aspects[i]
                                                                                 : _("_Auto"));
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(it));
        g_object_set_data(G_OBJECT(it), "kp-value", GINT_TO_POINTER((int)i));
        g_signal_connect(it, "toggled", G_CALLBACK(on_aspect), app);
        gtk_menu_shell_append(GTK_MENU_SHELL(sub), it);
        aspect_items[i] = it;
    }
    item(m, _("_Next aspect ratio"), kp_tracks_cycle_aspect, app, "aspect");
    separator(m);
    GtkWidget *full = gtk_menu_item_new_with_mnemonic(_("_Fullscreen"));
    g_signal_connect_swapped(full, "activate", G_CALLBACK(fullscreen), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), full);
    kp_shortcuts_attach(full, "fullscreen");

    m = top_menu(bar, _("_Audio"));
    track_submenu(m, _("_Track"), KP_TRACK_AUDIO);
    separator(m);
    item(m, _("Delay +100 ms"), audio_later, app, "audio_delay_up");
    item(m, _("Delay -100 ms"), audio_sooner, app, "audio_delay_down");
    item(m, _("_No delay"), audio_reset, app, NULL);
    separator(m);
    mute_item = gtk_check_menu_item_new_with_mnemonic(_("_Mute"));
    g_signal_connect(mute_item, "toggled", G_CALLBACK(on_mute), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), mute_item);
    kp_shortcuts_attach(mute_item, "mute");

    m = top_menu(bar, _("_Subtitles"));
    track_submenu(m, _("_Track"), KP_TRACK_SUB);
    item(m, _("_Show / hide"), kp_tracks_toggle_subs, app, "toggle_subtitles");
    separator(m);
    item(m, _("Delay +100 ms"), sub_later, app, "sub_delay_up");
    item(m, _("Delay -100 ms"), sub_sooner, app, "sub_delay_down");
    item(m, _("_No delay"), sub_reset, app, NULL);

    kp_tracks_file_changed(app);
}

void kp_tracks_free(void)
{
    g_slist_free(engine_items);
    engine_items = NULL;
    clear_tracks();
    if (tracks)
        g_array_free(tracks, TRUE);
    tracks = NULL;
}
