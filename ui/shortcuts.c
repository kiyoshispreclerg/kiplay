/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gdk/gdkkeysyms.h>
#include <string.h>

#include "app.h"
#include "i18n.h"
#include "log.h"

/* Every keyboard command of the window, with up to two keys each, kept in
 * kiplay.ini as GTK accelerator names:
 *
 *   [shortcuts]
 *   next = <Control>Right,greater
 *   mute =                         (no key)
 *
 * Only keys that differ from the defaults are written, so new defaults
 * reach whoever never changed them. Keys go through the window's
 * key-press handler, not through a GtkAccelGroup: plain keys (Space, Q,
 * arrows) have to work too, and a GtkAccelGroup would take them from text
 * fields. The menus only show the first key of their command.
 *
 * Fixed and not listed: the media keys, and Alt+1..9 for the tabs. */

#define KEYS 2

typedef struct {
    guint key;
    GdkModifierType mods;
} binding;

typedef struct {
    const char *name;     /* the key in [shortcuts]; append only */
    const char *label;    /* N_() */
    const char *defaults; /* up to KEYS accelerator names, comma-separated */
    void (*run)(kp_app *app);
    bool in_list;         /* only while the playlist has the focus */
} action;

static void seek_fwd(kp_app *app) { kp_player_seek_relative(app, 5); }
static void seek_back(kp_app *app) { kp_player_seek_relative(app, -5); }
static void seek_fwd_long(kp_app *app) { kp_player_seek_relative(app, 60); }
static void seek_back_long(kp_app *app) { kp_player_seek_relative(app, -60); }
static void volume_up(kp_app *app) { kp_controls_change_volume(app, 5); }
static void volume_down(kp_app *app) { kp_controls_change_volume(app, -5); }
static void move_up(kp_app *app) { kp_playlists_move_selected(app, -1); }
static void move_down(kp_app *app) { kp_playlists_move_selected(app, 1); }
static void next_audio(kp_app *app) { kp_tracks_cycle(app, KP_TRACK_AUDIO); }
static void next_sub(kp_app *app) { kp_tracks_cycle(app, KP_TRACK_SUB); }
static void audio_later(kp_app *app) { kp_tracks_delay(app, KP_TRACK_AUDIO, 0.1); }
static void audio_sooner(kp_app *app) { kp_tracks_delay(app, KP_TRACK_AUDIO, -0.1); }
static void sub_later(kp_app *app) { kp_tracks_delay(app, KP_TRACK_SUB, 0.1); }
static void sub_sooner(kp_app *app) { kp_tracks_delay(app, KP_TRACK_SUB, -0.1); }
static void fullscreen(kp_app *app) { kp_layout_set_fullscreen(app, !app->fullscreen); }

static void toggle(GtkWidget *w)
{
    if (GTK_IS_CHECK_MENU_ITEM(w))
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(w),
                                       !gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(w)));
    else if (GTK_IS_TOGGLE_BUTTON(w))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w),
                                     !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)));
}

static void stop_after(kp_app *app) { toggle(app->stop_after_item); }
static void repeat(kp_app *app) { toggle(app->repeat_btn); }
static void shuffle(kp_app *app) { toggle(app->shuffle_btn); }
static void compact(kp_app *app) { toggle(app->compact_item); }
static void video_area(kp_app *app) { toggle(app->show_video_item); }
static void chat(kp_app *app) { toggle(app->chat_item); }
static void playlists(kp_app *app) { toggle(app->playlists_item); }

static const action actions[] = {
    {"play_pause", N_("Play / Pause"), "space", kp_player_toggle_pause, false},
    {"stop", N_("Stop"), "", kp_player_stop, false},
    {"previous", N_("Previous"), "<Control>Left,less", kp_player_prev, false},
    {"next", N_("Next"), "<Control>Right,greater", kp_player_next, false},
    {"seek_forward", N_("Forward 5 s"), "Right", seek_fwd, false},
    {"seek_backward", N_("Back 5 s"), "Left", seek_back, false},
    {"seek_forward_long", N_("Forward 1 min"), "<Shift>Right", seek_fwd_long, false},
    {"seek_backward_long", N_("Back 1 min"), "<Shift>Left", seek_back_long, false},
    {"stop_after", N_("Stop after this track"), "<Control>m", stop_after, false},
    {"repeat", N_("Repeat"), "<Control>r", repeat, false},
    {"shuffle", N_("Shuffle"), "<Control>s", shuffle, false},
    {"volume_up", N_("Volume up"), "plus,KP_Add", volume_up, false},
    {"volume_down", N_("Volume down"), "minus,KP_Subtract", volume_down, false},
    {"mute", N_("Mute"), "m", kp_player_toggle_mute, false},
    {"next_audio", N_("Next audio track"), "numbersign", next_audio, false},
    {"audio_delay_up", N_("Audio delay +100 ms"), "<Control>plus,<Control>KP_Add", audio_later,
     false},
    {"audio_delay_down", N_("Audio delay -100 ms"), "<Control>minus,<Control>KP_Subtract",
     audio_sooner, false},
    {"next_subtitle", N_("Next subtitle"), "j", next_sub, false},
    {"toggle_subtitles", N_("Show / hide subtitles"), "v", kp_tracks_toggle_subs, false},
    {"sub_delay_up", N_("Subtitle delay +100 ms"), "x", sub_later, false},
    {"sub_delay_down", N_("Subtitle delay -100 ms"), "z", sub_sooner, false},
    {"aspect", N_("Next aspect ratio"), "a", kp_tracks_cycle_aspect, false},
    {"fullscreen", N_("Fullscreen"), "f,F11", fullscreen, false},
    {"compact", N_("Compact mode"), "<Control>c", compact, false},
    {"video_area", N_("Video area"), "<Control><Shift>v", video_area, false},
    {"playlists", N_("Playlists"), "<Control>l", playlists, false},
    {"chat", N_("Chat"), "c", chat, false},
    {"add_files", N_("Add files"), "<Control>o", kp_playlists_add_files, false},
    {"add_folder", N_("Add folder"), "<Control><Shift>o", kp_playlists_add_folder, false},
    {"new_playlist", N_("New playlist"), "<Control>t", kp_playlists_new, false},
    {"rename_playlist", N_("Rename playlist"), "F2", kp_playlists_rename, false},
    {"close_playlist", N_("Close playlist"), "<Control>w", kp_playlists_close, false},
    {"jump", N_("Jump to file"), "<Control>j", kp_jump_dialog, false},
    {"queue", N_("Queue / unqueue"), "q", kp_playlists_toggle_queue, true},
    {"remove", N_("Remove selected"), "Delete", kp_playlists_remove_selected, true},
    {"cut", N_("Cut selected"), "<Control>x,<Shift>Delete", kp_playlists_cut, true},
    {"copy", N_("Copy selected"), "<Control>Insert", kp_playlists_copy, true},
    {"paste", N_("Paste"), "<Control>v,<Shift>Insert", kp_playlists_paste, true},
    {"move_up", N_("Move selection up"), "<Alt>Up", move_up, true},
    {"move_down", N_("Move selection down"), "<Alt>Down", move_down, true},
    {"preferences", N_("Preferences"), "<Control>p", kp_prefs_dialog, false},
    {"quit", N_("Quit"), "<Control>q", kp_window_quit, false},
};
#define N_ACTIONS G_N_ELEMENTS(actions)

static binding keys[N_ACTIONS][KEYS];
/* Menu items showing a command's key: action index in "kp-action". */
static GSList *menu_items;

static int find_action(const char *name)
{
    for (size_t i = 0; i < N_ACTIONS; i++)
        if (!strcmp(actions[i].name, name))
            return (int)i;
    return -1;
}

/* "a,b" into bindings; unknown names are skipped. */
static void parse(const char *list, binding out[KEYS])
{
    memset(out, 0, sizeof(binding) * KEYS);
    char **names = g_strsplit(list ? list : "", ",", -1);
    int n = 0;
    for (char **s = names; *s && n < KEYS; s++) {
        char *name = g_strstrip(*s);
        if (!*name)
            continue;
        gtk_accelerator_parse(name, &out[n].key, &out[n].mods);
        if (out[n].key)
            n++;
        else
            kp_log_warn(_("unknown key in [shortcuts]: %s"), name);
    }
    g_strfreev(names);
}

static char *unparse(const binding b[KEYS])
{
    GString *s = g_string_new(NULL);
    for (int k = 0; k < KEYS; k++) {
        if (!b[k].key)
            continue;
        char *name = gtk_accelerator_name(b[k].key, b[k].mods);
        if (s->len)
            g_string_append_c(s, ',');
        g_string_append(s, name);
        g_free(name);
    }
    return g_string_free(s, FALSE);
}

/* The menus show the keys through accelerators in a group of their own
 * that no window has: GTK 2's accel labels only read keys from a group,
 * and this one never fires (the key-press handler runs the commands). */
static GtkAccelGroup *shown;

static void show_key(GtkWidget *item, int a)
{
    guint old_key = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(item), "kp-key"));
    guint old_mods = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(item), "kp-mods"));
    if (old_key == keys[a][0].key && old_mods == (guint)keys[a][0].mods)
        return;
    if (old_key)
        gtk_widget_remove_accelerator(item, shown, old_key, (GdkModifierType)old_mods);
    if (keys[a][0].key)
        gtk_widget_add_accelerator(item, "activate", shown, keys[a][0].key, keys[a][0].mods,
                                   GTK_ACCEL_VISIBLE);
    g_object_set_data(G_OBJECT(item), "kp-key", GUINT_TO_POINTER(keys[a][0].key));
    g_object_set_data(G_OBJECT(item), "kp-mods", GUINT_TO_POINTER((guint)keys[a][0].mods));
}

static void update_menu_items(void)
{
    for (GSList *l = menu_items; l; l = l->next)
        show_key(l->data, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(l->data), "kp-action")));
}

void kp_shortcuts_load(kp_app *app)
{
    for (size_t i = 0; i < N_ACTIONS; i++)
    {
        /* Present but empty: no key on purpose. */
        const char *v = kp_prefs_get_raw(app->prefs, "shortcuts", actions[i].name);
        parse(v ? v : actions[i].defaults, keys[i]);
    }
    update_menu_items();
}

static void forget_item(gpointer data, GObject *gone)
{
    (void)data;
    menu_items = g_slist_remove(menu_items, gone);
}

void kp_shortcuts_attach(GtkWidget *item, const char *name)
{
    int a = find_action(name);
    if (a < 0)
        return;
    if (!shown)
        shown = gtk_accel_group_new();
    g_object_set_data(G_OBJECT(item), "kp-action", GINT_TO_POINTER(a));
    menu_items = g_slist_prepend(menu_items, item);
    g_object_weak_ref(G_OBJECT(item), forget_item, NULL);
    show_key(item, a);
}

/* The key as bindings name it: Shift stays only where it isn't what made
 * the character ("+" on a US keyboard is Shift+=, and binds as "plus";
 * Shift+Right stays Shift+Right). Letters are compared in lower case. */
static void normalize(const GdkEventKey *ev, guint *key, GdkModifierType *mods)
{
    GdkModifierType consumed = 0;
    gdk_keymap_translate_keyboard_state(gdk_keymap_get_default(), ev->hardware_keycode,
                                        (GdkModifierType)ev->state, ev->group, NULL, NULL, NULL,
                                        &consumed);
    *key = gdk_keyval_to_lower(ev->keyval);
    if (*key != ev->keyval)
        consumed &= ~GDK_SHIFT_MASK; /* Shift+Q is Shift+q, a key of its own */
    *mods = ev->state & gtk_accelerator_get_default_mod_mask() & ~consumed;
}

static bool is_text_field(GtkWidget *focus)
{
    return focus && GTK_IS_EDITABLE(focus);
}

static bool in_playlist(kp_app *app, GtkWidget *focus)
{
    return focus && GTK_IS_TREE_VIEW(focus) && gtk_widget_is_ancestor(focus, app->notebook);
}

bool kp_shortcuts_key(kp_app *app, const GdkEventKey *ev)
{
    guint key;
    GdkModifierType mods;
    normalize(ev, &key, &mods);
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(app->window));
    /* In a text field, typing is typing: only keys with Ctrl or Alt, and
     * function keys, are commands there. */
    bool typing = is_text_field(focus) && !(mods & (GDK_CONTROL_MASK | GDK_MOD1_MASK)) &&
                  !(key >= GDK_F1 && key <= GDK_F35);
    if (typing)
        return false;
    for (size_t i = 0; i < N_ACTIONS; i++)
        for (int k = 0; k < KEYS; k++)
            if (keys[i][k].key && gdk_keyval_to_lower(keys[i][k].key) == key &&
                keys[i][k].mods == mods) {
                if (actions[i].in_list && !in_playlist(app, focus))
                    return false;
                kp_log_debug("key: %s", actions[i].name);
                actions[i].run(app);
                return true;
            }
    return false;
}

/* ---- the editor (Preferences > Shortcuts) -------------------------------- */

enum { COL_LABEL, COL_ACTION, COL_KEY0, COL_MODS0, COL_KEY1, COL_MODS1, N_COLS };

typedef struct {
    GtkListStore *store;
    GtkWidget *note;
} editor;

static void store_keys(GtkListStore *store, GtkTreeIter *it, const binding b[KEYS])
{
    gtk_list_store_set(store, it, COL_KEY0, b[0].key, COL_MODS0, (guint)b[0].mods, COL_KEY1,
                       b[1].key, COL_MODS1, (guint)b[1].mods, -1);
}

static void fill(editor *ed, bool defaults)
{
    gtk_list_store_clear(ed->store);
    for (size_t i = 0; i < N_ACTIONS; i++) {
        GtkTreeIter it;
        binding b[KEYS];
        if (defaults)
            parse(actions[i].defaults, b);
        else
            memcpy(b, keys[i], sizeof b);
        gtk_list_store_append(ed->store, &it);
        gtk_list_store_set(ed->store, &it, COL_LABEL, _(actions[i].label), COL_ACTION, (int)i, -1);
        store_keys(ed->store, &it, b);
    }
}

static void get_keys(GtkTreeModel *m, GtkTreeIter *it, binding b[KEYS])
{
    guint mods0, mods1;
    gtk_tree_model_get(m, it, COL_KEY0, &b[0].key, COL_MODS0, &mods0, COL_KEY1, &b[1].key,
                       COL_MODS1, &mods1, -1);
    b[0].mods = (GdkModifierType)mods0;
    b[1].mods = (GdkModifierType)mods1;
}

/* The key went to another command: it leaves the one that had it. */
static void take_key(editor *ed, const char *path_str, int slot, guint key,
                     GdkModifierType mods)
{
    GtkTreeModel *m = GTK_TREE_MODEL(ed->store);
    GtkTreeIter it;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
    int row = gtk_tree_path_get_indices(path)[0];
    gtk_tree_path_free(path);
    gtk_label_set_text(GTK_LABEL(ed->note), "");
    gboolean ok = gtk_tree_model_get_iter_first(m, &it);
    for (int r = 0; ok; r++, ok = gtk_tree_model_iter_next(m, &it)) {
        binding b[KEYS];
        get_keys(m, &it, b);
        if (r == row) {
            b[slot].key = key;
            b[slot].mods = mods;
            store_keys(ed->store, &it, b);
            continue;
        }
        for (int k = 0; k < KEYS && key; k++)
            if (b[k].key == key && b[k].mods == mods) {
                char *label;
                gtk_tree_model_get(m, &it, COL_LABEL, &label, -1);
                char *name = gtk_accelerator_get_label(key, mods);
                char *msg = g_strdup_printf(_("%s was taken from \"%s\"."), name, label);
                gtk_label_set_text(GTK_LABEL(ed->note), msg);
                g_free(msg);
                g_free(name);
                g_free(label);
                b[k].key = 0;
                b[k].mods = 0;
                store_keys(ed->store, &it, b);
            }
    }
}

static void on_accel_edited(GtkCellRendererAccel *r, gchar *path, guint key,
                            GdkModifierType mods, guint hw, gpointer data)
{
    (void)hw;
    int slot = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "kp-slot"));
    take_key(data, path, slot, gdk_keyval_to_lower(key),
             mods & gtk_accelerator_get_default_mod_mask());
}

static void on_accel_cleared(GtkCellRendererAccel *r, gchar *path, gpointer data)
{
    int slot = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "kp-slot"));
    take_key(data, path, slot, 0, 0);
}

static void on_defaults(GtkButton *b, gpointer data)
{
    (void)b;
    editor *ed = data;
    fill(ed, true);
    gtk_label_set_text(GTK_LABEL(ed->note), "");
}

/* No key: an empty cell, not GTK's "Disabled" on most rows. */
static void key_cell(GtkTreeViewColumn *col, GtkCellRenderer *r, GtkTreeModel *m, GtkTreeIter *it,
                     gpointer data)
{
    (void)col;
    guint key;
    gtk_tree_model_get(m, it, GPOINTER_TO_INT(data), &key, -1);
    if (!key)
        g_object_set(r, "text", "", NULL);
}

static GtkTreeViewColumn *key_column(editor *ed, const char *title, int slot)
{
    GtkCellRenderer *r = gtk_cell_renderer_accel_new();
    /* "Other": plain keys (Space, Q) are allowed, unlike GTK's own mode. */
    g_object_set(r, "editable", TRUE, "accel-mode", GTK_CELL_RENDERER_ACCEL_MODE_OTHER, NULL);
    g_object_set_data(G_OBJECT(r), "kp-slot", GINT_TO_POINTER(slot));
    g_signal_connect(r, "accel-edited", G_CALLBACK(on_accel_edited), ed);
    g_signal_connect(r, "accel-cleared", G_CALLBACK(on_accel_cleared), ed);
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
        title, r, "accel-key", slot ? COL_KEY1 : COL_KEY0, "accel-mods",
        slot ? COL_MODS1 : COL_MODS0, NULL);
    gtk_tree_view_column_set_cell_data_func(col, r, key_cell,
                                            GINT_TO_POINTER(slot ? COL_KEY1 : COL_KEY0), NULL);
    gtk_tree_view_column_set_min_width(col, 130);
    return col;
}

GtkWidget *kp_shortcuts_editor_new(kp_app *app)
{
    (void)app;
    editor *ed = g_new0(editor, 1);
    ed->store = gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_INT, G_TYPE_UINT, G_TYPE_UINT,
                                   G_TYPE_UINT, G_TYPE_UINT);
    fill(ed, false);

    GtkWidget *box = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    g_object_set_data_full(G_OBJECT(box), "kp-editor", ed, g_free);
    g_object_set_data_full(G_OBJECT(box), "kp-store", ed->store, g_object_unref);

    GtkWidget *hint = gtk_label_new(_("Click a key to change it; Backspace removes it. The media "
                                      "keys and Alt+1..9 (tabs) always work."));
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_set_size_request(hint, 480, -1);
    gtk_misc_set_alignment(GTK_MISC(hint), 0, 0.5);
    gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);

    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ed->store));
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(view), FALSE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view),
                                gtk_tree_view_column_new_with_attributes(
                                    _("Command"), gtk_cell_renderer_text_new(), "text", COL_LABEL,
                                    NULL));
    gtk_tree_view_column_set_expand(gtk_tree_view_get_column(GTK_TREE_VIEW(view), 0), TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), key_column(ed, _("Key"), 0));
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), key_column(ed, _("Alternative"), 1));
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    GtkWidget *row = gtk_hbox_new(FALSE, 6);
    ed->note = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(ed->note), 0, 0.5);
    gtk_label_set_ellipsize(GTK_LABEL(ed->note), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(row), ed->note, TRUE, TRUE, 0);
    GtkWidget *defaults = gtk_button_new_with_mnemonic(_("Restore _defaults"));
    g_signal_connect(defaults, "clicked", G_CALLBACK(on_defaults), ed);
    gtk_box_pack_end(GTK_BOX(row), defaults, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    return box;
}

void kp_shortcuts_editor_apply(kp_app *app, GtkWidget *w)
{
    editor *ed = g_object_get_data(G_OBJECT(w), "kp-editor");
    GtkTreeModel *m = GTK_TREE_MODEL(ed->store);
    GtkTreeIter it;
    for (gboolean ok = gtk_tree_model_get_iter_first(m, &it); ok;
         ok = gtk_tree_model_iter_next(m, &it)) {
        int a;
        binding b[KEYS];
        gtk_tree_model_get(m, &it, COL_ACTION, &a, -1);
        get_keys(m, &it, b);
        /* The empty slot goes last. */
        if (!b[0].key && b[1].key) {
            b[0] = b[1];
            b[1] = (binding){0, 0};
        }
        binding def[KEYS];
        parse(actions[a].defaults, def);
        char *now = unparse(b), *was = unparse(def);
        if (!strcmp(now, was))
            kp_prefs_unset(app->prefs, "shortcuts", actions[a].name);
        else
            kp_prefs_set_str(app->prefs, "shortcuts", actions[a].name, now);
        g_free(now);
        g_free(was);
    }
    kp_shortcuts_load(app);
}
