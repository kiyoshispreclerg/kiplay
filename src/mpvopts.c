/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mpvopts.h"

#include <glib.h>
#include <string.h>

/* A language list as mpv wants it: "pt, en" -> "pt,en". */
static char *lang_list(const char *s)
{
    GString *out = g_string_new(NULL);
    for (; *s; s++)
        if (!g_ascii_isspace(*s))
            g_string_append_c(out, *s);
    return g_string_free(out, FALSE);
}

static void add(GPtrArray *a, const char *option, const char *value)
{
    if (value && *value)
        g_ptr_array_add(a, g_strdup_printf("--%s=%s", option, value));
}

char **kp_mpv_pref_args(const kp_prefs *p)
{
    GPtrArray *a = g_ptr_array_new();
    const char *hwdec = kp_prefs_get_str(p, "video", "hwdec", NULL);
    add(a, "hwdec", hwdec);
    add(a, "vo", kp_prefs_get_str(p, "video", "vo", NULL));
    add(a, "ao", kp_prefs_get_str(p, "audio", "ao", NULL));
    const char *alang = kp_prefs_get_str(p, "audio", "languages", NULL);
    if (alang) {
        char *l = lang_list(alang);
        add(a, "alang", l);
        g_free(l);
    }
    const char *slang = kp_prefs_get_str(p, "subtitles", "languages", NULL);
    if (slang) {
        char *l = lang_list(slang);
        add(a, "slang", l);
        g_free(l);
    }
    add(a, "sub-font", kp_prefs_get_str(p, "subtitles", "font", NULL));
    int size = kp_prefs_get_int(p, "subtitles", "font_size", 0, 0, 500);
    if (size > 0)
        g_ptr_array_add(a, g_strdup_printf("--sub-font-size=%d", size));
    static const char *const autos[] = {"exact", "fuzzy", "no", NULL};
    int sub_auto = kp_prefs_get_choice(p, "subtitles", "auto", autos, 0);
    if (sub_auto != 0)
        add(a, "sub-auto", autos[sub_auto]);
    g_ptr_array_add(a, NULL);
    return (char **)g_ptr_array_free(a, FALSE);
}

char *kp_mpv_pref_signature(const kp_prefs *p)
{
    char **args = kp_mpv_pref_args(p);
    char *joined = g_strjoinv("\n", args);
    char *sig = g_strdup_printf("%s\n%s\n%d\n%s", joined,
                                kp_prefs_get_str(p, "mpv", "program", "mpv"),
                                kp_prefs_get_bool(p, "mpv", "use_config", false),
                                kp_prefs_get_str(p, "mpv", "extra", ""));
    g_free(joined);
    g_strfreev(args);
    return sig;
}
