/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "splitspec.h"

#include <glib/gstdio.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "json.h"

#define IS_DIGIT(c) ((c) >= '0' && (c) <= '9')

/* A first time this close to 0 is the start: no piece of a second
 * before it. */
#define LEAD_MIN_S 1.0
/* An info.json with the formats of a long video runs to megabytes; more
 * than this isn't one. */
#define MAX_SIDECAR (64 * 1024 * 1024)

/* Digits at s, at most max of them: how many (0 if none). */
static int digits(const char *s, int max, int *value)
{
    int n = 0, v = 0;
    while (n < max && IS_DIGIT(s[n]))
        v = v * 10 + (s[n++] - '0');
    if (IS_DIGIT(s[n]))
        return 0; /* too many */
    *value = v;
    return n;
}

/* A time starting exactly at s ("m:ss", "h:mm:ss", ".frac" after either):
 * its length in bytes, 0 if none. */
static size_t time_at(const char *s, double *seconds)
{
    int part[3], n = 0;
    size_t len = 0;
    int d = digits(s, 3, &part[0]);
    if (!d)
        return 0;
    len = (size_t)d;
    n = 1;
    while (n < 3 && s[len] == ':') {
        int v;
        int k = digits(s + len + 1, 2, &v);
        if (k != 2 || v > 59)
            break;
        part[n++] = v;
        len += 3;
    }
    if (n < 2)
        return 0;
    double frac = 0;
    if (s[len] == '.' && IS_DIGIT(s[len + 1])) {
        double scale = 0.1;
        len++;
        while (IS_DIGIT(s[len])) {
            frac += (s[len++] - '0') * scale;
            scale /= 10;
        }
    }
    if (s[len] == ':' || IS_DIGIT(s[len]))
        return 0; /* "1:2:3:4", "12:345" */
    double t = n == 3 ? part[0] * 3600.0 + part[1] * 60.0 + part[2] : part[0] * 60.0 + part[1];
    *seconds = t + frac;
    return len;
}

/* The first time in line: where it starts and its length (0 if none). */
static size_t find_time(const char *line, size_t *at, double *seconds)
{
    for (size_t i = 0; line[i]; i++) {
        if (!IS_DIGIT(line[i]))
            continue;
        /* Glued to a word or a number, it is part of it ("v1:30"). */
        if (i > 0 && (g_ascii_isalnum(line[i - 1]) || line[i - 1] == ':' || line[i - 1] == '.'))
            continue;
        size_t len = time_at(line + i, seconds);
        if (len) {
            *at = i;
            return len;
        }
    }
    return 0;
}

static bool sep_char(const char *s, size_t *len)
{
    static const char *const seps[] = {" ", "\t", "-", "|", ":", "~", ",", ";", ">",
                                       "\xE2\x80\x93" /* – */, "\xE2\x80\x94" /* — */,
                                       "\xE2\x80\xA2" /* • */, "\xC2\xA0" /* nbsp */};
    for (size_t i = 0; i < G_N_ELEMENTS(seps); i++) {
        size_t n = strlen(seps[i]);
        if (!strncmp(s, seps[i], n)) {
            *len = n;
            return true;
        }
    }
    return false;
}

/* Separators off both ends, in place (lead also takes dots: "3:45. Title"). */
static void trim_seps(GString *s)
{
    size_t n, start = 0;
    while (start < s->len) {
        if (s->str[start] == '.')
            n = 1;
        else if (!sep_char(s->str + start, &n))
            break;
        start += n;
    }
    g_string_erase(s, 0, (gssize)start);
    for (;;) {
        bool cut = false;
        for (size_t k = 1; k <= 3 && k <= s->len; k++) {
            if (sep_char(s->str + s->len - k, &n) && n == k) {
                g_string_truncate(s, s->len - k);
                cut = true;
                break;
            }
        }
        if (!cut)
            break;
    }
}

/* "01. ", "1) ", "#3 ", "02 - " at the start: off, when something is left. */
static void drop_track_number(GString *s)
{
    const char *p = s->str;
    bool hash = *p == '#';
    int v;
    int d = digits(p + hash, 3, &v);
    if (!d)
        return;
    p += hash + d;
    size_t n;
    if (hash && *p == ' ')
        ; /* "#3 Title" */
    else if ((*p == '.' || *p == ')') && (p[1] == ' ' || p[1] == '\t'))
        p++; /* "01. Title", "1) Title" */
    else if (*p == ' ' && sep_char(p + 1, &n) && p[1] != ' ' && p[1 + n] == ' ')
        p += 1 + n; /* "02 - Title" */
    else
        return;
    GString *rest = g_string_new(p);
    trim_seps(rest);
    if (rest->len)
        g_string_assign(s, rest->str);
    g_string_free(rest, TRUE);
}

/* The text of a line around its time: brackets of the time, a second time
 * (a range "0:00 - 3:45"), separators and a track number off. */
static char *line_text(const char *line, size_t at, size_t len)
{
    size_t b = at, e = at + len;
    if (b > 0 && (line[b - 1] == '[' || line[b - 1] == '(') &&
        (line[e] == ']' || line[e] == ')')) {
        b--;
        e++;
    }
    GString *after = g_string_new(line + e);
    trim_seps(after);
    double ignored;
    size_t len2 = IS_DIGIT(after->str[0]) ? time_at(after->str, &ignored) : 0;
    if (len2) {
        g_string_erase(after, 0, (gssize)len2);
        trim_seps(after);
    }
    GString *s = g_string_new_len(line, (gssize)b);
    trim_seps(s);
    if (s->len && after->len)
        g_string_append_c(s, ' ');
    g_string_append(s, after->str);
    g_string_free(after, TRUE);
    /* "Title ()" once the time is gone. */
    if (g_str_has_suffix(s->str, "()") || g_str_has_suffix(s->str, "[]"))
        g_string_truncate(s, s->len - 2);
    trim_seps(s);
    drop_track_number(s);
    char *t = g_strstrip(g_string_free(s, FALSE));
    return t;
}

static void clear_piece(void *p)
{
    g_free(((kp_split_piece *)p)->text);
}

static GArray *new_pieces(void)
{
    GArray *a = g_array_new(FALSE, TRUE, sizeof(kp_split_piece));
    g_array_set_clear_func(a, clear_piece);
    return a;
}

typedef struct {
    double t;
    char *text;
    int line;
} mark;

/* Lengths that add up to the file's: a track list. Each "m:ss" can be off
 * by up to a second, and some lists leave out a short gap or two. */
static bool fits_as_durations(const GArray *marks, double duration)
{
    if (duration <= 0 || !marks->len)
        return false;
    double sum = 0;
    for (guint i = 0; i < marks->len; i++) {
        double t = g_array_index(marks, mark, i).t;
        if (t <= 0)
            return false;
        sum += t;
    }
    double tol = 2.0 + marks->len + duration * 0.01;
    return fabs(sum - duration) <= tol;
}

GArray *kp_split_parse(const char *text, double duration, kp_split_result *res)
{
    *res = (kp_split_result){KP_SPLIT_OK, 0, false};
    GArray *marks = g_array_new(FALSE, FALSE, sizeof(mark));
    char **lines = g_strsplit(text ? text : "", "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *l = lines[i];
        size_t n = strlen(l);
        if (n && l[n - 1] == '\r')
            l[n - 1] = '\0';
        size_t at;
        double t;
        size_t len = find_time(l, &at, &t);
        if (!len)
            continue;
        mark m = {t, line_text(l, at, len), i + 1};
        g_array_append_val(marks, m);
    }
    g_strfreev(lines);

    GArray *pieces = new_pieces();
    if (!marks->len)
        res->status = KP_SPLIT_EMPTY;
    bool ordered = true;
    for (guint i = 1; i < marks->len; i++)
        ordered &= g_array_index(marks, mark, i).t > g_array_index(marks, mark, i - 1).t;
    double first = marks->len ? g_array_index(marks, mark, 0).t : 0;
    bool durations = marks->len && fits_as_durations(marks, duration) &&
                     (!ordered || first >= LEAD_MIN_S);
    res->as_durations = durations;

    double at = 0;
    for (guint i = 0; res->status == KP_SPLIT_OK && i < marks->len; i++) {
        mark *m = &g_array_index(marks, mark, i);
        double start = durations ? at : m->t;
        if (!durations && i == 0 && start < LEAD_MIN_S)
            start = 0;
        if (!durations && i == 0 && start > 0) {
            kp_split_piece lead = {0, start, g_strdup(""), 0};
            g_array_append_val(pieces, lead);
        }
        if (!durations && i > 0 && start <= g_array_index(pieces, kp_split_piece, pieces->len - 1).start) {
            res->status = KP_SPLIT_UNORDERED;
            res->line = m->line;
            break;
        }
        if (duration > 0 && start >= duration) {
            res->status = KP_SPLIT_PAST_END;
            res->line = m->line;
            break;
        }
        if (pieces->len)
            g_array_index(pieces, kp_split_piece, pieces->len - 1).end = start;
        kp_split_piece p = {start, duration > 0 ? duration : -1, m->text, m->line};
        m->text = NULL;
        g_array_append_val(pieces, p);
        at += m->t;
    }
    for (guint i = 0; i < marks->len; i++)
        g_free(g_array_index(marks, mark, i).text);
    g_array_unref(marks);
    if (res->status != KP_SPLIT_OK)
        g_array_set_size(pieces, 0);
    return pieces;
}

bool kp_split_artist_title(const char *text, char **artist, char **title)
{
    static const char *const dashes[] = {" - ", " \xE2\x80\x93 ", " \xE2\x80\x94 "};
    const char *best = NULL;
    size_t blen = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(dashes); i++) {
        const char *d = strstr(text, dashes[i]);
        if (d && (!best || d < best)) {
            best = d;
            blen = strlen(dashes[i]);
        }
    }
    if (!best)
        return false;
    char *a = g_strstrip(g_strndup(text, (gsize)(best - text)));
    char *t = g_strstrip(g_strdup(best + blen));
    if (!a[0] || !t[0]) {
        g_free(a);
        g_free(t);
        return false;
    }
    *artist = a;
    *title = t;
    return true;
}

char *kp_split_file_name(int n, int total, const char *title, const char *ext)
{
    int width = 2;
    for (int t = total; t >= 100; t /= 10)
        width++;
    GString *s = g_string_new(NULL);
    g_string_printf(s, "%0*d", width, n);
    if (title && title[0]) {
        g_string_append(s, " - ");
        size_t from = s->len;
        const char *end;
        /* Invalid UTF-8 stays out (the name is shown, and kept). */
        g_utf8_validate(title, -1, &end);
        for (const char *p = title; p < end; p++) {
            unsigned char c = (unsigned char)*p;
            if (c < 0x20 || c == 0x7F)
                continue;
            g_string_append_c(s, c == '/' ? '-' : (char)c);
        }
        while (s->len > from && s->str[from] == '.')
            g_string_erase(s, (gssize)from, 1);
        /* Room for the extension and ".part" in a 255-byte name. */
        if (s->len > 200) {
            const char *cut = g_utf8_find_prev_char(s->str, s->str + 200 + 1);
            g_string_truncate(s, (gsize)(cut - s->str));
        }
        while (s->len > from && s->str[s->len - 1] == ' ')
            g_string_truncate(s, s->len - 1);
        if (s->len == from)
            g_string_truncate(s, from - 3);
    }
    if (ext && ext[0])
        g_string_append_printf(s, ".%s", ext);
    return g_string_free(s, FALSE);
}

char *kp_split_format_time(double seconds)
{
    if (seconds < 0)
        seconds = 0;
    long ms = lround(seconds * 1000);
    long whole = ms / 1000, frac = ms % 1000;
    long h = whole / 3600, m = whole / 60 % 60, s = whole % 60;
    char base[32], dec[8] = "";
    if (h)
        snprintf(base, sizeof base, "%ld:%02ld:%02ld", h, m, s);
    else
        snprintf(base, sizeof base, "%ld:%02ld", m, s);
    if (frac) {
        snprintf(dec, sizeof dec, ".%03ld", frac);
        for (size_t n = strlen(dec); n > 2 && dec[n - 1] == '0'; n--)
            dec[n - 1] = '\0';
    }
    return g_strconcat(base, dec, NULL);
}

static void append_mark(GString *s, double start, const char *title)
{
    char *t = kp_split_format_time(start);
    g_string_append_printf(s, "%s %s\n", t, title ? title : "");
    g_free(t);
}

static bool has_time(const char *text)
{
    GArray *marks;
    kp_split_result res;
    marks = kp_split_parse(text, 0, &res);
    g_array_unref(marks);
    return res.status != KP_SPLIT_EMPTY;
}

static char *read_small(const char *path)
{
    GStatBuf st;
    char *text = NULL;
    if (g_stat(path, &st) || st.st_size > MAX_SIDECAR)
        return NULL;
    if (!g_file_get_contents(path, &text, NULL, NULL))
        return NULL;
    if (!g_utf8_validate(text, -1, NULL)) {
        g_free(text);
        return NULL;
    }
    return text;
}

/* yt-dlp's info.json: its chapters, else the description when it has
 * times. */
static char *from_info_json(const char *path)
{
    char *raw = read_small(path);
    /* cJSON reads numbers with strtod: decimals need the C locale. */
    locale_t cloc = newlocale(LC_ALL_MASK, "C", (locale_t)0);
    locale_t old = cloc ? uselocale(cloc) : (locale_t)0;
    cJSON *root = raw ? cJSON_Parse(raw) : NULL;
    if (cloc) {
        uselocale(old);
        freelocale(cloc);
    }
    g_free(raw);
    if (!root)
        return NULL;
    char *out = NULL;
    const cJSON *ch = kp_json_path(root, "chapters");
    if (cJSON_IsArray(ch) && cJSON_GetArraySize(ch) > 0) {
        GString *s = g_string_new(NULL);
        const cJSON *c;
        cJSON_ArrayForEach(c, ch) append_mark(s, kp_json_num(c, "start_time", 0),
                                              kp_json_str(c, "title"));
        out = g_string_free(s, FALSE);
    }
    const char *desc = kp_json_str(root, "description");
    if (!out && desc && has_time(desc))
        out = g_strdup(desc);
    cJSON_Delete(root);
    return out;
}

char *kp_split_text_for(const char *path, const double *starts, char *const *titles, int n)
{
    if (n > 0) {
        GString *s = g_string_new(NULL);
        for (int i = 0; i < n; i++)
            append_mark(s, starts[i], titles[i]);
        return g_string_free(s, FALSE);
    }
    const char *slash = strrchr(path, '/'), *dot = strrchr(path, '.');
    char *base = dot && (!slash || dot > slash) ? g_strndup(path, (gsize)(dot - path)) : g_strdup(path);
    char *info = g_strconcat(base, ".info.json", NULL);
    char *out = from_info_json(info);
    g_free(info);
    if (!out) {
        char *dpath = g_strconcat(base, ".description", NULL);
        out = read_small(dpath);
        g_free(dpath);
        if (out && !has_time(out)) {
            g_free(out);
            out = NULL;
        }
    }
    g_free(base);
    return out;
}
