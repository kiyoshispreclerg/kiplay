/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_UTIL_H
#define KP_UTIL_H

#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#define KP_PATH_MAX 4096

/* Reads a whole file into a NUL-terminated buffer; NULL on error (errno set).
 * The caller frees it. */
char *kp_read_file(const char *path, size_t *len);

/* ---- Saving the user's own files so a power cut cannot lose them --------
 *
 * What the user would miss (preferences, playlists) is written to
 * <path>.tmp, flushed to the disk and renamed over <path>; before that the
 * current <path> is copied the same way to <path>.backup, but only if it is
 * sound, so a damaged file never replaces a good backup. Reading falls back
 * to the backup when the file is missing or unusable.
 *
 * "Sound" depends on the format: a kp_check_fn gets the whole file (NUL
 * terminated after len bytes) and says whether it is. */
typedef bool (*kp_check_fn)(const char *text, size_t len);

/* Not empty and no NUL bytes: what a power cut leaves behind is an empty
 * or a zeroed file. The test for being read. */
bool kp_check_usable(const char *text, size_t len);
/* Usable and the last line is complete (ends with '\n'). */
bool kp_check_lines(const char *text, size_t len);

/* fflush + fsync. 0 on success, -1 (errno set) otherwise. */
int kp_flush_sync(FILE *f);

/* Copies path to path.backup (atomically, same mode) if ok accepts it.
 * 0 if done or there was nothing to copy; -1 (errno set, a warning
 * logged unless the file is missing) if not: a damaged file gives EINVAL. */
int kp_backup_file(const char *path, kp_check_fn ok);

/* Like kp_read_file, but when path is missing or ok rejects it and
 * path.backup is accepted, returns the backup (and logs which). Otherwise
 * it is exactly kp_read_file(path). ok NULL means kp_check_usable. */
char *kp_read_file_or_backup(const char *path, kp_check_fn ok, size_t *len);

/* Writes len bytes to path atomically and durably (temp file, fsync,
 * rename, fsync of the folder), creating the parent folders and keeping the
 * mode of the file it replaces. With backup_ok the previous file is first
 * saved as path.backup (see kp_backup_file); failing to do that is only
 * logged. 0 on success, -1 (errno set) otherwise. */
int kp_save_file(const char *path, const char *text, size_t len, kp_check_fn backup_ok);

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

/* $XDG_DATA_HOME/kiplay/<name> (or ~/.local/share/...); same rules. */
bool kp_data_path(char *out, size_t size, const char *name);

/* Playback times as players show them: "3:18", "1:02:03"; "--:--" for a
 * negative (unknown) duration. */
void kp_format_duration(char *out, size_t size, int64_t ms);

#endif
