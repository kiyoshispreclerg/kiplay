/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mpris.h"

#include <gio/gio.h>
#include <string.h>
#include <unistd.h>

#include "log.h"

#define OBJECT_PATH "/org/mpris/MediaPlayer2"
#define ROOT_IFACE "org.mpris.MediaPlayer2"
#define PLAYER_IFACE "org.mpris.MediaPlayer2.Player"
#define BUS_NAME "org.mpris.MediaPlayer2.kiplay"
#define NO_TRACK "/org/mpris/MediaPlayer2/TrackList/NoTrack"
#define US 1000000.0

static const char introspection[] =
    "<node>"
    " <interface name='org.mpris.MediaPlayer2'>"
    "  <method name='Raise'/>"
    "  <method name='Quit'/>"
    "  <property name='CanQuit' type='b' access='read'/>"
    "  <property name='CanRaise' type='b' access='read'/>"
    "  <property name='HasTrackList' type='b' access='read'/>"
    "  <property name='Identity' type='s' access='read'/>"
    "  <property name='DesktopEntry' type='s' access='read'/>"
    "  <property name='SupportedUriSchemes' type='as' access='read'/>"
    "  <property name='SupportedMimeTypes' type='as' access='read'/>"
    " </interface>"
    " <interface name='org.mpris.MediaPlayer2.Player'>"
    "  <method name='Next'/>"
    "  <method name='Previous'/>"
    "  <method name='Pause'/>"
    "  <method name='PlayPause'/>"
    "  <method name='Stop'/>"
    "  <method name='Play'/>"
    "  <method name='Seek'><arg direction='in' name='Offset' type='x'/></method>"
    "  <method name='SetPosition'>"
    "   <arg direction='in' name='TrackId' type='o'/>"
    "   <arg direction='in' name='Position' type='x'/>"
    "  </method>"
    "  <method name='OpenUri'><arg direction='in' name='Uri' type='s'/></method>"
    "  <signal name='Seeked'><arg name='Position' type='x'/></signal>"
    "  <property name='PlaybackStatus' type='s' access='read'/>"
    "  <property name='LoopStatus' type='s' access='readwrite'/>"
    "  <property name='Rate' type='d' access='readwrite'/>"
    "  <property name='Shuffle' type='b' access='readwrite'/>"
    "  <property name='Metadata' type='a{sv}' access='read'/>"
    "  <property name='Volume' type='d' access='readwrite'/>"
    "  <property name='Position' type='x' access='read'/>"
    "  <property name='MinimumRate' type='d' access='read'/>"
    "  <property name='MaximumRate' type='d' access='read'/>"
    "  <property name='CanGoNext' type='b' access='read'/>"
    "  <property name='CanGoPrevious' type='b' access='read'/>"
    "  <property name='CanPlay' type='b' access='read'/>"
    "  <property name='CanPause' type='b' access='read'/>"
    "  <property name='CanSeek' type='b' access='read'/>"
    "  <property name='CanControl' type='b' access='read'/>"
    " </interface>"
    "</node>";

/* Player properties that change, as bits (what PropertiesChanged lists). */
enum {
    CH_STATUS = 1 << 0,
    CH_METADATA = 1 << 1,
    CH_VOLUME = 1 << 2,
    CH_LOOP = 1 << 3,
    CH_SHUFFLE = 1 << 4,
    CH_SEEK = 1 << 5,
};

struct kp_mpris {
    kp_mpris_ops ops;
    void *ud;
    GCancellable *cancel;
    GDBusConnection *conn;
    GDBusNodeInfo *node;
    guint reg_root, reg_player, own_id, own_instance_id;
    char *name; /* owned bus name, NULL until then */
    bool tried_instance;

    kp_mpris_status status;
    char *title, *artist, *album, *url;
    double length;
    unsigned track_serial; /* 0 = no track */
    double volume;
    bool repeat, shuffle, seekable;

    unsigned changed;
    guint notify_idle;
};

/* ---- properties ---------------------------------------------------------- */

static const char *status_name(kp_mpris_status s)
{
    return s == KP_MPRIS_PLAYING ? "Playing" : s == KP_MPRIS_PAUSED ? "Paused" : "Stopped";
}

static char *track_path(const kp_mpris *m)
{
    return m->track_serial ? g_strdup_printf("/org/kiplay/track/%u", m->track_serial)
                           : g_strdup(NO_TRACK);
}

static GVariant *metadata(const kp_mpris *m)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    char *path = track_path(m);
    g_variant_builder_add(&b, "{sv}", "mpris:trackid", g_variant_new_object_path(path));
    g_free(path);
    if (m->track_serial) {
        if (m->length > 0)
            g_variant_builder_add(&b, "{sv}", "mpris:length",
                                  g_variant_new_int64((gint64)(m->length * US)));
        if (m->title)
            g_variant_builder_add(&b, "{sv}", "xesam:title", g_variant_new_string(m->title));
        if (m->artist) {
            const char *artists[] = {m->artist, NULL};
            g_variant_builder_add(&b, "{sv}", "xesam:artist", g_variant_new_strv(artists, -1));
        }
        if (m->album)
            g_variant_builder_add(&b, "{sv}", "xesam:album", g_variant_new_string(m->album));
        if (m->url)
            g_variant_builder_add(&b, "{sv}", "xesam:url", g_variant_new_string(m->url));
    }
    return g_variant_builder_end(&b);
}

static GVariant *root_property(const kp_mpris *m, const char *name)
{
    (void)m;
    if (!strcmp(name, "CanQuit") || !strcmp(name, "CanRaise"))
        return g_variant_new_boolean(TRUE);
    if (!strcmp(name, "HasTrackList"))
        return g_variant_new_boolean(FALSE);
    if (!strcmp(name, "Identity"))
        return g_variant_new_string("Kiplay");
    if (!strcmp(name, "DesktopEntry"))
        return g_variant_new_string("kiplay");
    if (!strcmp(name, "SupportedUriSchemes")) {
        static const char *const schemes[] = {"file", "http", "https", NULL};
        return g_variant_new_strv(schemes, -1);
    }
    if (!strcmp(name, "SupportedMimeTypes"))
        return g_variant_new_strv(NULL, 0);
    return NULL;
}

static GVariant *player_property(kp_mpris *m, const char *name)
{
    if (!strcmp(name, "PlaybackStatus"))
        return g_variant_new_string(status_name(m->status));
    if (!strcmp(name, "LoopStatus"))
        return g_variant_new_string(m->repeat ? "Playlist" : "None");
    if (!strcmp(name, "Rate") || !strcmp(name, "MinimumRate") || !strcmp(name, "MaximumRate"))
        return g_variant_new_double(1.0);
    if (!strcmp(name, "Shuffle"))
        return g_variant_new_boolean(m->shuffle);
    if (!strcmp(name, "Metadata"))
        return metadata(m);
    if (!strcmp(name, "Volume"))
        return g_variant_new_double(m->volume);
    if (!strcmp(name, "Position")) {
        double pos = m->status != KP_MPRIS_STOPPED && m->ops.position ? m->ops.position(m->ud) : 0;
        return g_variant_new_int64((gint64)(pos * US));
    }
    if (!strcmp(name, "CanGoNext") || !strcmp(name, "CanGoPrevious") ||
        !strcmp(name, "CanPlay") || !strcmp(name, "CanControl"))
        return g_variant_new_boolean(TRUE);
    if (!strcmp(name, "CanPause"))
        return g_variant_new_boolean(m->track_serial != 0);
    if (!strcmp(name, "CanSeek"))
        return g_variant_new_boolean(m->seekable && m->status != KP_MPRIS_STOPPED);
    return NULL;
}

static GVariant *get_property(GDBusConnection *c, const char *sender, const char *path,
                              const char *iface, const char *name, GError **err, gpointer data)
{
    (void)c;
    (void)sender;
    (void)path;
    kp_mpris *m = data;
    GVariant *v = !strcmp(iface, ROOT_IFACE) ? root_property(m, name) : player_property(m, name);
    if (!v)
        g_set_error(err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "no property %s", name);
    return v;
}

static gboolean set_property(GDBusConnection *c, const char *sender, const char *path,
                             const char *iface, const char *name, GVariant *value, GError **err,
                             gpointer data)
{
    (void)c;
    (void)sender;
    (void)path;
    (void)iface;
    kp_mpris *m = data;
    if (!strcmp(name, "LoopStatus") && g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
        /* Kiplay repeats the playlist; "Track" is taken as repeat too. */
        bool on = strcmp(g_variant_get_string(value, NULL), "None") != 0;
        if (m->ops.set_repeat)
            m->ops.set_repeat(m->ud, on);
        return TRUE;
    }
    if (!strcmp(name, "Shuffle") && g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
        if (m->ops.set_shuffle)
            m->ops.set_shuffle(m->ud, g_variant_get_boolean(value));
        return TRUE;
    }
    if (!strcmp(name, "Volume") && g_variant_is_of_type(value, G_VARIANT_TYPE_DOUBLE)) {
        if (m->ops.set_volume)
            m->ops.set_volume(m->ud, CLAMP(g_variant_get_double(value), 0.0, 1.0));
        return TRUE;
    }
    if (!strcmp(name, "Rate"))
        return TRUE; /* only 1.0, as Minimum/MaximumRate say */
    g_set_error(err, G_DBUS_ERROR, G_DBUS_ERROR_PROPERTY_READ_ONLY, "%s is read-only", name);
    return FALSE;
}

/* ---- methods ------------------------------------------------------------- */

static void call0(void (*fn)(void *), void *ud)
{
    if (fn)
        fn(ud);
}

static void method_call(GDBusConnection *c, const char *sender, const char *path,
                        const char *iface, const char *method, GVariant *params,
                        GDBusMethodInvocation *inv, gpointer data)
{
    (void)c;
    (void)sender;
    (void)path;
    kp_mpris *m = data;
    kp_log_debug("mpris: %s.%s", iface, method);
    /* Answer first: what the call does may take a while (a dialog). */
    g_dbus_method_invocation_return_value(inv, NULL);
    if (!strcmp(method, "Raise"))
        call0(m->ops.raise, m->ud);
    else if (!strcmp(method, "Quit"))
        call0(m->ops.quit, m->ud);
    else if (!strcmp(method, "Next"))
        call0(m->ops.next, m->ud);
    else if (!strcmp(method, "Previous"))
        call0(m->ops.previous, m->ud);
    else if (!strcmp(method, "Pause"))
        call0(m->ops.pause, m->ud);
    else if (!strcmp(method, "PlayPause"))
        call0(m->ops.play_pause, m->ud);
    else if (!strcmp(method, "Stop"))
        call0(m->ops.stop, m->ud);
    else if (!strcmp(method, "Play"))
        call0(m->ops.play, m->ud);
    else if (!strcmp(method, "Seek")) {
        gint64 off;
        g_variant_get(params, "(x)", &off);
        if (m->ops.seek && m->seekable)
            m->ops.seek(m->ud, (double)off / US);
    } else if (!strcmp(method, "SetPosition")) {
        const char *track;
        gint64 pos;
        g_variant_get(params, "(&ox)", &track, &pos);
        /* Only for the track playing now, and inside it (the spec). */
        char *now = track_path(m);
        bool ok = !strcmp(track, now) && pos >= 0 && (m->length <= 0 || pos <= m->length * US);
        g_free(now);
        if (ok && m->ops.set_position && m->seekable)
            m->ops.set_position(m->ud, (double)pos / US);
    } else if (!strcmp(method, "OpenUri")) {
        const char *uri;
        g_variant_get(params, "(&s)", &uri);
        if (m->ops.open_uri)
            m->ops.open_uri(m->ud, uri);
    }
}

static const GDBusInterfaceVTable vtable = {method_call, get_property, set_property, {0}};

/* ---- change notification ------------------------------------------------- */

/* Adds name to the list unless it is there. */
static void add_name(const char **names, int *n, const char *name)
{
    for (int i = 0; i < *n; i++)
        if (!strcmp(names[i], name))
            return;
    names[(*n)++] = name;
}

static gboolean notify(gpointer data)
{
    kp_mpris *m = data;
    m->notify_idle = 0;
    unsigned ch = m->changed;
    m->changed = 0;
    if (!m->conn || !m->reg_player || !ch)
        return G_SOURCE_REMOVE;
    const char *names[10];
    int n = 0;
    if (ch & CH_STATUS) {
        add_name(names, &n, "PlaybackStatus");
        add_name(names, &n, "CanSeek");
        add_name(names, &n, "CanPause");
    }
    if (ch & CH_METADATA) {
        add_name(names, &n, "Metadata");
        add_name(names, &n, "CanPause");
    }
    if (ch & CH_VOLUME)
        add_name(names, &n, "Volume");
    if (ch & CH_LOOP)
        add_name(names, &n, "LoopStatus");
    if (ch & CH_SHUFFLE)
        add_name(names, &n, "Shuffle");
    if (ch & CH_SEEK)
        add_name(names, &n, "CanSeek");
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    for (int i = 0; i < n; i++)
        g_variant_builder_add(&b, "{sv}", names[i], player_property(m, names[i]));
    GError *err = NULL;
    if (!g_dbus_connection_emit_signal(m->conn, NULL, OBJECT_PATH,
                                       "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                       g_variant_new("(sa{sv}as)", PLAYER_IFACE, &b, NULL),
                                       &err)) {
        kp_log_debug("mpris: %s", err->message);
        g_error_free(err);
    }
    return G_SOURCE_REMOVE;
}

static void changed(kp_mpris *m, unsigned what)
{
    m->changed |= what;
    if (!m->notify_idle)
        m->notify_idle = g_idle_add(notify, m);
}

/* ---- the bus ------------------------------------------------------------- */

static guint own(kp_mpris *m, const char *name);

static void on_name_acquired(GDBusConnection *c, const char *name, gpointer data)
{
    (void)c;
    kp_mpris *m = data;
    g_free(m->name);
    m->name = g_strdup(name);
    kp_log_debug("mpris: %s", name);
}

static void on_name_lost(GDBusConnection *c, const char *name, gpointer data)
{
    kp_mpris *m = data;
    if (!c || m->tried_instance) {
        kp_log_debug("mpris: could not own %s", name);
        return;
    }
    /* Another player called Kiplay is there: this one gets its own name. */
    m->tried_instance = true;
    char *mine = g_strdup_printf("%s.instance%d", BUS_NAME, (int)getpid());
    m->own_instance_id = own(m, mine);
    g_free(mine);
}

static guint own(kp_mpris *m, const char *name)
{
    return g_bus_own_name_on_connection(m->conn, name, G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
                                        on_name_acquired, on_name_lost, m, NULL);
}

static void on_bus(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src;
    GError *err = NULL;
    GDBusConnection *conn = g_bus_get_finish(res, &err);
    if (!conn) {
        /* Cancelled means m is gone. */
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            kp_log_debug("mpris: no session bus: %s", err->message);
        g_error_free(err);
        return;
    }
    kp_mpris *m = data;
    m->conn = conn;
    m->reg_root = g_dbus_connection_register_object(conn, OBJECT_PATH, m->node->interfaces[0],
                                                    &vtable, m, NULL, &err);
    if (m->reg_root)
        m->reg_player = g_dbus_connection_register_object(
            conn, OBJECT_PATH, m->node->interfaces[1], &vtable, m, NULL, &err);
    if (!m->reg_player) {
        kp_log_debug("mpris: %s", err->message);
        g_error_free(err);
        return;
    }
    m->own_id = own(m, BUS_NAME);
}

kp_mpris *kp_mpris_new(const kp_mpris_ops *ops, void *ud)
{
    kp_mpris *m = g_new0(kp_mpris, 1);
    m->ops = *ops;
    m->ud = ud;
    m->volume = 1.0;
    m->node = g_dbus_node_info_new_for_xml(introspection, NULL);
    m->cancel = g_cancellable_new();
    g_bus_get(G_BUS_TYPE_SESSION, m->cancel, on_bus, m);
    return m;
}

void kp_mpris_free(kp_mpris *m)
{
    if (!m)
        return;
    g_cancellable_cancel(m->cancel);
    g_object_unref(m->cancel);
    if (m->notify_idle)
        g_source_remove(m->notify_idle);
    if (m->own_id)
        g_bus_unown_name(m->own_id);
    if (m->own_instance_id)
        g_bus_unown_name(m->own_instance_id);
    if (m->conn) {
        if (m->reg_root)
            g_dbus_connection_unregister_object(m->conn, m->reg_root);
        if (m->reg_player)
            g_dbus_connection_unregister_object(m->conn, m->reg_player);
        g_object_unref(m->conn);
    }
    g_dbus_node_info_unref(m->node);
    g_free(m->name);
    g_free(m->title);
    g_free(m->artist);
    g_free(m->album);
    g_free(m->url);
    g_free(m);
}

const char *kp_mpris_bus_name(const kp_mpris *m)
{
    return m->name;
}

void kp_mpris_set_status(kp_mpris *m, kp_mpris_status status)
{
    if (m->status != status) {
        m->status = status;
        changed(m, CH_STATUS);
    }
}

/* Replaces *dst; true if it changed. */
static bool replace(char **dst, const char *src)
{
    if (g_strcmp0(*dst, src) == 0)
        return false;
    g_free(*dst);
    *dst = g_strdup(src);
    return true;
}

void kp_mpris_set_track(kp_mpris *m, const kp_mpris_track *t, bool new_track)
{
    static const kp_mpris_track none = {0};
    bool ch = false;
    if (!t) {
        ch = m->track_serial != 0;
        m->track_serial = 0;
        t = &none;
    } else if (new_track || !m->track_serial) {
        m->track_serial++;
        if (!m->track_serial) /* wrapped */
            m->track_serial = 1;
        ch = true;
    }
    ch |= replace(&m->title, t->title);
    ch |= replace(&m->artist, t->artist);
    ch |= replace(&m->album, t->album);
    ch |= replace(&m->url, t->url);
    if (m->length != t->length) {
        m->length = t->length;
        ch = true;
    }
    if (ch)
        changed(m, CH_METADATA);
}

void kp_mpris_set_volume(kp_mpris *m, double volume)
{
    if (m->volume != volume) {
        m->volume = volume;
        changed(m, CH_VOLUME);
    }
}

void kp_mpris_set_repeat(kp_mpris *m, bool on)
{
    if (m->repeat != on) {
        m->repeat = on;
        changed(m, CH_LOOP);
    }
}

void kp_mpris_set_shuffle(kp_mpris *m, bool on)
{
    if (m->shuffle != on) {
        m->shuffle = on;
        changed(m, CH_SHUFFLE);
    }
}

void kp_mpris_set_seekable(kp_mpris *m, bool on)
{
    if (m->seekable != on) {
        m->seekable = on;
        changed(m, CH_SEEK);
    }
}

void kp_mpris_seeked(kp_mpris *m, double seconds)
{
    if (!m->conn || !m->reg_player)
        return;
    g_dbus_connection_emit_signal(m->conn, NULL, OBJECT_PATH, PLAYER_IFACE, "Seeked",
                                  g_variant_new("(x)", (gint64)(seconds * US)), NULL);
}
