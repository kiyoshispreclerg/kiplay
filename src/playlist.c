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
    kp_playlist_type type;
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

kp_playlist_type kp_playlist_get_type(const kp_playlist *pl)
{
    return pl->type;
}

void kp_playlist_set_type(kp_playlist *pl, kp_playlist_type type)
{
    if (pl->type == type)
        return;
    pl->type = type;
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

static const char *intern(kp_playlist *pl, const char *s)
{
    return s && *s ? g_string_chunk_insert(pl->strings, s) : NULL;
}

void kp_playlist_insert(kp_playlist *pl, size_t at, const char *location, const kp_tags *tags)
{
    if (pl->n == pl->cap) {
        pl->cap = pl->cap ? pl->cap * 2 : 64;
        pl->items = g_renew(kp_entry, pl->items, pl->cap);
    }
    if (at > pl->n)
        at = pl->n;
    memmove(&pl->items[at + 1], &pl->items[at], (pl->n - at) * sizeof(kp_entry));
    pl->n++;
    kp_entry *e = &pl->items[at];
    *e = (kp_entry){.location = g_string_chunk_insert(pl->strings, location), .length_ms = -1};
    if (tags) {
        e->title = intern(pl, tags->title);
        e->artist = e->title ? intern(pl, tags->artist) : NULL;
        e->album = e->title ? intern(pl, tags->album) : NULL;
        e->length_ms = tags->length_ms >= 0 ? tags->length_ms : -1;
    }
    pl->dirty = true;
}

void kp_playlist_add(kp_playlist *pl, const char *location, const kp_tags *tags)
{
    kp_playlist_insert(pl, pl->n, location, tags);
}

static bool same(const char *a, const char *b)
{
    if (b && !*b)
        b = NULL;
    return a == b || (a && b && strcmp(a, b) == 0);
}

bool kp_playlist_set_tags(kp_playlist *pl, size_t i, const kp_tags *t)
{
    if (i >= pl->n)
        return false;
    kp_entry *e = &pl->items[i];
    bool changed = false;
    if (t->title && *t->title &&
        (!same(e->title, t->title) || !same(e->artist, t->artist) || !same(e->album, t->album))) {
        e->title = intern(pl, t->title);
        e->artist = intern(pl, t->artist);
        e->album = intern(pl, t->album);
        changed = true;
    }
    /* m3u keeps whole seconds: compare at that precision, or every play
     * would rewrite the file. */
    if (t->length_ms >= 0 && (e->length_ms < 0 || e->length_ms / 1000 != t->length_ms / 1000)) {
        e->length_ms = t->length_ms;
        changed = true;
    }
    if (changed)
        pl->dirty = true;
    return changed;
}

void kp_playlist_set_scanned(kp_playlist *pl, size_t i)
{
    if (i < pl->n)
        pl->items[i].scanned = true;
}

size_t kp_playlist_move(kp_playlist *pl, const size_t *rows, size_t n, size_t to, int *new_order)
{
    if (to > pl->n)
        to = pl->n;
    /* Old indexes in their new order: the others before "to", the moved
     * ones, the others from "to" on. */
    size_t *order = g_new(size_t, pl->n + 1);
    bool *moving = g_new0(bool, pl->n + 1);
    size_t k = 0;
    for (size_t j = 0; j < n; j++)
        if (rows[j] < pl->n)
            moving[rows[j]] = true;
    for (size_t i = 0; i < to; i++)
        if (!moving[i])
            order[k++] = i;
    size_t first = k;
    for (size_t i = 0; i < pl->n; i++)
        if (moving[i])
            order[k++] = i;
    for (size_t i = to; i < pl->n; i++)
        if (!moving[i])
            order[k++] = i;
    kp_entry *items = g_new(kp_entry, pl->cap ? pl->cap : 1);
    bool moved = false;
    for (size_t i = 0; i < pl->n; i++) {
        items[i] = pl->items[order[i]];
        moved |= order[i] != i;
        if (new_order)
            new_order[i] = (int)order[i];
    }
    g_free(pl->items);
    pl->items = items;
    if (moved)
        pl->dirty = true;
    g_free(order);
    g_free(moving);
    return first;
}

void kp_playlist_remove(kp_playlist *pl, size_t i)
{
    if (i >= pl->n)
        return;
    memmove(&pl->items[i], &pl->items[i + 1], (pl->n - i - 1) * sizeof(kp_entry));
    pl->n--;
    pl->dirty = true;
}

void kp_playlist_remove_rows(kp_playlist *pl, const size_t *rows, size_t n)
{
    size_t k = 0, r = 0;
    for (size_t i = 0; i < pl->n; i++) {
        if (r < n && rows[r] == i) {
            r++;
            continue;
        }
        pl->items[k++] = pl->items[i];
    }
    if (k != pl->n)
        pl->dirty = true;
    pl->n = k;
}

long kp_playlist_find_entry(const kp_playlist *pl, const char *location, long hint)
{
    if (hint >= 0 && (size_t)hint < pl->n && pl->items[hint].location == location)
        return hint;
    for (size_t i = 0; i < pl->n; i++)
        if (pl->items[i].location == location)
            return (long)i;
    return -1;
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

/* A location as written in a playlist file, made absolute. */
static char *resolve(const char *line, const char *dir)
{
    if (line[0] == '/' || has_scheme(line) || !dir)
        return g_strdup(line);
    return g_build_filename(dir, line, NULL);
}

/* Calls fn for each line, stripped, without the BOM and the CR. */
static void each_line(const char *text, void (*fn)(char *line, void *ud), void *ud)
{
    const char *p = text;
    /* A UTF-8 BOM is common in .m3u8 written on Windows. */
    if (!strncmp(p, "\xEF\xBB\xBF", 3))
        p += 3;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char *line = g_strndup(p, len);
        fn(g_strstrip(line), ud);
        g_free(line);
        p = end ? end + 1 : p + len;
    }
}

typedef struct {
    kp_playlist *pl;
    const char *dir;
    char *title, *artist, *album;
    int64_t length;
} m3u_state;

static void m3u_reset(m3u_state *st)
{
    g_clear_pointer(&st->title, g_free);
    g_clear_pointer(&st->artist, g_free);
    g_clear_pointer(&st->album, g_free);
    st->length = -1;
}

static char *field(const char *line, const char *prefix)
{
    return g_strstrip(g_strdup(line + strlen(prefix)));
}

static void m3u_line(char *line, void *ud)
{
    m3u_state *st = ud;
    if (g_str_has_prefix(line, "#PLAYLIST:")) {
        g_free(st->pl->name);
        st->pl->name = field(line, "#PLAYLIST:");
    } else if (g_str_has_prefix(line, "#KIPLAY-TYPE:")) {
        char *type = field(line, "#KIPLAY-TYPE:");
        st->pl->type = !strcmp(type, "audio") ? KP_PLAYLIST_AUDIO : KP_PLAYLIST_MIXED;
        g_free(type);
    } else if (g_str_has_prefix(line, "#EXTINF:")) {
        const char *num = line + strlen("#EXTINF:");
        char *num_end;
        long long secs = strtoll(num, &num_end, 10);
        st->length = num_end != num && secs >= 0 ? secs * 1000 : -1;
        char *comma = strchr(line, ',');
        g_free(st->title);
        st->title = comma ? g_strstrip(g_strdup(comma + 1)) : NULL;
    } else if (g_str_has_prefix(line, "#EXTART:")) {
        g_free(st->artist);
        st->artist = field(line, "#EXTART:");
    } else if (g_str_has_prefix(line, "#EXTALB:")) {
        g_free(st->album);
        st->album = field(line, "#EXTALB:");
    } else if (line[0] && line[0] != '#') {
        /* "#EXTINF:..,Artist - Title" with #EXTART: the title alone. */
        const char *title = st->title;
        size_t alen = st->artist ? strlen(st->artist) : 0;
        if (title && alen && !strncmp(title, st->artist, alen) && !strncmp(title + alen, " - ", 3) &&
            title[alen + 3])
            title += alen + 3;
        kp_tags t = {title, st->artist, st->album, st->length};
        char *loc = resolve(line, st->dir);
        kp_playlist_add(st->pl, loc, &t);
        g_free(loc);
        m3u_reset(st);
    }
}

kp_playlist *kp_playlist_parse_m3u(const char *text, const char *dir,
                                   const char *default_name)
{
    m3u_state st = {.pl = kp_playlist_new(default_name), .dir = dir, .length = -1};
    each_line(text, m3u_line, &st);
    m3u_reset(&st);
    st.pl->dirty = false;
    return st.pl;
}

/* PLS keys are numbered and may come in any order: gathered by number. */
typedef struct {
    long num;
    char *file, *title;
    int64_t length;
} pls_item;

typedef struct {
    GArray *items; /* pls_item */
    char *name;
} pls_state;

static pls_item *pls_get(pls_state *st, long num)
{
    for (guint i = 0; i < st->items->len; i++) {
        pls_item *it = &g_array_index(st->items, pls_item, i);
        if (it->num == num)
            return it;
    }
    pls_item it = {.num = num, .length = -1};
    g_array_append_val(st->items, it);
    return &g_array_index(st->items, pls_item, st->items->len - 1);
}

static void pls_line(char *line, void *ud)
{
    pls_state *st = ud;
    char *eq = strchr(line, '=');
    if (!eq || line[0] == '[' || line[0] == ';')
        return;
    *eq = '\0';
    const char *key = g_strstrip(line), *value = eq + 1;
    if (!g_ascii_strcasecmp(key, "X-GNOME-Title") || !g_ascii_strcasecmp(key, "PlaylistName")) {
        g_free(st->name);
        st->name = g_strdup(value);
        return;
    }
    static const char *const keys[] = {"File", "Title", "Length"};
    for (int k = 0; k < 3; k++) {
        size_t kl = strlen(keys[k]);
        if (g_ascii_strncasecmp(key, keys[k], kl) || !g_ascii_isdigit(key[kl]))
            continue;
        char *end;
        long num = strtol(key + kl, &end, 10);
        if (*end)
            return;
        pls_item *it = pls_get(st, num);
        if (k == 0) {
            g_free(it->file);
            it->file = g_strdup(value);
        } else if (k == 1) {
            g_free(it->title);
            it->title = g_strdup(value);
        } else {
            long long secs = strtoll(value, &end, 10);
            it->length = end != value && secs >= 0 ? secs * 1000 : -1;
        }
        return;
    }
}

static gint cmp_pls(gconstpointer a, gconstpointer b)
{
    long x = ((const pls_item *)a)->num, y = ((const pls_item *)b)->num;
    return x < y ? -1 : x > y;
}

kp_playlist *kp_playlist_parse_pls(const char *text, const char *dir,
                                   const char *default_name)
{
    pls_state st = {.items = g_array_new(FALSE, FALSE, sizeof(pls_item))};
    each_line(text, pls_line, &st);
    g_array_sort(st.items, cmp_pls);
    kp_playlist *pl = kp_playlist_new(st.name ? st.name : default_name);
    for (guint i = 0; i < st.items->len; i++) {
        pls_item *it = &g_array_index(st.items, pls_item, i);
        if (it->file && *it->file) {
            char *loc = resolve(it->file, dir);
            kp_tags t = {it->title, NULL, NULL, it->length};
            kp_playlist_add(pl, loc, &t);
            g_free(loc);
        }
        g_free(it->file);
        g_free(it->title);
    }
    g_array_free(st.items, TRUE);
    g_free(st.name);
    pl->dirty = false;
    return pl;
}

static bool looks_like_pls(const char *text)
{
    if (!strncmp(text, "\xEF\xBB\xBF", 3))
        text += 3;
    text += strspn(text, " \t\r\n");
    return !g_ascii_strncasecmp(text, "[playlist]", 10);
}

kp_playlist *kp_playlist_load(const char *path, const char *default_name)
{
    char *text;
    gsize len;
    if (!g_file_get_contents(path, &text, &len, NULL)) {
        errno = ENOENT;
        return NULL;
    }
    if (!g_utf8_validate(text, (gssize)len, NULL)) {
        /* Old .m3u files are in the system's 8-bit encoding. */
        char *conv = g_convert(text, (gssize)len, "UTF-8", "WINDOWS-1252", NULL, NULL, NULL);
        if (conv) {
            g_free(text);
            text = conv;
        }
    }
    char *dir = g_path_get_dirname(path);
    kp_playlist *pl = looks_like_pls(text) ? kp_playlist_parse_pls(text, dir, default_name)
                                           : kp_playlist_parse_m3u(text, dir, default_name);
    g_free(dir);
    g_free(text);
    return pl;
}

/* One line in the file. */
static char *one_line(const char *s)
{
    return g_strdelimit(g_strdup(s ? s : ""), "\r\n", ' ');
}

/* What other players show: "Artist - Title". */
static char *shown_title(const kp_entry *e)
{
    if (!e->title)
        return g_strdup("");
    char *t = e->artist ? g_strdup_printf("%s - %s", e->artist, e->title) : g_strdup(e->title);
    g_strdelimit(t, "\r\n", ' ');
    return t;
}

char *kp_playlist_dump(const kp_playlist *pl)
{
    GString *out = g_string_new("#EXTM3U\n");
    /* The name is one line: a newline in it would end the header early. */
    char *name = one_line(pl->name);
    g_string_append_printf(out, "#PLAYLIST:%s\n", name);
    g_free(name);
    if (pl->type == KP_PLAYLIST_AUDIO)
        g_string_append(out, "#KIPLAY-TYPE:audio\n");
    for (size_t i = 0; i < pl->n; i++) {
        const kp_entry *e = &pl->items[i];
        if (e->title || e->length_ms >= 0) {
            char *t = shown_title(e);
            g_string_append_printf(out, "#EXTINF:%lld,%s\n",
                                   e->length_ms >= 0 ? (long long)(e->length_ms / 1000) : -1LL, t);
            g_free(t);
        }
        if (e->title && e->artist) {
            char *a = one_line(e->artist);
            g_string_append_printf(out, "#EXTART:%s\n", a);
            g_free(a);
        }
        if (e->title && e->album) {
            char *a = one_line(e->album);
            g_string_append_printf(out, "#EXTALB:%s\n", a);
            g_free(a);
        }
        g_string_append_printf(out, "%s\n", e->location);
    }
    return g_string_free(out, FALSE);
}

char *kp_playlist_dump_pls(const kp_playlist *pl)
{
    GString *out = g_string_new("[playlist]\n");
    char *name = one_line(pl->name);
    g_string_append_printf(out, "X-GNOME-Title=%s\n", name);
    g_free(name);
    for (size_t i = 0; i < pl->n; i++) {
        const kp_entry *e = &pl->items[i];
        g_string_append_printf(out, "File%zu=%s\n", i + 1, e->location);
        if (e->title) {
            char *t = shown_title(e);
            g_string_append_printf(out, "Title%zu=%s\n", i + 1, t);
            g_free(t);
        }
        g_string_append_printf(out, "Length%zu=%lld\n", i + 1,
                               e->length_ms >= 0 ? (long long)(e->length_ms / 1000) : -1LL);
    }
    g_string_append_printf(out, "NumberOfEntries=%zu\nVersion=2\n", pl->n);
    return g_string_free(out, FALSE);
}

int kp_playlist_save(const kp_playlist *pl, const char *path)
{
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    char *text = g_str_has_suffix(path, ".pls") ? kp_playlist_dump_pls(pl) : kp_playlist_dump(pl);
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
