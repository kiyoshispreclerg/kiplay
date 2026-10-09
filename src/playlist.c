/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "playlist.h"

#include <errno.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>

struct kp_playlist {
    char *name;
    GStringChunk *strings;
    kp_entry *items;
    size_t n, cap;
    bool dirty;
};

kp_playlist *kp_playlist_new(const char *name)
{
    kp_playlist *pl = g_new0(kp_playlist, 1);
    pl->name = g_strdup(name ? name : "");
    pl->strings = g_string_chunk_new(4096);
    return pl;
}

void kp_playlist_free(kp_playlist *pl)
{
    if (!pl)
        return;
    g_free(pl->name);
    g_string_chunk_free(pl->strings);
    g_free(pl->items);
    g_free(pl);
}

const char *kp_playlist_name(const kp_playlist *pl)
{
    return pl->name;
}

void kp_playlist_set_name(kp_playlist *pl, const char *name)
{
    if (strcmp(pl->name, name) == 0)
        return;
    g_free(pl->name);
    pl->name = g_strdup(name);
    pl->dirty = true;
}

size_t kp_playlist_count(const kp_playlist *pl)
{
    return pl->n;
}

const kp_entry *kp_playlist_get(const kp_playlist *pl, size_t i)
{
    return i < pl->n ? &pl->items[i] : NULL;
}

void kp_playlist_add(kp_playlist *pl, const char *location, const char *title,
                     int64_t length_ms)
{
    if (pl->n == pl->cap) {
        pl->cap = pl->cap ? pl->cap * 2 : 64;
        pl->items = g_renew(kp_entry, pl->items, pl->cap);
    }
    kp_entry *e = &pl->items[pl->n++];
    e->location = g_string_chunk_insert(pl->strings, location);
    e->title = title && *title ? g_string_chunk_insert(pl->strings, title) : NULL;
    e->length_ms = length_ms;
    pl->dirty = true;
}

void kp_playlist_remove(kp_playlist *pl, size_t i)
{
    if (i >= pl->n)
        return;
    memmove(&pl->items[i], &pl->items[i + 1], (pl->n - i - 1) * sizeof(kp_entry));
    pl->n--;
    pl->dirty = true;
}

void kp_playlist_clear(kp_playlist *pl)
{
    if (pl->n == 0)
        return;
    pl->n = 0;
    g_string_chunk_clear(pl->strings);
    pl->dirty = true;
}

int64_t kp_playlist_total_ms(const kp_playlist *pl, size_t *unknown)
{
    int64_t total = 0;
    size_t u = 0;
    for (size_t i = 0; i < pl->n; i++) {
        if (pl->items[i].length_ms >= 0)
            total += pl->items[i].length_ms;
        else
            u++;
    }
    if (unknown)
        *unknown = u;
    return total;
}

static bool has_scheme(const char *s)
{
    /* RFC 3986: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":" */
    if (!g_ascii_isalpha(*s))
        return false;
    for (const char *c = s; *c; c++) {
        if (*c == ':')
            return c[1] == '/';
        if (!g_ascii_isalnum(*c) && *c != '+' && *c != '-' && *c != '.')
            return false;
    }
    return false;
}

kp_playlist *kp_playlist_parse_m3u(const char *text, const char *dir,
                                   const char *default_name)
{
    kp_playlist *pl = kp_playlist_new(default_name);
    char *title = NULL;
    int64_t length = -1;
    const char *p = text;
    /* A UTF-8 BOM is common in .m3u8 written on Windows. */
    if (!strncmp(p, "\xEF\xBB\xBF", 3))
        p += 3;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char *line = g_strndup(p, len);
        g_strstrip(line);
        if (g_str_has_prefix(line, "#PLAYLIST:")) {
            g_free(pl->name);
            pl->name = g_strdup(g_strstrip(line + strlen("#PLAYLIST:")));
        } else if (g_str_has_prefix(line, "#EXTINF:")) {
            char *comma = strchr(line, ',');
            length = -1;
            char *num_end;
            long long secs = strtoll(line + strlen("#EXTINF:"), &num_end, 10);
            if (num_end != line + strlen("#EXTINF:") && secs >= 0)
                length = secs * 1000;
            g_free(title);
            title = comma ? g_strdup(g_strstrip(comma + 1)) : NULL;
        } else if (line[0] && line[0] != '#') {
            char *loc;
            if (line[0] == '/' || has_scheme(line) || !dir)
                loc = g_strdup(line);
            else
                loc = g_build_filename(dir, line, NULL);
            kp_playlist_add(pl, loc, title, length);
            g_free(loc);
            g_free(title);
            title = NULL;
            length = -1;
        }
        g_free(line);
        p = end ? end + 1 : p + len;
    }
    g_free(title);
    pl->dirty = false;
    return pl;
}

kp_playlist *kp_playlist_load(const char *path, const char *default_name)
{
    char *text;
    if (!g_file_get_contents(path, &text, NULL, NULL)) {
        errno = ENOENT;
        return NULL;
    }
    char *dir = g_path_get_dirname(path);
    kp_playlist *pl = kp_playlist_parse_m3u(text, dir, default_name);
    g_free(dir);
    g_free(text);
    return pl;
}

char *kp_playlist_dump(const kp_playlist *pl)
{
    GString *out = g_string_new("#EXTM3U\n");
    /* The name is one line: a newline in it would end the header early. */
    char *name = g_strdelimit(g_strdup(pl->name), "\r\n", ' ');
    g_string_append_printf(out, "#PLAYLIST:%s\n", name);
    g_free(name);
    for (size_t i = 0; i < pl->n; i++) {
        const kp_entry *e = &pl->items[i];
        if (e->title || e->length_ms >= 0) {
            char *t = g_strdelimit(g_strdup(e->title ? e->title : ""), "\r\n", ' ');
            g_string_append_printf(out, "#EXTINF:%lld,%s\n",
                                   e->length_ms >= 0 ? (long long)(e->length_ms / 1000) : -1LL, t);
            g_free(t);
        }
        g_string_append_printf(out, "%s\n", e->location);
    }
    return g_string_free(out, FALSE);
}

int kp_playlist_save(const kp_playlist *pl, const char *path)
{
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    char *text = kp_playlist_dump(pl);
    GError *err = NULL;
    /* g_file_set_contents writes a temp file and renames it over. */
    bool ok = g_file_set_contents(path, text, -1, &err);
    g_free(text);
    if (!ok) {
        errno = EIO;
        g_error_free(err);
        return -1;
    }
    return 0;
}

bool kp_playlist_dirty(const kp_playlist *pl)
{
    return pl->dirty;
}

void kp_playlist_mark_saved(kp_playlist *pl)
{
    pl->dirty = false;
}
