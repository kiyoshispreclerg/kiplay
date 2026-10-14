/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sidecar.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "json3.h"
#include "media.h"
#include "srv3.h"
#include "util.h"

/* Plain subtitle formats the player reads. In order of preference when a
 * language has more than one. */
static const char *const plain_exts[] = {"ass", "ssa", "srt", "vtt", "smi", "sub", "sup", NULL};

void kp_sidecar_free(kp_sidecar *sc)
{
    if (!sc)
        return;
    g_free(sc->path);
    g_free(sc->lang);
    g_free(sc->format);
    g_free(sc);
}

static int index_in(const char *const list[], const char *s)
{
    for (int i = 0; list[i]; i++)
        if (!g_ascii_strcasecmp(list[i], s))
            return i;
    return -1;
}

/* Language tags: "ko", "es-419", "zh-Hans", "en_US". */
static bool looks_like_lang(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 16)
        return false;
    for (; *s; s++)
        if (!g_ascii_isalnum(*s) && *s != '-' && *s != '_')
            return false;
    return true;
}

/* How good a candidate is within its language: higher wins. */
static int rank(const kp_sidecar *sc)
{
    if (sc->kind == KP_SIDECAR_YT_SUB)
        return 100 - (!strcmp(sc->format, "json3") ? 1 : 0);
    int i = index_in(plain_exts, sc->format);
    return 50 - (i < 0 ? 40 : i);
}

static gint by_lang(gconstpointer a, gconstpointer b)
{
    const kp_sidecar *x = *(kp_sidecar *const *)a, *y = *(kp_sidecar *const *)b;
    if ((x->kind == KP_SIDECAR_CHAT) != (y->kind == KP_SIDECAR_CHAT))
        return x->kind == KP_SIDECAR_CHAT ? 1 : -1;
    return g_utf8_collate(x->lang, y->lang);
}

GPtrArray *kp_sidecars_find(const char *video_path)
{
    GPtrArray *all = g_ptr_array_new_with_free_func((GDestroyNotify)kp_sidecar_free);
    char *dir = g_path_get_dirname(video_path);
    char *file = g_path_get_basename(video_path);
    char *dot = strrchr(file, '.');
    if (dot && dot != file)
        *dot = '\0';
    size_t base_len = strlen(file);

    GDir *d = g_dir_open(dir, 0, NULL);
    GHashTable *best = g_hash_table_new(g_str_hash, g_str_equal); /* lang -> kp_sidecar* */
    kp_sidecar *chat = NULL;
    const char *name;
    while (d && (name = g_dir_read_name(d))) {
        if (strncmp(name, file, base_len) != 0 || name[base_len] != '.')
            continue;
        const char *rest = name + base_len + 1; /* "<lang>.<ext>" or "<ext>" */
        const char *last = strrchr(rest, '.');
        char *ext = g_ascii_strdown(last ? last + 1 : rest, -1);
        char *lang = last ? g_strndup(rest, (gsize)(last - rest)) : g_strdup("");
        kp_sidecar_kind kind;
        if (!strcmp(ext, "srv3") || !strcmp(ext, "json3"))
            kind = KP_SIDECAR_YT_SUB;
        else if (index_in(plain_exts, ext) >= 0)
            kind = KP_SIDECAR_SUB;
        else if (!strcmp(ext, "json") && !strcmp(lang, "live_chat"))
            kind = KP_SIDECAR_CHAT;
        else
            kind = (kp_sidecar_kind)-1;
        /* Not "Title.mp4" next to "Title.mkv", and not "Title.en.mkv". */
        if ((int)kind < 0 || (kind != KP_SIDECAR_CHAT && !looks_like_lang(lang) && *lang) ||
            (kind != KP_SIDECAR_CHAT && kp_media_kind_of(name) != KP_MEDIA_NONE)) {
            g_free(ext);
            g_free(lang);
            continue;
        }
        kp_sidecar *sc = g_new0(kp_sidecar, 1);
        sc->kind = kind;
        sc->path = g_build_filename(dir, name, NULL);
        sc->lang = kind == KP_SIDECAR_CHAT ? g_strdup("") : lang;
        sc->format = ext;
        if (kind == KP_SIDECAR_CHAT) {
            g_free(lang);
            if (chat)
                kp_sidecar_free(chat);
            chat = sc;
            continue;
        }
        kp_sidecar *cur = g_hash_table_lookup(best, sc->lang);
        if (cur && rank(cur) >= rank(sc)) {
            kp_sidecar_free(sc);
            continue;
        }
        g_hash_table_replace(best, sc->lang, sc);
        if (cur)
            kp_sidecar_free(cur);
    }
    if (d)
        g_dir_close(d);

    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, best);
    while (g_hash_table_iter_next(&it, NULL, &v))
        g_ptr_array_add(all, v);
    if (chat)
        g_ptr_array_add(all, chat);
    g_ptr_array_sort(all, by_lang);

    g_hash_table_destroy(best);
    g_free(dir);
    g_free(file);
    return all;
}

char *kp_sidecar_video_id(const char *path)
{
    char *file = g_path_get_basename(path);
    char *dot = strrchr(file, '.');
    if (dot)
        *dot = '\0';
    size_t n = strlen(file);
    char *id = NULL;
    if (n > 13 && file[n - 1] == ']' && file[n - 13] == '[') {
        bool ok = true;
        for (size_t i = n - 12; i < n - 1; i++)
            ok = ok && (g_ascii_isalnum(file[i]) || file[i] == '_' || file[i] == '-');
        if (ok)
            id = g_strndup(file + n - 12, 11);
    }
    g_free(file);
    return id;
}

void kp_sidecar_ass_opts(const kp_prefs *p, kp_ytass_opts *o)
{
    kp_ytass_opts_default(o);
    const char *edge = kp_prefs_get_str(p, "subtitles", "youtube_edge", "youtube");
    if (!strcmp(edge, "strong"))
        o->edge_scale = 3.0;
}

static char *default_cache_dir(void)
{
    return g_build_filename(g_get_user_cache_dir(), "kiplay", "subs", NULL);
}

bool kp_sidecar_is_converted(const char *path)
{
    char *dir = default_cache_dir();
    bool in_it = g_str_has_prefix(path, dir) && path[strlen(dir)] == '/';
    g_free(dir);
    return in_it;
}

/* Same source and options give the same name; anything else a new one. */
static char *cache_name(const kp_sidecar *sc, const kp_ytass_opts *o, const struct stat *st)
{
    char num[G_ASCII_DTOSTR_BUF_SIZE];
    GString *key = g_string_new(sc->path);
    g_string_append_printf(key, "|%lld|%lld|%d|%d|", (long long)st->st_size,
                           (long long)st->st_mtime, o->width, o->height);
    g_string_append(key, g_ascii_formatd(num, sizeof num, "%.4f", o->em_ratio));
    g_string_append_c(key, '|');
    g_string_append(key, g_ascii_formatd(num, sizeof num, "%.4f", o->edge_scale));
    for (int i = 0; i < KP_YT_FONT_STYLES; i++)
        g_string_append_printf(key, "|%s", o->fonts[i] ? o->fonts[i] : "");
    char *sum = g_compute_checksum_for_string(G_CHECKSUM_SHA1, key->str, -1);
    sum[16] = '\0';
    /* mpv takes the language from the name before the extension. */
    char *name = g_strdup_printf("%s%s%s.ass", sum, *sc->lang ? "." : "", sc->lang);
    g_free(sum);
    g_string_free(key, TRUE);
    return name;
}

kp_ytsub *kp_ytsub_load(const char *path, char **err)
{
    return g_str_has_suffix(path, ".json3") ? kp_json3_load(path, err) : kp_srv3_load(path, err);
}

char *kp_sidecar_play_path(const kp_sidecar *sc, const kp_ytass_opts *o, const char *cache_dir,
                           char **err)
{
    if (sc->kind != KP_SIDECAR_YT_SUB)
        return g_strdup(sc->path);

    struct stat st;
    if (g_stat(sc->path, &st) != 0) {
        if (err)
            *err = g_strdup(g_strerror(errno));
        return NULL;
    }
    char *dir = cache_dir ? g_strdup(cache_dir) : default_cache_dir();
    char *name = cache_name(sc, o, &st);
    char *out = g_build_filename(dir, name, NULL);
    g_free(dir);
    g_free(name);
    if (kp_file_exists(out))
        return out;

    kp_ytsub *sub = kp_ytsub_load(sc->path, err);
    if (!sub) {
        g_free(out);
        return NULL;
    }
    char *ass = kp_ytsub_to_ass(sub, o);
    kp_ytsub_free(sub);
    kp_make_parent_dirs(out);
    char *tmp = g_strdup_printf("%s.tmp%d", out, (int)getpid());
    GError *gerr = NULL;
    bool ok = g_file_set_contents(tmp, ass, -1, &gerr) && g_rename(tmp, out) == 0;
    if (!ok) {
        if (err)
            *err = g_strdup(gerr ? gerr->message : g_strerror(errno));
        g_unlink(tmp);
        g_free(out);
        out = NULL;
    }
    g_clear_error(&gerr);
    g_free(tmp);
    g_free(ass);
    return out;
}
