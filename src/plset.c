/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "plset.h"

#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int id;
    kp_playlist *pl;
    bool is_new; /* never saved: write it even if empty */
} slot;

struct kp_plset {
    char *dir;
    slot *items;
    size_t n, cap;
};

static void push(kp_plset *s, size_t at, int id, kp_playlist *pl, bool is_new)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->items = g_renew(slot, s->items, s->cap);
    }
    if (at > s->n)
        at = s->n;
    memmove(&s->items[at + 1], &s->items[at], (s->n - at) * sizeof(slot));
    s->items[at] = (slot){id, pl, is_new};
    s->n++;
}

static char *file_for(const kp_plset *s, int id)
{
    char name[32];
    g_snprintf(name, sizeof name, "%d.m3u8", id);
    return g_build_filename(s->dir, name, NULL);
}

/* "12.m3u8" -> 12; -1 for anything else. */
static int id_of(const char *file)
{
    char *end;
    long v = strtol(file, &end, 10);
    if (end == file || strcmp(end, ".m3u8") != 0 || v < 0 || v > 1000000)
        return -1;
    return (int)v;
}

static int cmp_slot(const void *a, const void *b)
{
    return ((const slot *)a)->id - ((const slot *)b)->id;
}

kp_plset *kp_plset_load(const char *dir, const char *order)
{
    kp_plset *s = g_new0(kp_plset, 1);
    s->dir = g_strdup(dir);
    GDir *d = g_dir_open(dir, 0, NULL);
    if (d) {
        const char *f;
        while ((f = g_dir_read_name(d))) {
            int id = id_of(f);
            if (id < 0)
                continue;
            char *path = g_build_filename(dir, f, NULL);
            kp_playlist *pl = kp_playlist_load_saved(path, f);
            g_free(path);
            if (pl)
                push(s, s->n, id, pl, false);
        }
        g_dir_close(d);
    }
    if (s->n > 1)
        qsort(s->items, s->n, sizeof(slot), cmp_slot);

    /* Bring the ids named in order to the front, in that order. */
    size_t placed = 0;
    char **ids = g_strsplit(order ? order : "", ",", -1);
    for (char **p = ids; *p; p++) {
        char *end;
        long id = strtol(*p, &end, 10);
        if (end == *p)
            continue;
        long at = kp_plset_find(s, (int)id);
        if (at >= (long)placed) {
            kp_plset_move(s, (size_t)at, placed);
            placed++;
        }
    }
    g_strfreev(ids);
    return s;
}

void kp_plset_free(kp_plset *s)
{
    if (!s)
        return;
    for (size_t i = 0; i < s->n; i++)
        kp_playlist_free(s->items[i].pl);
    g_free(s->items);
    g_free(s->dir);
    g_free(s);
}

size_t kp_plset_count(const kp_plset *s)
{
    return s->n;
}

kp_playlist *kp_plset_get(const kp_plset *s, size_t i)
{
    return i < s->n ? s->items[i].pl : NULL;
}

int kp_plset_id(const kp_plset *s, size_t i)
{
    return i < s->n ? s->items[i].id : -1;
}

long kp_plset_find(const kp_plset *s, int id)
{
    for (size_t i = 0; i < s->n; i++)
        if (s->items[i].id == id)
            return (long)i;
    return -1;
}

size_t kp_plset_insert(kp_plset *s, size_t at, const char *name)
{
    int id = 1;
    for (size_t i = 0; i < s->n; i++)
        if (s->items[i].id >= id)
            id = s->items[i].id + 1;
    if (at > s->n)
        at = s->n;
    push(s, at, id, kp_playlist_new(name), true);
    return at;
}

void kp_plset_remove(kp_plset *s, size_t i)
{
    if (i >= s->n)
        return;
    char *path = file_for(s, s->items[i].id);
    g_remove(path);
    /* The backup too, or it would be the only thing left of the list. */
    char *bak = g_strconcat(path, ".backup", NULL);
    g_remove(bak);
    g_free(bak);
    g_free(path);
    kp_playlist_free(s->items[i].pl);
    memmove(&s->items[i], &s->items[i + 1], (s->n - i - 1) * sizeof(slot));
    s->n--;
}

void kp_plset_move(kp_plset *s, size_t from, size_t to)
{
    if (from >= s->n || to >= s->n || from == to)
        return;
    slot tmp = s->items[from];
    if (from < to)
        memmove(&s->items[from], &s->items[from + 1], (to - from) * sizeof(slot));
    else
        memmove(&s->items[to + 1], &s->items[to], (from - to) * sizeof(slot));
    s->items[to] = tmp;
}

char *kp_plset_order(const kp_plset *s)
{
    GString *out = g_string_new(NULL);
    for (size_t i = 0; i < s->n; i++)
        g_string_append_printf(out, i ? ",%d" : "%d", s->items[i].id);
    return g_string_free(out, FALSE);
}

int kp_plset_save(kp_plset *s)
{
    int ret = 0, saved_errno = 0;
    for (size_t i = 0; i < s->n; i++) {
        slot *it = &s->items[i];
        if (!it->is_new && !kp_playlist_dirty(it->pl))
            continue;
        char *path = file_for(s, it->id);
        if (kp_playlist_save_kept(it->pl, path) == 0) {
            kp_playlist_mark_saved(it->pl);
            it->is_new = false;
        } else {
            ret = -1;
            saved_errno = errno;
        }
        g_free(path);
    }
    if (ret)
        errno = saved_errno;
    return ret;
}
