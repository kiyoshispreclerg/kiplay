/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_PREFS_H
#define KP_PREFS_H

#include <stdbool.h>

/* What the window remembers between runs (layout, size, playlists...), kept in
 * ~/.config/kiplay/kiplay.ini. It is an ordinary INI the person may edit:
 * comments and unknown keys survive a save.
 *
 * kp_prefs owns its kp_ini and the path. Missing or unreadable files give
 * empty prefs (the getters then return the defaults); a broken line is
 * skipped with a warning. */

typedef struct kp_prefs kp_prefs;

/* path NULL = the default place (kp_config_path). Never returns NULL unless
 * out of memory. */
kp_prefs *kp_prefs_load(const char *path);
void kp_prefs_free(kp_prefs *p);

/* The default when absent or not a number, and clamped to [min, max]. */
int kp_prefs_get_int(const kp_prefs *p, const char *section, const char *key,
                     int def, int min, int max);
void kp_prefs_set_int(kp_prefs *p, const char *section, const char *key, int v);

/* The default when absent or empty. The pointer is valid until the next set. */
const char *kp_prefs_get_str(const kp_prefs *p, const char *section, const char *key,
                             const char *def);
void kp_prefs_set_str(kp_prefs *p, const char *section, const char *key, const char *v);

/* true/yes/1 and false/no/0 (any case); anything else gives the default. */
bool kp_prefs_get_bool(const kp_prefs *p, const char *section, const char *key, bool def);
void kp_prefs_set_bool(kp_prefs *p, const char *section, const char *key, bool v);

/* Index of the value in names (NULL-terminated, any case), else def. */
int kp_prefs_get_choice(const kp_prefs *p, const char *section, const char *key,
                        const char *const names[], int def);

/* Writes only if something changed. 0 or -1 (errno set). */
int kp_prefs_save(kp_prefs *p);

#endif
