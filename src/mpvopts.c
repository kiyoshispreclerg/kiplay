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

static bool runs(const char *program)
{
    if (strchr(program, '/'))
        return g_file_test(program, G_FILE_TEST_IS_EXECUTABLE);
    char *found = g_find_program_in_path(program);
    g_free(found);
    return found != NULL;
}

bool kp_video_fallback(const kp_prefs *p)
{
    if (!g_strcmp0(kp_prefs_get_str(p, "video", "backend", "mpv"), "mplayer"))
        return false;
    /* Asked once per program name: the answer holds until Kiplay restarts. */
    static char *checked;
    static bool fallback;
    const char *mpv = kp_prefs_get_str(p, "mpv", "program", "mpv");
    if (g_strcmp0(checked, mpv) != 0) {
        g_free(checked);
        checked = g_strdup(mpv);
        fallback = !runs(mpv) && runs(kp_prefs_get_str(p, "mplayer", "program", "mplayer"));
    }
    return fallback;
}

const char *kp_video_backend(const kp_prefs *p)
{
    if (!g_strcmp0(kp_prefs_get_str(p, "video", "backend", "mpv"), "mplayer") ||
        kp_video_fallback(p))
        return "mplayer";
    return "mpv";
}

static void add_opt(GPtrArray *a, const char *option, const char *value)
{
    if (value && *value) {
        g_ptr_array_add(a, g_strdup(option));
        g_ptr_array_add(a, g_strdup(value));
    }
}

char **kp_mplayer_pref_args(const kp_prefs *p)
{
    GPtrArray *a = g_ptr_array_new();
    add_opt(a, "-vo", kp_prefs_get_str(p, "mplayer", "vo", NULL));
    add_opt(a, "-ao", kp_prefs_get_str(p, "mplayer", "ao", NULL));
    const char *alang = kp_prefs_get_str(p, "audio", "languages", NULL);
    if (alang) {
        char *l = lang_list(alang);
        add_opt(a, "-alang", l);
        g_free(l);
    }
    const char *slang = kp_prefs_get_str(p, "subtitles", "languages", NULL);
    if (slang) {
        char *l = lang_list(slang);
        add_opt(a, "-slang", l);
        g_free(l);
    }
    /* A fontconfig name; the size is MPlayer's own scale (percent of the
     * screen height), not mpv's pixels, so it is left alone. */
    add_opt(a, "-font", kp_prefs_get_str(p, "subtitles", "font", NULL));
    if (kp_prefs_get_choice(p, "subtitles", "auto",
                            (const char *const[]){"exact", "fuzzy", "no", NULL}, 0) == 2)
        g_ptr_array_add(a, g_strdup("-noautosub"));
    g_ptr_array_add(a, NULL);
    return (char **)g_ptr_array_free(a, FALSE);
}

char *kp_mpv_pref_signature(const kp_prefs *p)
{
    bool mplayer = !strcmp(kp_video_backend(p), "mplayer");
    char **args = mplayer ? kp_mplayer_pref_args(p) : kp_mpv_pref_args(p);
    char *joined = g_strjoinv("\n", args);
    const char *sect = mplayer ? "mplayer" : "mpv";
    char *sig = g_strdup_printf("%s\n%s\n%s\n%d\n%s", sect, joined,
                                kp_prefs_get_str(p, sect, "program", sect),
                                kp_prefs_get_bool(p, sect, "use_config", false),
                                kp_prefs_get_str(p, sect, "extra", ""));
    g_free(joined);
    g_strfreev(args);
    return sig;
}
