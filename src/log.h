/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_LOG_H
#define KP_LOG_H

#include <stdbool.h>

/* Messages go to stderr prefixed with "kiplay: ". */
void kp_log_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kp_log_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kp_log_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Only with -v (kp_log_set_verbose). Not translated: for developers. */
void kp_log_set_verbose(bool on);
/* For debug output that costs something to build. */
bool kp_log_debug_enabled(void);
void kp_log_debug(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
