/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_FILESETTINGS_H
#define KP_FILESETTINGS_H

#include <stdbool.h>

/* What Kiplay remembers about one file, like SMPlayer's file_settings:
 * where it stopped, the audio and subtitle tracks and the delays chosen,
 * the aspect ratio. One small INI per file under
 * ~/.local/share/kiplay/files/<2 hex>/<sha1>.ini, the SHA-1 of the
 * location and the file's size: the same path with another file in it
 * (re-downloaded, re-encoded) starts fresh.
 *
 * Only local files have settings (a stream has no size to check). An
 * empty kp_file_settings is not stored: saving one removes the file. */

typedef struct {
    double position;              /* where to resume, seconds; 0 = beginning */
    int aid, sid;                 /* chosen tracks; -1 = not chosen, sid 0 = none */
    double audio_delay, sub_delay; /* seconds */
    char aspect[16];              /* "16:9"...; "" = the file's own */
} kp_file_settings;

#define KP_FILE_SETTINGS_INIT {0, -1, -1, 0, 0, ""}

bool kp_file_settings_empty(const kp_file_settings *fs);

/* dir: where the settings live (NULL = the default under the data folder).
 * Path of the settings of location, or NULL if it isn't a local file that
 * exists. g_free it. */
char *kp_file_settings_path(const char *dir, const char *location);
/* Fills fs (KP_FILE_SETTINGS_INIT when nothing is stored); false if none. */
bool kp_file_settings_load(const char *dir, const char *location, kp_file_settings *fs);
/* Writes them (atomically), or removes them when empty. 0 or -1. */
int kp_file_settings_save(const char *dir, const char *location, const kp_file_settings *fs);

#endif
