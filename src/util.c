/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "i18n.h"
#include "log.h"

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

bool kp_check_usable(const char *text, size_t len)
{
    return len > 0 && !memchr(text, '\0', len);
}

bool kp_check_lines(const char *text, size_t len)
{
    return kp_check_usable(text, len) && text[len - 1] == '\n';
}

int kp_flush_sync(FILE *f)
{
    if (fflush(f) != 0)
        return -1;
    return fsync(fileno(f)) == 0 ? 0 : -1;
}

/* So the new name survives a power cut too. Best effort. */
static void sync_parent(const char *path)
{
    char dir[KP_PATH_MAX];
    if (!kp_pathf(dir, sizeof dir, "%s", path))
        return;
    char *slash = strrchr(dir, '/');
    if (!slash)
        return;
    if (slash == dir)
        slash[1] = '\0';
    else
        *slash = '\0';
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        (void)fsync(fd);
        close(fd);
    }
}

/* path.tmp, flushed, then renamed over path. mode < 0: the default one. */
static int write_atomic(const char *path, const char *text, size_t len, int mode)
{
    char tmp[KP_PATH_MAX];
    if (!kp_pathf(tmp, sizeof tmp, "%s.tmp", path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode < 0 ? 0666 : (mode_t)mode);
    if (fd < 0)
        return -1;
    FILE *f = fdopen(fd, "wb");
    if (!f) {
        int e = errno;
        close(fd);
        remove(tmp);
        errno = e;
        return -1;
    }
    /* open() applies the umask; the original's mode must come out as it was. */
    bool ok = (mode < 0 || fchmod(fd, (mode_t)mode) == 0) &&
              (len == 0 || fwrite(text, 1, len, f) == len) && kp_flush_sync(f) == 0;
    int e = errno;
    if (fclose(f) != 0 && ok) {
        ok = false;
        e = errno;
    }
    if (ok && rename(tmp, path) < 0) {
        ok = false;
        e = errno;
    }
    if (!ok) {
        remove(tmp);
        errno = e;
        return -1;
    }
    sync_parent(path);
    return 0;
}

int kp_backup_file(const char *path, kp_check_fn ok)
{
    char bak[KP_PATH_MAX];
    if (!kp_pathf(bak, sizeof bak, "%s.backup", path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    struct stat st;
    if (stat(path, &st) < 0)
        return errno == ENOENT ? 0 : -1;
    size_t len = 0;
    char *text = kp_read_file(path, &len);
    if (!text) {
        kp_log_warn(_("could not read %s to back it up"), path);
        return -1;
    }
    if (!ok(text, len)) {
        kp_log_warn(_("%s looks damaged: keeping the previous backup"), path);
        free(text);
        errno = EINVAL;
        return -1;
    }
    int r = write_atomic(bak, text, len, st.st_mode & 07777);
    int e = errno;
    free(text);
    if (r < 0)
        kp_log_warn(_("could not write %s"), bak);
    errno = e;
    return r;
}

char *kp_read_file_or_backup(const char *path, kp_check_fn ok, size_t *len)
{
    if (!ok)
        ok = kp_check_usable;
    size_t n = 0;
    char *text = kp_read_file(path, &n);
    if (text && ok(text, n)) {
        if (len)
            *len = n;
        return text;
    }
    int e = errno;
    char bak[KP_PATH_MAX];
    size_t bn = 0;
    char *b = kp_pathf(bak, sizeof bak, "%s.backup", path) ? kp_read_file(bak, &bn) : NULL;
    if (b && ok(b, bn)) {
        kp_log_warn(_("%s is missing or damaged: using the copy in %s"), path, bak);
        free(text);
        if (len)
            *len = bn;
        return b;
    }
    free(b);
    /* No usable copy: what kp_read_file gave, errno included. */
    errno = e;
    if (text && len)
        *len = n;
    return text;
}

int kp_save_file(const char *path, const char *text, size_t len, kp_check_fn backup_ok)
{
    kp_make_parent_dirs(path);
    if (backup_ok)
        (void)kp_backup_file(path, backup_ok);
    struct stat st;
    int mode = stat(path, &st) == 0 ? (int)(st.st_mode & 07777) : -1;
    return write_atomic(path, text, len, mode);
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
