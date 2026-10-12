/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lineio.h"

#include "log.h"

struct kp_lineio {
    char *name; /* for the debug log */
    kp_line_cb cb;
    void *ud;
    GDataInputStream *in;
    GPollableOutputStream *out;
    GSource *out_source;
    GCancellable *cancel;
    GString *pending;
    unsigned gen; /* bumped by attach/detach, so a read never outlives its streams */
};

kp_lineio *kp_lineio_new(const char *name, kp_line_cb cb, void *ud)
{
    kp_lineio *io = g_new0(kp_lineio, 1);
    io->name = g_strdup(name);
    io->cb = cb;
    io->ud = ud;
    io->pending = g_string_new(NULL);
    return io;
}

void kp_lineio_detach(kp_lineio *io)
{
    io->gen++;
    if (io->cancel) {
        g_cancellable_cancel(io->cancel);
        g_clear_object(&io->cancel);
    }
    if (io->out_source) {
        g_source_destroy(io->out_source);
        g_source_unref(io->out_source);
        io->out_source = NULL;
    }
    g_clear_object(&io->in);
    g_clear_object(&io->out);
    g_string_truncate(io->pending, 0);
}

void kp_lineio_free(kp_lineio *io)
{
    if (!io)
        return;
    kp_lineio_detach(io);
    g_string_free(io->pending, TRUE);
    g_free(io->name);
    g_free(io);
}

static void flush(kp_lineio *io);

static gboolean on_writable(GObject *stream, gpointer data)
{
    (void)stream;
    kp_lineio *io = data;
    g_source_destroy(io->out_source);
    g_source_unref(io->out_source);
    io->out_source = NULL;
    flush(io);
    return G_SOURCE_REMOVE;
}

/* Writes what it can; the rest waits for the other side to drain. */
static void flush(kp_lineio *io)
{
    if (!io->out || io->pending->len == 0 || io->out_source)
        return;
    GError *err = NULL;
    gssize n = g_pollable_output_stream_write_nonblocking(io->out, io->pending->str,
                                                          io->pending->len, NULL, &err);
    if (n > 0)
        g_string_erase(io->pending, 0, n);
    if (err) {
        bool again = g_error_matches(err, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK);
        if (!again)
            kp_log_debug("%s: write: %s", io->name, err->message);
        g_error_free(err);
        if (!again)
            return;
    }
    if (io->pending->len > 0) {
        io->out_source = g_pollable_output_stream_create_source(io->out, NULL);
        g_source_set_callback(io->out_source, (GSourceFunc)(void (*)(void))on_writable, io, NULL);
        g_source_attach(io->out_source, NULL);
    }
}

void kp_lineio_send(kp_lineio *io, const char *line)
{
    g_string_append(io->pending, line);
    g_string_append_c(io->pending, '\n');
    flush(io);
}

static void read_next(kp_lineio *io);

static void on_line(GObject *src, GAsyncResult *res, gpointer data)
{
    GError *err = NULL;
    char *line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(src), res, NULL, &err);
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err);
        return; /* io may be gone */
    }
    kp_lineio *io = data;
    if (!line) {
        if (err) {
            kp_log_debug("%s: read: %s", io->name, err->message);
            g_error_free(err);
        }
        io->cb(io->ud, NULL);
        return;
    }
    unsigned gen = io->gen;
    io->cb(io->ud, line);
    g_free(line);
    /* The callback may have detached (or re-attached) us. */
    if (io->gen == gen)
        read_next(io);
}

static void read_next(kp_lineio *io)
{
    g_data_input_stream_read_line_async(io->in, G_PRIORITY_DEFAULT, io->cancel, on_line, io);
}

void kp_lineio_attach(kp_lineio *io, GInputStream *in, GOutputStream *out)
{
    io->gen++;
    io->cancel = g_cancellable_new();
    io->in = g_data_input_stream_new(in);
    io->out = G_POLLABLE_OUTPUT_STREAM(g_object_ref(out));
    read_next(io);
    flush(io);
}
