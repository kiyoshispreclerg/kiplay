/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_INI_H
#define KP_INI_H

#include <stdbool.h>

/* A small INI document that can be read and edited in place:
 *
 *   # comment            ; comment too
 *   [section]
 *   key = value
 *
 * Every line is kept as written, so a file edited by kiplay keeps
 * the user's comments, order and blank lines; only the touched lines change.
 * Section and key names are case-insensitive. Values are trimmed; there is
 * no quoting or escaping (a value may contain "=", "#" and ";"). */

typedef struct kp_ini kp_ini;

/* Empty document. */
kp_ini *kp_ini_new(void);
/* Parses text. Malformed lines are kept (and skipped by the getters); each
 * one is reported to warn with its 1-based line number, if warn is set. */
kp_ini *kp_ini_parse(const char *text,
                     void (*warn)(void *ud, int line, const char *msg),
                     void *ud);
void kp_ini_free(kp_ini *ini);

/* NULL if absent. With repeated keys, the last one wins. */
const char *kp_ini_get(const kp_ini *ini, const char *section, const char *key);
/* Line number of that key (0 if absent), for messages. */
int kp_ini_line(const kp_ini *ini, const char *section, const char *key);

/* Iteration in file order. Returns false at the end. *section is "" for keys
 * before the first header. Pass i = 0 first, then what it leaves in *i. */
bool kp_ini_next(const kp_ini *ini, int *i, const char **section,
                 const char **key, const char **value, int *line);

/* Sections in file order, each once; same protocol as kp_ini_next. */
bool kp_ini_next_section(const kp_ini *ini, int *i, const char **section);

/* Replaces the value in place, or appends the key to its section (created at
 * the end if missing). */
int kp_ini_set(kp_ini *ini, const char *section, const char *key,
               const char *value);
/* Removes every line with that key. */
void kp_ini_unset(kp_ini *ini, const char *section, const char *key);
/* Removes the section header and its keys (comments inside go too). */
void kp_ini_remove_section(kp_ini *ini, const char *section);

/* The whole document as text; the caller frees it. */
char *kp_ini_dump(const kp_ini *ini);
/* Writes atomically (temp file + rename), creating the parent folders. */
int kp_ini_save(const kp_ini *ini, const char *path);

#endif
