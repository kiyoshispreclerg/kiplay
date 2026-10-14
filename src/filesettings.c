/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "filesettings.h"

#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ini.h"
#include "log.h"
#include "util.h"

#define SECTION "file"

bool kp_file_settings_empty(const kp_file_settings *fs)
{
    return fs->position <= 0 && fs->aid < 0 && fs->sid < 0 && fs->audio_delay == 0 &&
           fs->sub_delay == 0 && !fs->aspect[0];
}

/* The file behind a location: a path, or a file:// URI (a "?N" subtune
 * keeps its own settings, but the size is the file's). */
static char *local_file(const char *location)
{
    if (location[0] == '/')
        return g_strdup(location);
    if (!g_str_has_prefix(location, "file://"))
        return NULL;
    char *copy = g_strdup(location);
    char *q = strrchr(copy, '?');
    if (q)
        *q = '\0';
    char *path = g_filename_from_uri(copy, NULL, NULL);
    g_free(copy);
    return path;
}

char *kp_file_settings_path(const char *dir, const char *location)
{
    char *file = local_file(location);
    struct stat st;
    bool ok = file && stat(file, &st) == 0 && S_ISREG(st.st_mode);
    g_free(file);
    if (!ok)
        return NULL;
    char def[KP_PATH_MAX];
    if (!dir) {
        if (!kp_data_path(def, sizeof def, "files"))
            return NULL;
        dir = def;
    }
    char *key = g_strdup_printf("%s\n%lld", location, (long long)st.st_size);
    char *sum = g_compute_checksum_for_string(G_CHECKSUM_SHA1, key, -1);
    char sub[3] = {sum[0], sum[1], '\0'};
    char *name = g_strconcat(sum, ".ini", NULL);
    char *path = g_build_filename(dir, sub, name, NULL);
    g_free(name);
    g_free(sum);
    g_free(key);
    return path;
}

static double get_double(const kp_ini *ini, const char *key)
{
    const char *s = kp_ini_get(ini, SECTION, key);
    double v = s ? g_ascii_strtod(s, NULL) : 0;
    return isfinite(v) ? v : 0;
}

static int get_int(const kp_ini *ini, const char *key)
{
    const char *s = kp_ini_get(ini, SECTION, key);
    if (!s || !*s)
        return -1;
    char *end;
    gint64 v = g_ascii_strtoll(s, &end, 10);
    return !*end && v >= 0 && v < 100000 ? (int)v : -1;
}

bool kp_file_settings_load(const char *dir, const char *location, kp_file_settings *fs)
{
    *fs = (kp_file_settings)KP_FILE_SETTINGS_INIT;
    char *path = kp_file_settings_path(dir, location);
    char *text = path ? kp_read_file(path, NULL) : NULL;
    g_free(path);
    if (!text)
        return false;
    kp_ini *ini = kp_ini_parse(text, NULL, NULL);
    free(text);
    if (!ini)
        return false;
    fs->position = get_double(ini, "position");
    if (fs->position < 0)
        fs->position = 0;
    fs->aid = get_int(ini, "aid");
    fs->sid = get_int(ini, "sid");
    fs->audio_delay = get_double(ini, "audio_delay");
    fs->sub_delay = get_double(ini, "sub_delay");
    const char *aspect = kp_ini_get(ini, SECTION, "aspect");
    g_strlcpy(fs->aspect, aspect ? aspect : "", sizeof fs->aspect);
    kp_ini_free(ini);
    return true;
}

static void set_double(kp_ini *ini, const char *key, double v)
{
    char num[G_ASCII_DTOSTR_BUF_SIZE];
    if (v != 0)
        kp_ini_set(ini, SECTION, key, g_ascii_formatd(num, sizeof num, "%.3f", v));
}

static void set_int(kp_ini *ini, const char *key, int v)
{
    char num[16];
    if (v >= 0) {
        g_snprintf(num, sizeof num, "%d", v);
        kp_ini_set(ini, SECTION, key, num);
    }
}

int kp_file_settings_save(const char *dir, const char *location, const kp_file_settings *fs)
{
    char *path = kp_file_settings_path(dir, location);
    if (!path) {
        errno = ENOENT;
        return -1;
    }
    int ret = 0;
    if (kp_file_settings_empty(fs)) {
        if (g_remove(path) < 0 && errno != ENOENT)
            ret = -1;
        g_free(path);
        return ret;
    }
    kp_ini *ini = kp_ini_new();
    /* The location only helps whoever looks at the folder. */
    if (!strchr(location, '\n'))
        kp_ini_set(ini, SECTION, "location", location);
    set_double(ini, "position", fs->position);
    set_int(ini, "aid", fs->aid);
    set_int(ini, "sid", fs->sid);
    set_double(ini, "audio_delay", fs->audio_delay);
    set_double(ini, "sub_delay", fs->sub_delay);
    if (fs->aspect[0])
        kp_ini_set(ini, SECTION, "aspect", fs->aspect);
    ret = kp_ini_save(ini, path);
    if (ret < 0)
        kp_log_debug("file settings: %s: %s", path, g_strerror(errno));
    kp_ini_free(ini);
    g_free(path);
    return ret;
}
