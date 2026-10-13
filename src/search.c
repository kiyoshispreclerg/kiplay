/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "search.h"

#include <glib.h>
#include <string.h>

/* Case-folded and composed, so "Á" typed matches "á" stored either way. */
static char *fold(const char *s)
{
    char *norm = g_utf8_normalize(s, -1, G_NORMALIZE_ALL_COMPOSE);
    char *folded = g_utf8_casefold(norm ? norm : s, -1);
    g_free(norm);
    return folded;
}

char **kp_search_words(const char *query)
{
    char *folded = fold(g_utf8_validate(query, -1, NULL) ? query : "");
    char **parts = g_strsplit_set(folded, " \t", -1);
    g_free(folded);
    /* Drop the empty ones the spaces leave. */
    size_t k = 0;
    for (size_t i = 0; parts[i]; i++) {
        if (*parts[i])
            parts[k++] = parts[i];
        else
            g_free(parts[i]);
    }
    parts[k] = NULL;
    return parts;
}

char *kp_search_haystack(const kp_entry *e)
{
    char *path = g_str_has_prefix(e->location, "file://")
                     ? g_uri_unescape_string(e->location + strlen("file://"), NULL)
                     : NULL;
    const char *where = path ? path : e->location;
    char *shown = g_utf8_validate(where, -1, NULL) ? g_strdup(where)
                                                   : g_filename_display_name(where);
    char *all = g_strjoin("\n", e->title ? e->title : "", e->artist ? e->artist : "",
                          e->album ? e->album : "", shown, NULL);
    char *folded = fold(all);
    g_free(all);
    g_free(shown);
    g_free(path);
    return folded;
}

bool kp_search_match(const char *haystack, char *const *words)
{
    for (size_t i = 0; words[i]; i++)
        if (!strstr(haystack, words[i]))
            return false;
    return true;
}
