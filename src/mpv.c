/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "backend.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "json.h"
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
};
#define REQ_TIME 100

#define CONNECT_RETRY_MS 30
#define CONNECT_GIVE_UP_MS 10000
#define TICK_MS 250

struct kp_backend {
    kp_backend_opts opts;
    char *program, *extra;
    char **test_args;
    kp_event_cb cb;
    void *ud;

    char *sock_path;
    GSubprocess *proc;
    GCancellable *cancel; /* cancels the waits of the current process */
    GSocketConnection *conn;
    GDataInputStream *in;
    GPollableOutputStream *out;
    GSource *out_source;
    GString *pending; /* bytes not written yet (before connect or when full) */
    guint connect_timer, tick_timer;
    int connect_elapsed;
    bool ticking, loaded;
};

static void emit(kp_backend *b, kp_event_type type, double value, bool flag, const char *text)
{
    kp_event ev = {type, value, flag, text};
    b->cb(b->ud, &ev);
}

/* ---- parsing ------------------------------------------------------------- */

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

void kp_mpv_parse_line(const char *line, kp_event_cb cb, void *ud)
{
    cJSON *j = cJSON_Parse(line);
    if (!j)
        return;
    kp_event ev = {0};
    bool send = false;
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
    cJSON_Delete(j);
}

static void on_parsed(void *ud, const kp_event *ev)
{
    kp_backend *b = ud;
    if (ev->type == KP_EV_LOADED)
        b->loaded = true;
    else if (ev->type == KP_EV_ENDED || ev->type == KP_EV_IDLE)
        b->loaded = false;
    b->cb(b->ud, ev);
}

/* ---- writing ------------------------------------------------------------- */

static void flush(kp_backend *b);

static gboolean on_writable(GObject *stream, gpointer data)
{
    (void)stream;
    kp_backend *b = data;
    g_source_destroy(b->out_source);
    g_source_unref(b->out_source);
    b->out_source = NULL;
    flush(b);
    return G_SOURCE_REMOVE;
}

/* Writes what it can; the rest waits for the socket to drain. */
static void flush(kp_backend *b)
{
    if (!b->out || b->pending->len == 0 || b->out_source)
        return;
    GError *err = NULL;
    gssize n = g_pollable_output_stream_write_nonblocking(b->out, b->pending->str,
                                                          b->pending->len, NULL, &err);
    if (n > 0)
        g_string_erase(b->pending, 0, n);
    if (err) {
        bool again = g_error_matches(err, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK);
        if (!again)
            kp_log_debug("mpv: write: %s", err->message);
        g_error_free(err);
        if (!again)
            return;
    }
    if (b->pending->len > 0) {
        b->out_source = g_pollable_output_stream_create_source(b->out, NULL);
        g_source_set_callback(b->out_source, (GSourceFunc)(void (*)(void))on_writable, b, NULL);
        g_source_attach(b->out_source, NULL);
    }
}

/* Takes ownership of cmd ({"command": [...]}). */
static void send_json(kp_backend *b, cJSON *cmd)
{
    char *text = cJSON_PrintUnformatted(cmd);
    cJSON_Delete(cmd);
    if (!text)
        return;
    g_string_append(b->pending, text);
    g_string_append_c(b->pending, '\n');
    cJSON_free(text);
    flush(b);
}

/* {"command": [args...]}; args are strings unless they look like the
 * markers below. */
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

static void set_property(kp_backend *b, const char *name, cJSON *value)
{
    cJSON *cmd = command("set_property");
    cJSON_AddItemToArray(args_of(cmd), cJSON_CreateString(name));
    cJSON_AddItemToArray(args_of(cmd), value);
    send_json(b, cmd);
}

static void observe(kp_backend *b, int id, const char *name)
{
    cJSON *cmd = command("observe_property");
    cJSON_AddItemToArray(args_of(cmd), cJSON_CreateNumber(id));
    cJSON_AddItemToArray(args_of(cmd), cJSON_CreateString(name));
    send_json(b, cmd);
}

/* ---- reading ------------------------------------------------------------- */

static void read_next(kp_backend *b);

static void on_line(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *err = NULL;
    char *line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(src), res, NULL, &err);
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err);
        return; /* b may be gone */
    }
    kp_backend *b = data;
    if (!line) {
        /* EOF or error: the process is going away; its exit reports it. */
        if (err) {
            kp_log_debug("mpv: read: %s", err->message);
            g_error_free(err);
        }
        return;
    }
    kp_mpv_parse_line(line, on_parsed, b);
    g_free(line);
    read_next(b);
}

static void read_next(kp_backend *b)
{
    g_data_input_stream_read_line_async(b->in, G_PRIORITY_DEFAULT, b->cancel, on_line, b);
}

static gboolean on_tick(gpointer data)
{
    kp_backend *b = data;
    if (b->conn && b->loaded) {
        cJSON *cmd = command("get_property");
        cJSON_AddItemToArray(args_of(cmd), cJSON_CreateString("time-pos"));
        cJSON_AddNumberToObject(cmd, "request_id", REQ_TIME);
        send_json(b, cmd);
    }
    return G_SOURCE_CONTINUE;
}

void kp_backend_set_ticking(kp_backend *b, bool on)
{
    b->ticking = on;
    if (on && !b->tick_timer)
        b->tick_timer = g_timeout_add(TICK_MS, on_tick, b);
    else if (!on && b->tick_timer) {
        g_source_remove(b->tick_timer);
        b->tick_timer = 0;
    }
}

/* ---- the process --------------------------------------------------------- */

static void drop_connection(kp_backend *b)
{
    if (b->connect_timer) {
        g_source_remove(b->connect_timer);
        b->connect_timer = 0;
    }
    if (b->out_source) {
        g_source_destroy(b->out_source);
        g_source_unref(b->out_source);
        b->out_source = NULL;
    }
    g_clear_object(&b->in);
    b->out = NULL;
    if (b->conn) {
        g_io_stream_close(G_IO_STREAM(b->conn), NULL, NULL);
        g_clear_object(&b->conn);
    }
}

static gboolean try_connect(gpointer data)
{
    kp_backend *b = data;
    GSocketClient *client = g_socket_client_new();
    GSocketAddress *addr = g_unix_socket_address_new(b->sock_path);
    /* A unix socket connects (or fails) at once: no waiting here. */
    GSocketConnection *conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), NULL, NULL);
    g_object_unref(addr);
    g_object_unref(client);
    if (!conn) {
        b->connect_elapsed += CONNECT_RETRY_MS;
        if (b->connect_elapsed < CONNECT_GIVE_UP_MS)
            return G_SOURCE_CONTINUE;
        b->connect_timer = 0;
        kp_log_warn("mpv: no IPC socket after %d s", CONNECT_GIVE_UP_MS / 1000);
        if (b->proc)
            g_subprocess_force_exit(b->proc);
        return G_SOURCE_REMOVE;
    }
    b->connect_timer = 0;
    b->conn = conn;
    b->in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));
    b->out = G_POLLABLE_OUTPUT_STREAM(g_io_stream_get_output_stream(G_IO_STREAM(conn)));
    kp_log_debug("mpv: connected after %d ms", b->connect_elapsed);
    read_next(b);
    flush(b);
    return G_SOURCE_REMOVE;
}

static void on_exit(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *err = NULL;
    bool ok = g_subprocess_wait_finish(G_SUBPROCESS(src), res, &err);
    if (!ok && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err);
        return; /* we stopped it on purpose */
    }
    g_clear_error(&err);
    kp_backend *b = data;
    char *why;
    GSubprocess *p = G_SUBPROCESS(src);
    if (g_subprocess_get_if_signaled(p))
        why = g_strdup_printf("signal %d", g_subprocess_get_term_sig(p));
    else
        why = g_strdup_printf("exit status %d", g_subprocess_get_exit_status(p));
    kp_log_debug("mpv: exited (%s)", why);
    drop_connection(b);
    g_cancellable_cancel(b->cancel);
    g_clear_object(&b->cancel);
    g_clear_object(&b->proc);
    g_string_truncate(b->pending, 0);
    b->loaded = false;
    emit(b, KP_EV_CRASHED, 0, false, why);
    g_free(why);
}

kp_backend *kp_backend_new_mpv(const kp_backend_opts *opts, kp_event_cb cb, void *ud)
{
    kp_backend *b = g_new0(kp_backend, 1);
    b->opts = *opts;
    b->program = g_strdup(opts->program ? opts->program : "mpv");
    b->extra = g_strdup(opts->extra);
    b->test_args = g_strdupv((char **)opts->test_args);
    b->opts.program = b->program;
    b->opts.extra = b->extra;
    b->cb = cb;
    b->ud = ud;
    b->pending = g_string_new(NULL);
    static int serial;
    /* Short: unix socket paths are limited to 108 bytes. */
    char name[64];
    g_snprintf(name, sizeof name, "kiplay-%d-%d.sock", (int)getpid(), ++serial);
    b->sock_path = g_build_filename(g_get_user_runtime_dir(), name, NULL);
    return b;
}

static void shutdown_process(kp_backend *b)
{
    if (!b->proc)
        return;
    if (b->conn) {
        send_json(b, command("quit"));
    }
    if (b->cancel)
        g_cancellable_cancel(b->cancel);
    drop_connection(b);
    /* mpv quits on SIGTERM too; nobody waits for it after this. */
    g_subprocess_send_signal(b->proc, SIGTERM);
    g_clear_object(&b->proc);
    g_clear_object(&b->cancel);
    g_string_truncate(b->pending, 0);
    b->loaded = false;
    unlink(b->sock_path);
}

void kp_backend_free(kp_backend *b)
{
    if (!b)
        return;
    kp_backend_set_ticking(b, false);
    shutdown_process(b);
    g_string_free(b->pending, TRUE);
    g_free(b->sock_path);
    g_free(b->program);
    g_free(b->extra);
    g_strfreev(b->test_args);
    g_free(b);
}

bool kp_backend_running(const kp_backend *b)
{
    return b->proc != NULL;
}

int kp_backend_pid(const kp_backend *b)
{
    const char *id = b->proc ? g_subprocess_get_identifier(b->proc) : NULL;
    return id ? atoi(id) : 0;
}

bool kp_backend_start(kp_backend *b, char **err)
{
    if (b->proc)
        return true;
    unlink(b->sock_path);
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(argv, g_strdup(b->program));
    if (!b->opts.use_config)
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
    g_ptr_array_add(argv, g_strdup_printf("--input-ipc-server=%s", b->sock_path));
    g_ptr_array_add(argv, g_strdup_printf("--volume=%d", b->opts.volume));
    if (b->opts.wid)
        g_ptr_array_add(argv, g_strdup_printf("--wid=%lu", b->opts.wid));
    for (char **t = b->test_args; t && *t; t++)
        g_ptr_array_add(argv, g_strdup(*t));
    if (b->extra && *b->extra) {
        int n;
        char **extra;
        GError *gerr = NULL;
        if (g_shell_parse_argv(b->extra, &n, &extra, &gerr)) {
            for (int i = 0; i < n; i++)
                g_ptr_array_add(argv, extra[i]);
            g_free(extra); /* the strings moved into argv */
        } else {
            kp_log_warn("mpv: extra options: %s", gerr->message);
            g_error_free(gerr);
        }
    }
    g_ptr_array_add(argv, NULL);

    GError *gerr = NULL;
    b->proc = g_subprocess_newv((const char *const *)argv->pdata, G_SUBPROCESS_FLAGS_NONE, &gerr);
    if (kp_log_debug_enabled()) {
        char *line = g_strjoinv(" ", (char **)argv->pdata);
        kp_log_debug("mpv: %s", line);
        g_free(line);
    }
    g_ptr_array_free(argv, TRUE);
    if (!b->proc) {
        if (err)
            *err = g_strdup(gerr->message);
        g_error_free(gerr);
        return false;
    }
    b->cancel = g_cancellable_new();
    g_subprocess_wait_async(b->proc, b->cancel, on_exit, b);

    observe(b, OBS_DURATION, "duration");
    observe(b, OBS_PAUSE, "pause");
    observe(b, OBS_TITLE, "media-title");
    observe(b, OBS_TRACKS, "track-list");
    b->connect_elapsed = 0;
    b->connect_timer = g_timeout_add(CONNECT_RETRY_MS, try_connect, b);
    return true;
}

void kp_backend_load(kp_backend *b, const char *path, double start)
{
    set_property(b, "pause", cJSON_CreateFalse());
    cJSON *cmd = command("loadfile");
    cJSON_AddItemToArray(args_of(cmd), cJSON_CreateString(path));
    cJSON_AddItemToArray(args_of(cmd), cJSON_CreateString("replace"));
    if (start > 0) {
        char opt[64];
        g_snprintf(opt, sizeof opt, "start=%.3f", start);
        /* mpv wants a dot whatever the locale. */
        for (char *c = opt; *c; c++)
            if (*c == ',')
                *c = '.';
        cJSON_AddItemToArray(args_of(cmd), cJSON_CreateString(opt));
    }
    send_json(b, cmd);
}

void kp_backend_stop(kp_backend *b)
{
    send_json(b, command("stop"));
    b->loaded = false;
}

void kp_backend_set_pause(kp_backend *b, bool paused)
{
    set_property(b, "pause", cJSON_CreateBool(paused));
}

static void seek(kp_backend *b, double seconds, const char *how)
{
    cJSON *cmd = command("seek");
    cJSON_AddItemToArray(args_of(cmd), cJSON_CreateNumber(seconds));
    cJSON_AddItemToArray(args_of(cmd), cJSON_CreateString(how));
    send_json(b, cmd);
}

void kp_backend_seek(kp_backend *b, double seconds)
{
    seek(b, seconds, "absolute");
}

void kp_backend_seek_relative(kp_backend *b, double seconds)
{
    seek(b, seconds, "relative");
}

void kp_backend_set_volume(kp_backend *b, int volume)
{
    b->opts.volume = volume;
    set_property(b, "volume", cJSON_CreateNumber(volume));
}
