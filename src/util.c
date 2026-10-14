/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

char *kp_read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    char *buf = NULL;
    size_t size = 0, cap = 0;
    for (;;) {
        if (cap - size < 65536) {
            cap = cap ? cap * 2 : 1 << 20;
            char *nb = realloc(buf, cap + 1);
            if (!nb) {
                free(buf);
                fclose(f);
                errno = ENOMEM;
                return NULL;
            }
            buf = nb;
        }
        size_t n = fread(buf + size, 1, cap - size, f);
        size += n;
        if (n == 0)
            break;
    }
    if (ferror(f)) {
        free(buf);
        fclose(f);
        errno = EIO;
        return NULL;
    }
    fclose(f);
    buf[size] = '\0';
    if (len)
        *len = size;
    return buf;
}

bool kp_file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool kp_dir_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool kp_pathf(char *out, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, size, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= size) {
        if (size)
            out[0] = '\0';
        return false;
    }
    return true;
}

void kp_make_parent_dirs(const char *path)
{
    char buf[KP_PATH_MAX];
    if (!kp_pathf(buf, sizeof buf, "%s", path))
        return;
    for (char *p = buf + 1; *p; p++)
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
}

bool kp_config_path(char *out, size_t size, const char *name)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] == '/')
        return kp_pathf(out, size, "%s/kiplay/%s", xdg, name);
    const char *home = getenv("HOME");
    if (!home || !*home) {
        if (size)
            out[0] = '\0';
        return false;
    }
    return kp_pathf(out, size, "%s/.config/kiplay/%s", home, name);
}

bool kp_data_path(char *out, size_t size, const char *name)
{
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0] == '/')
        return kp_pathf(out, size, "%s/kiplay/%s", xdg, name);
    const char *home = getenv("HOME");
    if (!home || !*home) {
        if (size)
            out[0] = '\0';
        return false;
    }
    return kp_pathf(out, size, "%s/.local/share/kiplay/%s", home, name);
}

void kp_format_duration(char *out, size_t size, int64_t ms)
{
    if (ms < 0) {
        snprintf(out, size, "--:--");
        return;
    }
    long long t = ms / 1000;
    if (t >= 3600)
        snprintf(out, size, "%lld:%02lld:%02lld", t / 3600, t / 60 % 60, t % 60);
    else
        snprintf(out, size, "%lld:%02lld", t / 60, t % 60);
}
