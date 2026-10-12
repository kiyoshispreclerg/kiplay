/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <locale.h>
#include <string.h>
#include <unistd.h>

#include "backend_impl.h"
#include "cJSON.h"
#include "child.h"
#include "json.h"
#include "lineio.h"
#include "log.h"

/* mpv in --idle mode, driven over its JSON IPC socket
 * (https://mpv.io/manual/stable/#json-ipc). One JSON object per line each
 * way. Properties we care about are observed (mpv pushes changes), except
 * time-pos, which changes every frame: that one is asked for on a timer. */

enum {
    OBS_DURATION = 1,
    OBS_PAUSE,
    OBS_TITLE,
    OBS_TRACKS,
    OBS_METADATA,
};
#define REQ_TIME 100

#define CONNECT_RETRY_MS 30
#define CONNECT_GIVE_UP_MS 10000
#define TICK_MS 250

typedef struct {
    kp_backend base;
    kp_backend_opts opts;
    char *program, *extra;
    char **test_args;
    kp_event_cb cb;
    void *ud;

    char *sock_path;
    kp_child *child;
    GSocketConnection *conn;
    kp_lineio *io; /* queues commands until the socket exists */
    guint connect_timer, tick_timer;
    int connect_elapsed;
    bool loaded;
} mpv;

static void emit(mpv *m, kp_event_type type, double value, bool flag, const char *text)
{
    kp_event ev = {type, value, flag, text};
    m->cb(m->ud, &ev);
}

/* ---- parsing ------------------------------------------------------------- */

/* JSON numbers use a dot, but cJSON reads and writes them with strtod and
 * sprintf, which follow LC_NUMERIC: under pt_BR "268.05" fails to parse
 * and a seek would go out as "12,5". Every cJSON call here runs in the C
 * locale, whatever the program's. */
static locale_t c_locale(void)
{
    static locale_t loc;
    if (!loc)
        loc = newlocale(LC_ALL_MASK, "C", (locale_t)0);
    return loc;
}

/* A track list has a real video track if one isn't just embedded cover art. */
static bool has_real_video(const cJSON *tracks)
{
    const cJSON *t;
    cJSON_ArrayForEach(t, tracks)
    {
        const char *type = kp_json_str(t, "type");
        if (type && !strcmp(type, "video") && !cJSON_IsTrue(cJSON_GetObjectItem(t, "albumart")))
            return true;
    }
    return false;
}

/* Tag names vary in case (ID3 "title", Vorbis "TITLE"). */
static const char *tag(const cJSON *meta, const char *name)
{
    const cJSON *t;
    cJSON_ArrayForEach(t, meta)
    {
        if (t->string && !g_ascii_strcasecmp(t->string, name) && cJSON_IsString(t) &&
            *t->valuestring)
            return t->valuestring;
    }
    return NULL;
}

void kp_mpv_parse_line(const char *line, kp_event_cb cb, void *ud)
{
    locale_t old = uselocale(c_locale());
    cJSON *j = cJSON_Parse(line);
    uselocale(old);
    if (!j)
        return;
    kp_event ev = {0};
    bool send = false;
    char *owned = NULL;
    const char *event = kp_json_str(j, "event");
    const cJSON *data = cJSON_GetObjectItem(j, "data");
    if (event && !strcmp(event, "property-change")) {
        switch ((int)kp_json_num(j, "id", 0)) {
        case OBS_DURATION:
            ev = (kp_event){KP_EV_DURATION, cJSON_IsNumber(data) ? data->valuedouble : -1, false, NULL};
            send = true;
            break;
        case OBS_PAUSE:
            ev = (kp_event){KP_EV_PAUSED, 0, cJSON_IsTrue(data), NULL};
            send = true;
            break;
        case OBS_TITLE:
            if (cJSON_IsString(data)) {
                ev = (kp_event){KP_EV_TITLE, 0, false, data->valuestring};
                send = true;
            }
            break;
        case OBS_TRACKS:
            ev = (kp_event){KP_EV_VIDEO, 0, has_real_video(data), NULL};
            send = true;
            break;
        case OBS_METADATA: {
            /* Audacious's "Artist - Title"; the title alone if no artist. */
            const char *title = tag(data, "title"), *artist = tag(data, "artist");
            if (title && artist)
                owned = g_strdup_printf("%s - %s", artist, title);
            ev = (kp_event){KP_EV_TAGS, 0, false, owned ? owned : title};
            send = true;
            break;
        }
        default:
            break;
        }
    } else if (event && !strcmp(event, "file-loaded")) {
        ev = (kp_event){KP_EV_LOADED, 0, false, NULL};
        send = true;
    } else if (event && !strcmp(event, "end-file")) {
        /* "stop", "quit" and "redirect" are ours or mpv's business: only
         * a file that ran out (or failed) moves the playlist on. */
        const char *reason = kp_json_str(j, "reason");
        if (reason && (!strcmp(reason, "eof") || !strcmp(reason, "error"))) {
            ev = (kp_event){KP_EV_ENDED, 0, !strcmp(reason, "error"),
                            kp_json_str(j, "file_error")};
            send = true;
        }
    } else if (event && !strcmp(event, "idle")) {
        ev = (kp_event){KP_EV_IDLE, 0, false, NULL};
        send = true;
    } else if (!event && kp_json_num(j, "request_id", 0) == REQ_TIME && cJSON_IsNumber(data)) {
        ev = (kp_event){KP_EV_TIME, data->valuedouble, false, NULL};
        send = true;
    }
    if (send)
        cb(ud, &ev);
    g_free(owned);
    cJSON_Delete(j);
}

static void on_parsed(void *ud, const kp_event *ev)
{
    mpv *m = ud;
    if (ev->type == KP_EV_LOADED)
        m->loaded = true;
    else if (ev->type == KP_EV_ENDED || ev->type == KP_EV_IDLE)
        m->loaded = false;
    m->cb(m->ud, ev);
}

/* ---- commands ------------------------------------------------------------ */

/* Takes ownership of cmd ({"command": [...]}). */
static void send_json(mpv *m, cJSON *cmd)
{
    locale_t old = uselocale(c_locale());
    char *text = cJSON_PrintUnformatted(cmd);
    uselocale(old);
    cJSON_Delete(cmd);
    if (!text)
        return;
    kp_lineio_send(m->io, text);
    cJSON_free(text);
}

static cJSON *command(const char *name)
{
    cJSON *cmd = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(cmd, "command");
    cJSON_AddItemToArray(arr, cJSON_CreateString(name));
    return cmd;
}

static cJSON *args_of(cJSON *cmd)
{
    return cJSON_GetObjectItem(cmd, "command");
}

static void set_property(mpv *m, const char *name, cJSON *value)
{
    cJSON *cmd = command("set_property");
    cJSON_AddItemToArray(args_of(cmd), cJSON_CreateString(name));
    cJSON_AddItemToArray(args_of(cmd), value);
    send_json(m, cmd);
}

static void observe(mpv *m, int id, const char *name)
{
    cJSON *cmd = command("observe_property");
    cJSON_AddItemToArray(args_of(cmd), cJSON_CreateNumber(id));
    cJSON_AddItemToArray(args_of(cmd), cJSON_CreateString(name));
    send_json(m, cmd);
}

static void on_line(void *ud, const char *line)
{
    mpv *m = ud;
    if (!line)
        return; /* the process is going away; its exit reports it */
    if (!strstr(line, "\"request_id\":100"))
        kp_log_debug("mpv< %.200s", line);
    kp_mpv_parse_line(line, on_parsed, m);
}

static gboolean on_tick(gpointer data)
{
    mpv *m = data;
    if (m->conn && m->loaded) {
        cJSON *cmd = command("get_property");
        cJSON_AddItemToArray(args_of(cmd), cJSON_CreateString("time-pos"));
        cJSON_AddNumberToObject(cmd, "request_id", REQ_TIME);
        send_json(m, cmd);
    }
    return G_SOURCE_CONTINUE;
}

static void set_ticking(kp_backend *b, bool on)
{
    mpv *m = (mpv *)b;
    if (on && !m->tick_timer)
        m->tick_timer = g_timeout_add(TICK_MS, on_tick, m);
    else if (!on && m->tick_timer) {
        g_source_remove(m->tick_timer);
        m->tick_timer = 0;
    }
}

/* ---- the process --------------------------------------------------------- */

static void drop_connection(mpv *m)
{
    if (m->connect_timer) {
        g_source_remove(m->connect_timer);
        m->connect_timer = 0;
    }
    kp_lineio_detach(m->io);
    if (m->conn) {
        g_io_stream_close(G_IO_STREAM(m->conn), NULL, NULL);
        g_clear_object(&m->conn);
    }
}

static gboolean try_connect(gpointer data)
{
    mpv *m = data;
    GSocketClient *client = g_socket_client_new();
    GSocketAddress *addr = g_unix_socket_address_new(m->sock_path);
    /* A unix socket connects (or fails) at once: no waiting here. */
    GSocketConnection *conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), NULL, NULL);
    g_object_unref(addr);
    g_object_unref(client);
    if (!conn) {
        m->connect_elapsed += CONNECT_RETRY_MS;
        if (m->connect_elapsed < CONNECT_GIVE_UP_MS)
            return G_SOURCE_CONTINUE;
        m->connect_timer = 0;
        kp_log_warn("mpv: no IPC socket after %d s", CONNECT_GIVE_UP_MS / 1000);
        if (m->child)
            g_subprocess_force_exit(kp_child_proc(m->child));
        return G_SOURCE_REMOVE;
    }
    m->connect_timer = 0;
    m->conn = conn;
    kp_log_debug("mpv: connected after %d ms", m->connect_elapsed);
    kp_lineio_attach(m->io, g_io_stream_get_input_stream(G_IO_STREAM(conn)),
                     g_io_stream_get_output_stream(G_IO_STREAM(conn)));
    return G_SOURCE_REMOVE;
}

static void on_exit(void *ud, const char *why)
{
    mpv *m = ud;
    kp_log_debug("mpv: exited (%s)", why);
    drop_connection(m);
    kp_child_stop(m->child);
    m->child = NULL;
    m->loaded = false;
    emit(m, KP_EV_CRASHED, 0, false, why);
}

static void shutdown_process(mpv *m)
{
    if (!m->child)
        return;
    if (m->conn)
        send_json(m, command("quit"));
    drop_connection(m);
    /* mpv quits on SIGTERM too. */
    kp_child_stop(m->child);
    m->child = NULL;
    m->loaded = false;
    unlink(m->sock_path);
}

static void mpv_free(kp_backend *b)
{
    mpv *m = (mpv *)b;
    set_ticking(b, false);
    shutdown_process(m);
    kp_lineio_free(m->io);
    g_free(m->sock_path);
    g_free(m->program);
    g_free(m->extra);
    g_strfreev(m->test_args);
    g_free(m);
}

static bool running(const kp_backend *b)
{
    return ((const mpv *)b)->child != NULL;
}

static int pid(const kp_backend *b)
{
    return kp_child_pid(((const mpv *)b)->child);
}

static bool start(kp_backend *b, char **err)
{
    mpv *m = (mpv *)b;
    if (m->child)
        return true;
    unlink(m->sock_path);
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup(m->program));
    if (!m->opts.use_config)
        g_ptr_array_add(argv, g_strdup("--no-config"));
    /* Keys, mouse and the on-screen controller are Kiplay's; mpv only
     * decodes and draws. */
    const char *fixed[] = {"--idle=yes",
                           "--no-terminal",
                           "--force-window=no",
                           "--keep-open=no",
                           "--osc=no",
                           "--input-default-bindings=no",
                           "--input-vo-keyboard=no",
                           "--input-cursor=no",
                           "--cursor-autohide=no",
                           NULL};
    for (int i = 0; fixed[i]; i++)
        g_ptr_array_add(argv, g_strdup(fixed[i]));
    g_ptr_array_add(argv, g_strdup_printf("--input-ipc-server=%s", m->sock_path));
    g_ptr_array_add(argv, g_strdup_printf("--volume=%d", m->opts.volume));
    if (m->opts.wid)
        g_ptr_array_add(argv, g_strdup_printf("--wid=%lu", m->opts.wid));
    for (char **t = m->test_args; t && *t; t++)
        g_ptr_array_add(argv, g_strdup(*t));
    if (m->extra && *m->extra) {
        int n;
        char **extra;
        GError *gerr = NULL;
        if (g_shell_parse_argv(m->extra, &n, &extra, &gerr)) {
            for (int i = 0; i < n; i++)
                g_ptr_array_add(argv, extra[i]);
            g_free(extra); /* the strings moved into argv */
        } else {
            kp_log_warn("mpv: extra options: %s", gerr->message);
            g_error_free(gerr);
        }
    }
    g_ptr_array_add(argv, NULL);
    m->child = kp_child_spawn((const char *const *)argv->pdata, G_SUBPROCESS_FLAGS_NONE, on_exit,
                              m, err);
    g_ptr_array_free(argv, TRUE);
    if (!m->child)
        return false;

    observe(m, OBS_DURATION, "duration");
    observe(m, OBS_PAUSE, "pause");
    observe(m, OBS_TITLE, "media-title");
    observe(m, OBS_TRACKS, "track-list");
    observe(m, OBS_METADATA, "metadata");
    m->connect_elapsed = 0;
    m->connect_timer = g_timeout_add(CONNECT_RETRY_MS, try_connect, m);
    return true;
}

static void load(kp_backend *b, const char *path, double start_at)
{
    mpv *m = (mpv *)b;
    set_property(m, "pause", cJSON_CreateFalse());
    cJSON *cmd = command("loadfile");
    cJSON_AddItemToArray(args_of(cmd), cJSON_CreateString(path));
    cJSON_AddItemToArray(args_of(cmd), cJSON_CreateString("replace"));
    if (start_at > 0) {
        char opt[64], num[G_ASCII_DTOSTR_BUF_SIZE];
        g_snprintf(opt, sizeof opt, "start=%s", g_ascii_formatd(num, sizeof num, "%.3f", start_at));
        cJSON_AddItemToArray(args_of(cmd), cJSON_CreateString(opt));
    }
    send_json(m, cmd);
}

static void stop(kp_backend *b)
{
    mpv *m = (mpv *)b;
    send_json(m, command("stop"));
    m->loaded = false;
}

static void set_pause(kp_backend *b, bool paused)
{
    set_property((mpv *)b, "pause", cJSON_CreateBool(paused));
}

static void seek(kp_backend *b, double seconds, bool relative)
{
    cJSON *cmd = command("seek");
    cJSON_AddItemToArray(args_of(cmd), cJSON_CreateNumber(seconds));
    cJSON_AddItemToArray(args_of(cmd), cJSON_CreateString(relative ? "relative" : "absolute"));
    send_json((mpv *)b, cmd);
}

static void set_volume(kp_backend *b, int volume)
{
    mpv *m = (mpv *)b;
    m->opts.volume = volume;
    set_property(m, "volume", cJSON_CreateNumber(volume));
}

static const kp_backend_ops mpv_ops = {
    .name = "mpv",
    .caps = KP_CAP_VIDEO,
    .start = start,
    .running = running,
    .pid = pid,
    .load = load,
    .stop = stop,
    .set_pause = set_pause,
    .seek = seek,
    .set_volume = set_volume,
    .set_ticking = set_ticking,
    .free = mpv_free,
};

kp_backend *kp_backend_new_mpv(const kp_backend_opts *opts, kp_event_cb cb, void *ud)
{
    mpv *m = g_new0(mpv, 1);
    m->base.ops = &mpv_ops;
    m->opts = *opts;
    m->program = g_strdup(opts->program ? opts->program : "mpv");
    m->extra = g_strdup(opts->extra);
    m->test_args = g_strdupv((char **)opts->test_args);
    m->opts.program = m->program;
    m->opts.extra = m->extra;
    m->opts.device = NULL;
    m->cb = cb;
    m->ud = ud;
    m->io = kp_lineio_new("mpv", on_line, m);
    static int serial;
    /* Short: unix socket paths are limited to 108 bytes. */
    char name[64];
    g_snprintf(name, sizeof name, "kiplay-%d-%d.sock", (int)getpid(), ++serial);
    m->sock_path = g_build_filename(g_get_user_runtime_dir(), name, NULL);
    return &m->base;
}
