/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "instance.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <string.h>
#include <unistd.h>

#include "log.h"

#define PROTOCOL "kiplay 1"
/* A request is a few file names; this only stops a stuck client. */
#define MAX_REQUEST_LINES 100000

struct kp_instance {
    GSocketService *service;
    char *path;
    kp_instance_cb cb;
    void *ud;
};

char *kp_instance_socket_path(const char *config_dir)
{
    /* Short: unix socket paths are limited to 108 bytes. */
    char name[48];
    g_snprintf(name, sizeof name, "kiplay-%08x.sock", g_str_hash(config_dir));
    return g_build_filename(g_get_user_runtime_dir(), name, NULL);
}

static bool has_scheme(const char *s)
{
    char *scheme = g_uri_parse_scheme(s);
    bool yes = scheme != NULL;
    g_free(scheme);
    return yes;
}

char *kp_instance_absolute(const char *location)
{
    if (has_scheme(location) || g_path_is_absolute(location))
        return g_strdup(location);
    char *cwd = g_get_current_dir();
    char *abs = g_build_filename(cwd, location, NULL);
    g_free(cwd);
    /* "./a/../b.mp3" → "/cwd/b.mp3" */
    GFile *f = g_file_new_for_path(abs);
    char *clean = g_file_get_path(f);
    g_object_unref(f);
    g_free(abs);
    return clean;
}

bool kp_instance_forward(const char *socket_path, char **locations, bool enqueue)
{
    GSocketClient *client = g_socket_client_new();
    GSocketAddress *addr = g_unix_socket_address_new(socket_path);
    GSocketConnection *conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), NULL, NULL);
    g_object_unref(addr);
    g_object_unref(client);
    if (!conn)
        return false;
    /* A running Kiplay that hangs must not hang this one too. */
    g_socket_set_timeout(g_socket_connection_get_socket(conn), 5);
    GString *req = g_string_new(PROTOCOL);
    g_string_append(req, enqueue ? " enqueue\n" : " play\n");
    for (char **l = locations; l && *l; l++) {
        char *abs = kp_instance_absolute(*l);
        /* A newline in a name would split it: such names go as URIs. */
        if (strchr(abs, '\n')) {
            char *uri = g_filename_to_uri(abs, NULL, NULL);
            g_free(abs);
            abs = uri;
        }
        if (abs)
            g_string_append_printf(req, "%s\n", abs);
        g_free(abs);
    }
    g_string_append_c(req, '\n');
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    bool ok = g_output_stream_write_all(out, req->str, req->len, NULL, NULL, NULL);
    g_string_free(req, TRUE);
    /* Wait for the other side to close: it has read everything. */
    char c;
    g_input_stream_read(g_io_stream_get_input_stream(G_IO_STREAM(conn)), &c, 1, NULL, NULL);
    g_object_unref(conn);
    return ok;
}

typedef struct {
    kp_instance *in;
    GSocketConnection *conn;
    GDataInputStream *data;
    GPtrArray *lines;
    bool header, enqueue;
} request;

static void request_done(request *r)
{
    g_ptr_array_unref(r->lines);
    g_object_unref(r->data);
    g_io_stream_close(G_IO_STREAM(r->conn), NULL, NULL);
    g_object_unref(r->conn);
    g_free(r);
}

static void on_request_line(GObject *src, GAsyncResult *res, gpointer data)
{
    request *r = data;
    char *line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(src), res, NULL, NULL);
    if (!line) {
        request_done(r); /* closed early, or not UTF-8 */
        return;
    }
    if (!r->header) {
        r->header = true;
        if (strcmp(line, PROTOCOL " play") != 0 && strcmp(line, PROTOCOL " enqueue") != 0) {
            kp_log_debug("instance: bad request \"%s\"", line);
            g_free(line);
            request_done(r);
            return;
        }
        r->enqueue = g_str_has_suffix(line, "enqueue");
        g_free(line);
    } else if (!*line) {
        g_free(line);
        g_ptr_array_add(r->lines, NULL);
        r->in->cb(r->in->ud, (char **)r->lines->pdata, r->enqueue);
        request_done(r);
        return;
    } else if (r->lines->len < MAX_REQUEST_LINES) {
        g_ptr_array_add(r->lines, line);
    } else {
        g_free(line);
    }
    g_data_input_stream_read_line_async(r->data, G_PRIORITY_DEFAULT, NULL, on_request_line, r);
}

static gboolean on_incoming(GSocketService *service, GSocketConnection *conn, GObject *source,
                            gpointer user)
{
    (void)service;
    (void)source;
    request *r = g_new0(request, 1);
    r->in = user;
    r->conn = g_object_ref(conn);
    r->data = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));
    r->lines = g_ptr_array_new_with_free_func(g_free);
    g_data_input_stream_read_line_async(r->data, G_PRIORITY_DEFAULT, NULL, on_request_line, r);
    return TRUE;
}

kp_instance *kp_instance_listen(const char *socket_path, kp_instance_cb cb, void *ud, char **err)
{
    /* Nobody answered on it (or we wouldn't be here): a leftover. */
    unlink(socket_path);
    GSocketService *service = g_socket_service_new();
    GSocketAddress *addr = g_unix_socket_address_new(socket_path);
    GError *gerr = NULL;
    bool ok = g_socket_listener_add_address(G_SOCKET_LISTENER(service), addr, G_SOCKET_TYPE_STREAM,
                                            G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &gerr);
    g_object_unref(addr);
    if (!ok) {
        if (err)
            *err = g_strdup(gerr->message);
        g_error_free(gerr);
        g_object_unref(service);
        return NULL;
    }
    kp_instance *in = g_new0(kp_instance, 1);
    in->service = service;
    in->path = g_strdup(socket_path);
    in->cb = cb;
    in->ud = ud;
    g_signal_connect(service, "incoming", G_CALLBACK(on_incoming), in);
    g_socket_service_start(service);
    return in;
}

void kp_instance_free(kp_instance *in)
{
    if (!in)
        return;
    g_socket_service_stop(in->service);
    g_socket_listener_close(G_SOCKET_LISTENER(in->service));
    g_object_unref(in->service);
    unlink(in->path);
    g_free(in->path);
    g_free(in);
}
