/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gtk/gtk.h>

#include "i18n.h"
#include "instance.h"
#include "log.h"
#include "sidecar.h"
#include "util.h"
#include "window.h"

/* Folder of the executable, "" if unknown. */
static char self_dir[KP_PATH_MAX];

static void find_self_dir(void)
{
    ssize_t n = readlink("/proc/self/exe", self_dir, sizeof self_dir - 1);
    self_dir[n > 0 ? n : 0] = '\0';
    char *slash = strrchr(self_dir, '/');
    if (slash)
        *slash = '\0';
    else
        self_dir[0] = '\0';
}

/* Translations: locale/ next to the executable (a build tree), else the
 * installed ones. */
static void setup_gettext(void)
{
    char dir[KP_PATH_MAX];
    const char *localedir = LOCALEDIR;
    if (self_dir[0] && kp_pathf(dir, sizeof dir, "%s/locale", self_dir) && kp_dir_exists(dir))
        localedir = dir;
    bindtextdomain(KP_GETTEXT_DOMAIN, localedir);
    bind_textdomain_codeset(KP_GETTEXT_DOMAIN, "UTF-8");
    textdomain(KP_GETTEXT_DOMAIN);
}

static void usage(FILE *f)
{
    fprintf(f, _("Usage: kiplay [OPTION]... [FILE]...\n"
                 "Audio and video player with tabbed playlists.\n"
                 "The files go to the playlist on screen and the first one plays; if\n"
                 "Kiplay is already open, they go to that window.\n"
                 "\n"
                 "  -e, --enqueue        add the files without playing them\n"
                 "  -c, --config FILE    settings file (default ~/.config/kiplay/kiplay.ini)\n"
                 "      --convert FILE   write YouTube captions (srv3, json3) as ASS to the\n"
                 "                       standard output and exit\n"
                 "  -v, --verbose        print what is going on\n"
                 "  -V, --version        print the version and exit\n"
                 "  -h, --help           print this help and exit\n"));
}

/* kiplay --convert X.srv3 [-s WxH] > X.ass: the same conversion the
 * player does, for other players and for checking the result. */
static int convert_main(int argc, char **argv)
{
    kp_ytass_opts o;
    kp_ytass_opts_default(&o);
    const char *file = NULL;
    bool bad = false;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc)
            bad = sscanf(argv[++i], "%dx%d", &o.width, &o.height) != 2;
        else if (!strcmp(argv[i], "-g") && i + 1 < argc)
            o.edge_scale = g_ascii_strtod(argv[++i], NULL);
        else if (!file && argv[i][0] != '-')
            file = argv[i];
        else
            bad = true;
    }
    if (bad || !file || o.width < 1 || o.height < 1) {
        fprintf(stderr, _("Usage: kiplay --convert FILE.srv3|FILE.json3 [-s WIDTHxHEIGHT] [-g EDGE]\n"));
        return 2;
    }
    char *err = NULL;
    kp_ytsub *sub = kp_ytsub_load(file, &err);
    if (!sub) {
        fprintf(stderr, "%s: %s\n", file, err);
        g_free(err);
        return 1;
    }
    char *ass = kp_ytsub_to_ass(sub, &o);
    fputs(ass, stdout);
    g_free(ass);
    kp_ytsub_free(sub);
    return 0;
}

int main(int argc, char **argv)
{
    kp_window_opts opts = {.start_us = g_get_monotonic_time()};
    GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
    bool options_done = false;
    setlocale(LC_ALL, "");
    find_self_dir();
    setup_gettext();
    if (argc > 1 && !strcmp(argv[1], "--convert"))
        return convert_main(argc - 2, argv + 2);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool more = i + 1 < argc;
        if (options_done || a[0] != '-' || strcmp(a, "-") == 0) {
            /* Playlists keep absolute paths: a relative one would mean
             * something else from the next folder Kiplay runs in. */
            g_ptr_array_add(files, kp_instance_absolute(argv[i]));
        } else if (strcmp(a, "--") == 0) {
            options_done = true;
        } else if (strcmp(a, "-e") == 0 || strcmp(a, "--enqueue") == 0) {
            opts.enqueue = true;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            g_ptr_array_free(files, TRUE);
            return 0;
        } else if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            printf("kiplay %s\n", KP_VERSION);
            g_ptr_array_free(files, TRUE);
            return 0;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
            kp_log_set_verbose(true);
        } else if ((strcmp(a, "-c") == 0 || strcmp(a, "--config") == 0) && more) {
            opts.prefs_path = argv[++i];
        } else if (strcmp(a, "--exit-after-map") == 0) {
            /* Undocumented: for startup timing and tests. */
            opts.exit_after_map = true;
        } else {
            usage(stderr);
            g_ptr_array_free(files, TRUE);
            return 2;
        }
    }

    g_ptr_array_add(files, NULL);
    opts.files = (char **)files->pdata;

    /* Already open: hand the files over and leave, before paying for GTK. */
    char dir[KP_PATH_MAX];
    char *sock = kp_config_path(dir, sizeof dir, "") ? kp_instance_socket_path(dir) : NULL;
    if (sock && kp_instance_forward(sock, opts.files, opts.enqueue)) {
        kp_log_debug("handed %u file(s) to the running kiplay", files->len - 1);
        g_free(sock);
        g_ptr_array_free(files, TRUE);
        return 0;
    }
    opts.instance_socket = sock;

    if (!gtk_init_check(&argc, &argv)) {
        kp_log_error(_("cannot open the display"));
        return 1;
    }
    kp_log_debug("%.1f ms: gtk_init", (double)(g_get_monotonic_time() - opts.start_us) / 1000.0);
    /* Run from a build tree: the icon is still in ../data. */
    char data[KP_PATH_MAX];
    if (self_dir[0] && kp_pathf(data, sizeof data, "%s/../data/kiplay.svg", self_dir) &&
        kp_file_exists(data)) {
        data[strlen(data) - strlen("/kiplay.svg")] = '\0';
        gtk_icon_theme_append_search_path(gtk_icon_theme_get_default(), data);
    }
    int ret = kp_window_run(&opts);
    g_free(sock);
    g_ptr_array_free(files, TRUE);
    return ret;
}
