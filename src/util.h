/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_UTIL_H
#define KP_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KP_PATH_MAX 4096

/* Reads a whole file into a NUL-terminated buffer; NULL on error (errno set).
 * The caller frees it. */
char *kp_read_file(const char *path, size_t *len);

bool kp_file_exists(const char *path);
bool kp_dir_exists(const char *path);

/* mkdir -p for the directory part of path (errors are left to whoever
 * then creates the file). */
void kp_make_parent_dirs(const char *path);

/* snprintf for paths: false (and an empty string) if it would not fit. */
bool kp_pathf(char *out, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* $XDG_CONFIG_HOME/kiplay/<name> (or ~/.config/...); false without $HOME
 * or if it does not fit. */
bool kp_config_path(char *out, size_t size, const char *name);

/* Playback times as players show them: "3:18", "1:02:03"; "--:--" for a
 * negative (unknown) duration. */
void kp_format_duration(char *out, size_t size, int64_t ms);

#endif
