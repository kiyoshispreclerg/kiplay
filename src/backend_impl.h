/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_BACKEND_IMPL_H
#define KP_BACKEND_IMPL_H

#include "backend.h"

/* For the backend implementations only (mpv.c, audioback.c): each struct
 * starts with kp_backend, and backend.c forwards the public calls through
 * its ops. */

typedef struct {
    const char *name;
    unsigned caps; /* KP_CAP_* */
    bool (*start)(kp_backend *b, char **err);
    bool (*running)(const kp_backend *b);
    int (*pid)(const kp_backend *b);
    void (*load)(kp_backend *b, const char *path, double start);
    void (*queue)(kp_backend *b, const char *path); /* only with KP_CAP_GAPLESS */
    void (*stop)(kp_backend *b);
    void (*set_pause)(kp_backend *b, bool paused);
    void (*seek)(kp_backend *b, double seconds, bool relative);
    void (*set_volume)(kp_backend *b, int volume);
    void (*set_ticking)(kp_backend *b, bool on);
    void (*free)(kp_backend *b);
} kp_backend_ops;

struct kp_backend {
    const kp_backend_ops *ops;
};

#endif
