/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "prefs.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "i18n.h"
#include "ini.h"
#include "log.h"
#include "util.h"

struct kp_prefs {
    kp_ini *ini;
    char path[KP_PATH_MAX];
    bool dirty;
};

static void warn_line(void *ud, int line, const char *msg)
{
    const kp_prefs *p = ud;
    kp_log_warn("%s:%d: %s", p->path, line, msg);
}

kp_prefs *kp_prefs_load(const char *path)
{
    kp_prefs *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    if (path)
        kp_pathf(p->path, sizeof p->path, "%s", path);
    else
        kp_config_path(p->path, sizeof p->path, "kiplay.ini");

    char *text = p->path[0] ? kp_read_file_or_backup(p->path, NULL, NULL) : NULL;
    if (!text && p->path[0] && errno != ENOENT)
        kp_log_warn(_("could not read %s"), p->path);
    p->ini = text ? kp_ini_parse(text, warn_line, p) : kp_ini_new();
    free(text);
    if (!p->ini) {
        free(p);
        return NULL;
    }
    return p;
}

void kp_prefs_free(kp_prefs *p)
{
    if (!p)
        return;
    kp_ini_free(p->ini);
    free(p);
}

int kp_prefs_get_int(const kp_prefs *p, const char *section, const char *key,
                     int def, int min, int max)
{
    const char *s = kp_ini_get(p->ini, section, key);
    if (!s || !*s)
        return def;
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (*end || errno)
        return def;
    if (v < min)
        return min;
    if (v > max)
        return max;
    return (int)v;
}

void kp_prefs_set_int(kp_prefs *p, const char *section, const char *key, int v)
{
    char buf[16];
    snprintf(buf, sizeof buf, "%d", v);
    const char *old = kp_ini_get(p->ini, section, key);
    if (old && strcmp(old, buf) == 0)
        return;
    if (kp_ini_set(p->ini, section, key, buf) == 0)
        p->dirty = true;
}

const char *kp_prefs_get_str(const kp_prefs *p, const char *section, const char *key,
                             const char *def)
{
    const char *s = kp_ini_get(p->ini, section, key);
    return s && *s ? s : def;
}

void kp_prefs_set_str(kp_prefs *p, const char *section, const char *key, const char *v)
{
    const char *old = kp_ini_get(p->ini, section, key);
    if (old && strcmp(old, v) == 0)
        return;
    if (kp_ini_set(p->ini, section, key, v) == 0)
        p->dirty = true;
}

const char *kp_prefs_get_raw(const kp_prefs *p, const char *section, const char *key)
{
    return kp_ini_get(p->ini, section, key);
}

void kp_prefs_unset(kp_prefs *p, const char *section, const char *key)
{
    if (!kp_ini_get(p->ini, section, key))
        return;
    kp_ini_unset(p->ini, section, key);
    p->dirty = true;
}

bool kp_prefs_get_bool(const kp_prefs *p, const char *section, const char *key, bool def)
{
    const char *s = kp_ini_get(p->ini, section, key);
    if (!s)
        return def;
    if (!strcasecmp(s, "true") || !strcasecmp(s, "yes") || !strcmp(s, "1"))
        return true;
    if (!strcasecmp(s, "false") || !strcasecmp(s, "no") || !strcmp(s, "0"))
        return false;
    return def;
}

void kp_prefs_set_bool(kp_prefs *p, const char *section, const char *key, bool v)
{
    kp_prefs_set_str(p, section, key, v ? "true" : "false");
}

int kp_prefs_get_choice(const kp_prefs *p, const char *section, const char *key,
                        const char *const names[], int def)
{
    const char *s = kp_ini_get(p->ini, section, key);
    if (s)
        for (int i = 0; names[i]; i++)
            if (!strcasecmp(s, names[i]))
                return i;
    return def;
}

int kp_prefs_save(kp_prefs *p)
{
    if (!p->dirty)
        return 0;
    if (!p->path[0]) {
        errno = ENOENT;
        return -1;
    }
    if (kp_ini_save_backed_up(p->ini, p->path) < 0)
        return -1;
    p->dirty = false;
    return 0;
}
