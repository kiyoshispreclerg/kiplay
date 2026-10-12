/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cmd.h"

#include <stdlib.h>
#include <string.h>

/* line starts with word followed by a space or the end. */
static const char *after(const char *line, const char *word)
{
    size_t n = strlen(word);
    if (strncmp(line, word, n) != 0 || (line[n] != ' ' && line[n] != '\0'))
        return NULL;
    return line[n] == ' ' ? line + n + 1 : line + n;
}

static bool number(const char *s, double *out, const char **rest)
{
    char *end;
    *out = strtod(s, &end);
    if (end == s)
        return false;
    if (rest)
        *rest = *end == ' ' ? end + 1 : end;
    return rest || *end == '\0';
}

bool ka_cmd_parse(const char *line, ka_cmd *cmd)
{
    static const struct {
        const char *word;
        ka_cmd_type type;
    } simple[] = {
        {"pause", KA_CMD_PAUSE}, {"seek", KA_CMD_SEEK},   {"seekrel", KA_CMD_SEEKREL},
        {"volume", KA_CMD_VOLUME}, {"tick", KA_CMD_TICK},
    };
    const char *a;
    memset(cmd, 0, sizeof *cmd);
    if ((a = after(line, "load"))) {
        const char *path;
        if (!number(a, &cmd->num, &path) || !*path)
            return false;
        cmd->type = KA_CMD_LOAD;
        cmd->path = path;
        return true;
    }
    if ((a = after(line, "queue"))) {
        cmd->type = KA_CMD_QUEUE;
        cmd->path = a;
        return true;
    }
    if (after(line, "stop") && !line[4]) {
        cmd->type = KA_CMD_STOP;
        return true;
    }
    if (after(line, "quit") && !line[4]) {
        cmd->type = KA_CMD_QUIT;
        return true;
    }
    for (size_t i = 0; i < sizeof simple / sizeof simple[0]; i++) {
        if ((a = after(line, simple[i].word)) && number(a, &cmd->num, NULL)) {
            cmd->type = simple[i].type;
            return true;
        }
    }
    return false;
}
