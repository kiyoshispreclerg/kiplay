/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gme.h"

/* The fade of songs that don't say how long it is. */
#define FADE_MS 8000

struct ka_game {
    Music_Emu *emu;
    int track;
    int rate;
    int length_ms; /* where the fade starts */
    int fade_ms;
    char *song, *game, *author;
};

bool ka_game_is(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(slash ? slash : path, '.');
    return dot && dot[1] && gme_identify_extension(dot + 1) != NULL;
}

static char *dup_field(const char *s)
{
    return s && *s ? strdup(s) : NULL;
}

/* ---- the rip's .m3u ------------------------------------------------------
 *
 * "game.nsf::NSF,5,Title,0:01:30,,0:00:08" says song 5 of game.nsf is
 * "Title", fading out at 1:30 for 8 s (length and fade may be empty).
 * GME can load these, but then song N means the playlist's N-th line;
 * Kiplay keeps Audacious's meaning (song N of the file, which Audacious's
 * playlists hold) and takes only the names and lengths from here.
 *
 * Numbers are from 1, "$0F" from 0, as in GME; but some rips (Pokemon
 * Yellow's GBS) count from 0 in decimal too: a list with a 0 in it does. */

typedef struct {
    char *name;
    int length_ms, fade_ms; /* -1 if not given */
} m3u_entry;

/* "h:mm:ss.mmm" (any of the parts); -1 if empty. */
static int parse_time(const char *s)
{
    if (*s < '0' || *s > '9')
        return -1;
    long t = 0;
    for (;;) {
        char *end;
        long n = strtol(s, &end, 10);
        t = t * 60 + n;
        s = end;
        if (*s != ':')
            break;
        s++;
    }
    t *= 1000;
    if (*s == '.') {
        int scale = 100;
        for (s++; *s >= '0' && *s <= '9' && scale > 0; s++, scale /= 10)
            t += (*s - '0') * scale;
    }
    return t > 0x7fffffff ? -1 : (int)t;
}

/* Splits at commas (not "\,"), unescaping; up to max fields. */
static int split_fields(char *s, char **field, int max)
{
    int n = 0;
    field[n++] = s;
    char *out = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            *out++ = *++s;
        } else if (*s == ',' && n < max) {
            *out++ = '\0';
            field[n++] = out;
        } else {
            *out++ = *s;
        }
    }
    *out = '\0';
    return n;
}

/* The next line about `file`: its song number (from 0 if hex, else as
 * written: *hex says which) and fields; false at the end. */
static bool m3u_next(FILE *f, const char *file, char *line, int size, long *number, bool *hex,
                     char **field, int *nfield)
{
    while (fgets(line, size, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *sep = strstr(line, "::");
        if (line[0] == '#' || !sep)
            continue;
        *sep = '\0';
        *nfield = split_fields(sep + 2, field, 6); /* type, song, name, time, loop, fade */
        if (*nfield < 2 || strcmp(line, file) != 0)
            continue;
        *hex = field[1][0] == '$';
        char *end;
        *number = strtol(field[1] + *hex, &end, *hex ? 16 : 10);
        if (end != field[1] + *hex)
            return true;
    }
    return false;
}

/* Song `track` (from 0) of `file` (a base name) in that list. */
static bool m3u_find(const char *m3u, const char *file, int track, m3u_entry *found)
{
    FILE *f = fopen(m3u, "r");
    if (!f)
        return false;
    char line[2048], *field[6];
    int nfield;
    long number;
    bool hex, zero_based = false, have = false;
    while (m3u_next(f, file, line, sizeof line, &number, &hex, field, &nfield))
        if (!hex && number == 0)
            zero_based = true;
    rewind(f);
    while (!have && m3u_next(f, file, line, sizeof line, &number, &hex, field, &nfield)) {
        if ((hex || zero_based ? number : number - 1) != track)
            continue;
        found->name = nfield > 2 && field[2][0] ? strdup(field[2]) : NULL;
        found->length_ms = nfield > 3 ? parse_time(field[3]) : -1;
        found->fade_ms = nfield > 5 ? parse_time(field[5]) : -1;
        have = true;
    }
    fclose(f);
    return have;
}

/* "dir/game.nsf" -> "dir/game.m3u", if there is one. */
static char *m3u_beside(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(slash ? slash : path, '.');
    size_t base = dot ? (size_t)(dot - path) : strlen(path);
    char *m3u = malloc(base + 5);
    if (!m3u)
        return NULL;
    memcpy(m3u, path, base);
    memcpy(m3u + base, ".m3u", 5);
    struct stat st;
    if (stat(m3u, &st) != 0 || !S_ISREG(st.st_mode)) {
        free(m3u);
        return NULL;
    }
    return m3u;
}

ka_game *ka_game_open(const char *path, int subtune, int rate, char *err, int errsize)
{
    ka_game *g = calloc(1, sizeof *g);
    if (!g) {
        snprintf(err, (size_t)errsize, "out of memory");
        return NULL;
    }
    g->rate = rate;
    gme_err_t e = gme_open_file(path, &g->emu, rate > 0 ? rate : gme_info_only);
    if (e)
        goto fail;
    g->track = subtune > 0 ? subtune - 1 : 0;
    if (g->track >= gme_track_count(g->emu)) {
        e = "no such song in the file";
        goto fail;
    }
    gme_info_t *info;
    if ((e = gme_track_info(g->emu, &info, g->track)))
        goto fail;
    m3u_entry m = {.length_ms = -1, .fade_ms = -1};
    char *m3u = m3u_beside(path);
    const char *slash = strrchr(path, '/');
    if (m3u && m3u_find(m3u, slash ? slash + 1 : path, g->track, &m)) {
        if (m.length_ms > 0) {
            info->length = m.length_ms;
            info->fade_length = m.fade_ms;
        }
    }
    free(m3u);
    if (info->length > 0) {
        g->length_ms = info->length;
        /* No loop (a VGM that just ends): no fade either. */
        g->fade_ms = info->fade_length >= 0 ? info->fade_length
                     : info->loop_length == 0 ? 0
                                              : FADE_MS;
    } else if (info->loop_length > 0) {
        /* Twice through the loop, as GME's own players do. */
        g->length_ms = (info->intro_length > 0 ? info->intro_length : 0) + 2 * info->loop_length;
        g->fade_ms = FADE_MS;
    } else {
        g->length_ms = KA_GAME_LENGTH_S * 1000 - FADE_MS;
        g->fade_ms = FADE_MS;
    }
    g->song = m.name ? m.name : dup_field(info->song);
    g->game = dup_field(info->game);
    g->author = dup_field(info->author);
    gme_free_info(info);

    if (rate > 0) {
        /* The fade is ours: an SPC would set its own on start. */
        gme_set_autoload_playback_limit(g->emu, 0);
        if ((e = gme_start_track(g->emu, g->track)))
            goto fail;
        gme_set_fade_msecs(g->emu, g->length_ms, g->fade_ms);
    }
    return g;

fail:
    snprintf(err, (size_t)errsize, "%s", e);
    ka_game_close(g);
    return NULL;
}

void ka_game_close(ka_game *g)
{
    if (!g)
        return;
    gme_delete(g->emu);
    free(g->song);
    free(g->game);
    free(g->author);
    free(g);
}

long ka_game_read(ka_game *g, int16_t *out, long max)
{
    if (gme_track_ended(g->emu))
        return 0;
    /* gme_play counts samples, not frames. */
    if (gme_play(g->emu, (int)(max * 2), out))
        return 0;
    return max;
}

bool ka_game_seek(ka_game *g, double seconds)
{
    return gme_seek(g->emu, (int)(seconds * 1000)) == NULL;
}

double ka_game_duration(const ka_game *g)
{
    return (g->length_ms + g->fade_ms) / 1000.0;
}

const char *ka_game_song(const ka_game *g)
{
    return g->song;
}

const char *ka_game_name(const ka_game *g)
{
    return g->game;
}

const char *ka_game_author(const ka_game *g)
{
    return g->author;
}
