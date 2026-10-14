/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gio/gio.h>
#include <math.h>
#include <string.h>

#include "../harness.h"
#include "mpris.h"

/* MPRIS on a private session bus (GTestDBus runs its own dbus-daemon), so
 * the desktop's players never see it. */

typedef struct {
    int play_pause, next;
    double volume, position, seek;
    bool repeat;
} player;

static void op_play_pause(void *ud) { ((player *)ud)->play_pause++; }
static void op_next(void *ud) { ((player *)ud)->next++; }
static void op_volume(void *ud, double v) { ((player *)ud)->volume = v; }
static void op_position(void *ud, double s) { ((player *)ud)->position = s; }
static void op_seek(void *ud, double off) { ((player *)ud)->seek = off; }
static void op_repeat(void *ud, bool on) { ((player *)ud)->repeat = on; }
static double op_where(void *ud) { (void)ud; return 42.5; }

static const kp_mpris_ops ops = {
    .play_pause = op_play_pause,
    .next = op_next,
    .set_volume = op_volume,
    .set_position = op_position,
    .seek = op_seek,
    .set_repeat = op_repeat,
    .position = op_where,
};

static gboolean timeout(gpointer data)
{
    *(bool *)data = true;
    return G_SOURCE_REMOVE;
}

/* Runs the main loop until *cond or ms pass. */
static bool spin_until(const bool *cond, int ms)
{
    bool late = false;
    guint t = g_timeout_add(ms, timeout, &late);
    while (!*cond && !late)
        g_main_context_iteration(NULL, TRUE);
    if (!late)
        g_source_remove(t);
    return *cond;
}

static bool named(kp_mpris *m)
{
    bool late = false;
    guint t = g_timeout_add(5000, timeout, &late);
    while (!kp_mpris_bus_name(m) && !late)
        g_main_context_iteration(NULL, TRUE);
    if (!late)
        g_source_remove(t);
    return kp_mpris_bus_name(m) != NULL;
}

typedef struct {
    bool done;
    GVariant *reply;
} call_state;

static void on_reply(GObject *src, GAsyncResult *res, gpointer data)
{
    call_state *c = data;
    c->reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    c->done = true;
}

/* A call to the player, waited for; the reply (or NULL), to unref. */
static GVariant *call(GDBusConnection *conn, const char *name, const char *iface,
                      const char *method, GVariant *args)
{
    call_state c = {0};
    g_dbus_connection_call(conn, name, "/org/mpris/MediaPlayer2", iface, method, args, NULL,
                           G_DBUS_CALL_FLAGS_NONE, 5000, NULL, on_reply, &c);
    spin_until(&c.done, 6000);
    return c.reply;
}

static GVariant *get(GDBusConnection *conn, const char *name, const char *prop)
{
    GVariant *r = call(conn, name, "org.freedesktop.DBus.Properties", "Get",
                       g_variant_new("(ss)", "org.mpris.MediaPlayer2.Player", prop));
    if (!r)
        return NULL;
    GVariant *v;
    g_variant_get(r, "(v)", &v);
    g_variant_unref(r);
    return v;
}

typedef struct {
    bool got;
    char names[256];
} signal_state;

static void on_changed(GDBusConnection *c, const char *sender, const char *path,
                       const char *iface, const char *signal, GVariant *params, gpointer data)
{
    (void)c;
    (void)sender;
    (void)path;
    (void)iface;
    (void)signal;
    signal_state *s = data;
    GVariantIter *it;
    const char *name;
    GVariant *value;
    g_variant_get(params, "(&sa{sv}as)", &name, &it, NULL);
    s->names[0] = '\0';
    while (g_variant_iter_next(it, "{&sv}", &name, &value)) {
        g_strlcat(s->names, name, sizeof s->names);
        g_strlcat(s->names, " ", sizeof s->names);
        g_variant_unref(value);
    }
    g_variant_iter_free(it);
    s->got = true;
}

TEST(talks_mpris)
{
    char *daemon = g_find_program_in_path("dbus-daemon");
    if (!daemon) {
        fprintf(stderr, "  (dbus-daemon not installed: skipped)\n");
        return;
    }
    g_free(daemon);
    GTestDBus *bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(bus);

    player p = {0};
    kp_mpris *m = kp_mpris_new(&ops, &p);
    CHECK(named(m));
    const char *name = kp_mpris_bus_name(m);
    CHECK_STR_EQ(name, "org.mpris.MediaPlayer2.kiplay");
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    CHECK(conn);

    signal_state sig = {0};
    guint sub = g_dbus_connection_signal_subscribe(
        conn, NULL, "org.freedesktop.DBus.Properties", "PropertiesChanged",
        "/org/mpris/MediaPlayer2", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_changed, &sig, NULL);

    /* Nothing playing yet. */
    GVariant *v = get(conn, name, "PlaybackStatus");
    CHECK(v && !strcmp(g_variant_get_string(v, NULL), "Stopped"));
    g_variant_unref(v);

    /* A track starts: one PropertiesChanged with the status and metadata. */
    kp_mpris_track t = {"Emotion", "DJ Ross", "Best of", "file:///m/e.mp3", 198.5};
    kp_mpris_set_track(m, &t, true);
    kp_mpris_set_status(m, KP_MPRIS_PLAYING);
    kp_mpris_set_seekable(m, true);
    CHECK(spin_until(&sig.got, 3000));
    CHECK(strstr(sig.names, "PlaybackStatus") && strstr(sig.names, "Metadata"));

    v = get(conn, name, "Metadata");
    CHECK(v);
    const char *title = NULL, *track = NULL;
    gint64 length = 0;
    CHECK(g_variant_lookup(v, "xesam:title", "&s", &title));
    CHECK_STR_EQ(title, "Emotion");
    CHECK(g_variant_lookup(v, "mpris:length", "x", &length));
    CHECK_INT_EQ(length, 198500000);
    CHECK(g_variant_lookup(v, "mpris:trackid", "&o", &track));
    char *track_id = g_strdup(track);
    g_variant_unref(v);
    v = get(conn, name, "Position");
    CHECK(v && g_variant_get_int64(v) == 42500000);
    g_variant_unref(v);

    /* Requests reach the player. */
    GVariant *r = call(conn, name, "org.mpris.MediaPlayer2.Player", "PlayPause", NULL);
    CHECK(r);
    g_variant_unref(r);
    CHECK_INT_EQ(p.play_pause, 1);
    r = call(conn, name, "org.mpris.MediaPlayer2.Player", "Seek", g_variant_new("(x)", (gint64)-5000000));
    g_variant_unref(r);
    CHECK(fabs(p.seek + 5) < 1e-9);
    /* SetPosition only for the track playing now. */
    r = call(conn, name, "org.mpris.MediaPlayer2.Player", "SetPosition",
             g_variant_new("(ox)", "/org/kiplay/track/999", (gint64)10000000));
    g_variant_unref(r);
    CHECK(p.position == 0);
    r = call(conn, name, "org.mpris.MediaPlayer2.Player", "SetPosition",
             g_variant_new("(ox)", track_id, (gint64)10000000));
    g_variant_unref(r);
    CHECK(fabs(p.position - 10) < 1e-9);
    g_free(track_id);
    r = call(conn, name, "org.freedesktop.DBus.Properties", "Set",
             g_variant_new("(ssv)", "org.mpris.MediaPlayer2.Player", "Volume",
                           g_variant_new_double(0.25)));
    g_variant_unref(r);
    CHECK(fabs(p.volume - 0.25) < 1e-9);
    r = call(conn, name, "org.freedesktop.DBus.Properties", "Set",
             g_variant_new("(ssv)", "org.mpris.MediaPlayer2.Player", "LoopStatus",
                           g_variant_new_string("Playlist")));
    g_variant_unref(r);
    CHECK(p.repeat);

    g_dbus_connection_signal_unsubscribe(conn, sub);
    kp_mpris_free(m);

    /* With the name taken (another Kiplay, another process: here another
     * connection), it gets a name of its own. */
    char *addr = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    GDBusConnection *taker = g_dbus_connection_new_for_address_sync(
        addr, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                  G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, NULL);
    CHECK(taker);
    r = g_dbus_connection_call_sync(taker, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                    "org.freedesktop.DBus", "RequestName",
                                    g_variant_new("(su)", "org.mpris.MediaPlayer2.kiplay", 4u),
                                    NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL);
    CHECK(r);
    g_variant_unref(r);
    kp_mpris *other = kp_mpris_new(&ops, &p);
    CHECK(named(other));
    CHECK(g_str_has_prefix(kp_mpris_bus_name(other), "org.mpris.MediaPlayer2.kiplay.instance"));
    kp_mpris_free(other);
    g_object_unref(taker);
    g_free(addr);
    g_object_unref(conn);
    g_test_dbus_down(bus);
    g_object_unref(bus);
}

int main(void)
{
    RUN(talks_mpris);
    return harness_report();
}
