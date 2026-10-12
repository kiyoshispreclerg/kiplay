/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KA_CMD_H
#define KA_CMD_H

#include <stdbool.h>

/* kiplay-audio's protocol: one command per line on stdin, one event per
 * line on stdout, numbers with a dot. Paths are the rest of the line, so
 * they may contain spaces (never a newline: Kiplay sends those as URIs).
 *
 *   load <start seconds> <path>    play it (replaces the current file)
 *   queue [<path>]                 play this right after, no gap (none: clear)
 *   stop | pause 0|1 | seek <s> | seekrel <s> | volume <0..100>
 *   tick 0|1                       send "time" 4x a second while playing
 *   quit
 *
 * Events: loaded, duration <s>, tags <Artist - Title>, time <s>,
 * paused 0|1, next (the queued file took over), ended ok,
 * ended error <message>. */

typedef enum {
    KA_CMD_NONE,
    KA_CMD_LOAD,
    KA_CMD_QUEUE,
    KA_CMD_STOP,
    KA_CMD_PAUSE,
    KA_CMD_SEEK,
    KA_CMD_SEEKREL,
    KA_CMD_VOLUME,
    KA_CMD_TICK,
    KA_CMD_QUIT,
} ka_cmd_type;

typedef struct {
    ka_cmd_type type;
    double num;
    const char *path; /* points into the line; "" for "queue" alone */
} ka_cmd;

/* false (type KA_CMD_NONE) for anything it doesn't understand. */
bool ka_cmd_parse(const char *line, ka_cmd *cmd);

#endif
