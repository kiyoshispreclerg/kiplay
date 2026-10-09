/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Phase 0 prototype: mpv drawing into a GtkDrawingArea (--wid), driven over
 * its JSON IPC socket from the GLib main loop. Shows the playback time in a
 * label, Space pauses, q quits. Used to measure our memory next to mpv's.
 *
 *   embed [--sub FILE] VIDEO */
#include <gdk/gdkkeysyms.h>
#include <gdk/gdkx.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    GtkWidget *label;
    GPid mpv;
    char *sock_path;
    GSocketConnection *conn;
    GDataInputStream *in;
    const char *video, *sub;
} app;

/* JSON string body: only quotes, backslashes and control bytes need
 * escaping; UTF-8 passes as is (g_strescape would turn it into octal). */
static char *json_escape(const char *s)
{
    GString *o = g_string_new(NULL);
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if (*c == '"' || *c == '\\')
            g_string_append_printf(o, "\\%c", *c);
        else if (*c < 0x20)
            g_string_append_printf(o, "\\u%04x", *c);
        else
            g_string_append_c(o, (char)*c);
    }
    return g_string_free(o, FALSE);
}

static void send_cmd(app *a, const char *json)
{
    if (!a->conn)
        return;
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(a->conn));
    g_output_stream_write_all(out, json, strlen(json), NULL, NULL, NULL);
    g_output_stream_write_all(out, "\n", 1, NULL, NULL, NULL);
}

static void on_line(GObject *src, GAsyncResult *res, gpointer data)
{
    app *a = data;
    gsize len;
    char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(src), res, &len, NULL);
    if (!line) {
        gtk_label_set_text(GTK_LABEL(a->label), "mpv saiu");
        return;
    }
    /* Good enough for a prototype: the real thing parses with cJSON. */
    const char *p = strstr(line, "\"name\":\"time-pos\",\"data\":");
    if (p) {
        double t = g_ascii_strtod(p + strlen("\"name\":\"time-pos\",\"data\":"), NULL);
        char buf[64];
        snprintf(buf, sizeof(buf), "%02d:%05.2f", (int)t / 60, t - 60 * ((int)t / 60));
        gtk_label_set_text(GTK_LABEL(a->label), buf);
    }
    g_free(line);
    g_data_input_stream_read_line_async(a->in, G_PRIORITY_DEFAULT, NULL, on_line, a);
}

static gboolean try_connect(gpointer data)
{
    app *a = data;
    GSocketClient *c = g_socket_client_new();
    GSocketAddress *addr = g_unix_socket_address_new(a->sock_path);
    a->conn = g_socket_client_connect(c, G_SOCKET_CONNECTABLE(addr), NULL, NULL);
    g_object_unref(addr);
    g_object_unref(c);
    if (!a->conn)
        return G_SOURCE_CONTINUE; /* mpv not listening yet */
    a->in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(a->conn)));
    send_cmd(a, "{\"command\":[\"observe_property\",1,\"time-pos\"]}");
    char *esc = json_escape(a->video);
    char *cmd = g_strdup_printf("{\"command\":[\"loadfile\",\"%s\"]}", esc);
    send_cmd(a, cmd);
    g_free(cmd);
    g_free(esc);
    if (a->sub) {
        esc = json_escape(a->sub);
        cmd = g_strdup_printf("{\"command\":[\"set_property\",\"sub-files\",[\"%s\"]]}", esc);
        send_cmd(a, cmd);
        g_free(cmd);
        g_free(esc);
    }
    g_data_input_stream_read_line_async(a->in, G_PRIORITY_DEFAULT, NULL, on_line, a);
    return G_SOURCE_REMOVE;
}

static void on_realize(GtkWidget *w, gpointer data)
{
    app *a = data;
    char wid[32];
    snprintf(wid, sizeof(wid), "--wid=%lu", (unsigned long)GDK_WINDOW_XID(w->window));
    char *ipc = g_strdup_printf("--input-ipc-server=%s", a->sock_path);
    char *argv[] = {"mpv", "--idle=yes", "--no-config", "--no-terminal",
                    "--no-input-default-bindings", "--input-vo-keyboard=no",
                    "--osc=no", "--keep-open=yes", wid, ipc,
                    /* KP_MPV_AO=null in tests keeps the speakers quiet. */
                    g_getenv("KP_MPV_AO") ? "--ao=null" : NULL, NULL};
    GError *err = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                       NULL, NULL, &a->mpv, &err)) {
        fprintf(stderr, "mpv: %s\n", err->message);
        g_error_free(err);
    }
    g_free(ipc);
    g_timeout_add(50, try_connect, a);
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    app *a = data;
    if (ev->keyval == GDK_space)
        send_cmd(a, "{\"command\":[\"cycle\",\"pause\"]}");
    else if (ev->keyval == GDK_q)
        gtk_main_quit();
    return TRUE;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    app a = {0};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sub") && i + 1 < argc)
            a.sub = argv[++i];
        else
            a.video = argv[i];
    }
    if (!a.video) {
        fprintf(stderr, "uso: embed [--sub ARQ] VIDEO\n");
        return 2;
    }
    a.sock_path = g_strdup_printf("%s/kp-embed-%d.sock", g_get_user_runtime_dir(), (int)getpid());

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(win), 960, 600);
    GtkWidget *box = gtk_vbox_new(FALSE, 0);
    GtkWidget *video = gtk_drawing_area_new();
    GdkColor black = {0, 0, 0, 0};
    gtk_widget_modify_bg(video, GTK_STATE_NORMAL, &black);
    a.label = gtk_label_new("--:--");
    gtk_box_pack_start(GTK_BOX(box), video, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), a.label, FALSE, FALSE, 4);
    gtk_container_add(GTK_CONTAINER(win), box);
    g_signal_connect(video, "realize", G_CALLBACK(on_realize), &a);
    g_signal_connect(win, "key-press-event", G_CALLBACK(on_key), &a);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    gtk_widget_show_all(win);
    gtk_main();

    send_cmd(&a, "{\"command\":[\"quit\"]}");
    if (a.mpv)
        waitpid(a.mpv, NULL, 0);
    unlink(a.sock_path);
    g_free(a.sock_path);
    return 0;
}
