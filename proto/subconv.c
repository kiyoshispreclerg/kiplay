/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Phase 0 prototype: srv3 -> ASS on stdout.
 *   subconv [-s WxH] [-e EM_RATIO] [-g EDGE_SCALE] file.srv3 > file.ass
 * Becomes `kiplay --convert` once the program exists. */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "srv3.h"
#include "ytsub_ass.h"

int main(int argc, char **argv)
{
    kp_ytass_opts o;
    kp_ytass_opts_default(&o);
    int i = 1;
    for (; i + 1 < argc && argv[i][0] == '-'; i += 2) {
        if (!strcmp(argv[i], "-s"))
            sscanf(argv[i + 1], "%dx%d", &o.width, &o.height);
        else if (!strcmp(argv[i], "-e"))
            o.em_ratio = g_ascii_strtod(argv[i + 1], NULL);
        else if (!strcmp(argv[i], "-g"))
            o.edge_scale = g_ascii_strtod(argv[i + 1], NULL);
    }
    if (i >= argc) {
        fprintf(stderr, "usage: %s [-s WxH] [-e fraction] [-g outline] file.srv3\n", argv[0]);
        return 2;
    }
    char *err = NULL;
    kp_ytsub *s = kp_srv3_load(argv[i], &err);
    if (!s) {
        fprintf(stderr, "%s: %s\n", argv[i], err);
        g_free(err);
        return 1;
    }
    char *ass = kp_ytsub_to_ass(s, &o);
    fputs(ass, stdout);
    fprintf(stderr, "%zu events, %zu pens, %zu positions\n", s->nevents, s->npens, s->npos);
    g_free(ass);
    kp_ytsub_free(s);
    return 0;
}
