/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "log.h"

#include <stdarg.h>
#include <stdio.h>

#include "i18n.h"

__attribute__((format(printf, 2, 0)))
static void vlog(const char *level, const char *fmt, va_list ap)
{
    fprintf(stderr, "kiplay: %s", level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void kp_log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("", fmt, ap);
    va_end(ap);
}

void kp_log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(_("warning: "), fmt, ap);
    va_end(ap);
}

void kp_log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(_("error: "), fmt, ap);
    va_end(ap);
}

static bool verbose;

void kp_log_set_verbose(bool on)
{
    verbose = on;
}

void kp_log_debug(const char *fmt, ...)
{
    if (!verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    vlog("", fmt, ap);
    va_end(ap);
}
