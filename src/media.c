/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "media.h"

#include <glib.h>
#include <string.h>

/* Plain audio formats, game music (libgme) and MIDI go to kiplay-audio. */
static const char *const audio_exts[] = {
    "mp3", "mp2", "mp1", "ogg", "oga", "opus", "flac", "wav", "m4a", "aac", "wma", "ape",
    "wv",  "mpc", "spx", "aif", "aiff", "tta", "ac3", "dts", "mka", "au",  "snd", "w64",
    "nsf", "nsfe", "spc", "gbs", "vgm", "vgz", "gym", "hes", "kss", "ay",  "sap",
    "mid", "midi", "kar", NULL};

/* Video, and audio only mpv plays. */
static const char *const video_exts[] = {
    "mp4", "m4v", "mkv", "webm", "avi", "mov", "wmv", "asf", "flv", "mpg", "mpeg", "mpe",
    "ts",  "m2ts", "mts", "vob", "ogv", "ogm", "3gp", "3g2", "rm", "rmvb", "divx", "f4v",
    "dv",  "nsv", "mod", "xm", "it", "s3m", NULL};

static const char *const playlist_exts[] = {"m3u", "m3u8", "pls", NULL};

/* The extension, lower case, without a "?N" subtune; NULL if none. */
static char *extension(const char *location)
{
    char *s = g_ascii_strdown(location, -1);
    char *q = strrchr(s, '?');
    if (q && q[1] && q[1 + strspn(q + 1, "0123456789")] == '\0')
        *q = '\0';
    const char *slash = strrchr(s, '/');
    const char *dot = strrchr(slash ? slash : s, '.');
    char *ext = dot && dot[1] ? g_strdup(dot + 1) : NULL;
    g_free(s);
    return ext;
}

static bool in(const char *const list[], const char *ext)
{
    for (int i = 0; ext && list[i]; i++)
        if (!strcmp(list[i], ext))
            return true;
    return false;
}

kp_media_kind kp_media_kind_of(const char *location)
{
    char *ext = extension(location);
    kp_media_kind k = in(audio_exts, ext) ? KP_MEDIA_AUDIO
                      : in(video_exts, ext) ? KP_MEDIA_VIDEO
                                            : KP_MEDIA_NONE;
    g_free(ext);
    return k;
}

bool kp_media_video_has_sound(const char *location)
{
    char *ext = extension(location);
    static const char *const trackers[] = {"mod", "xm", "it", "s3m", NULL};
    bool yes = kp_media_kind_of(location) == KP_MEDIA_VIDEO && !in(trackers, ext);
    g_free(ext);
    return yes;
}

bool kp_media_is_playlist(const char *location)
{
    char *ext = extension(location);
    bool yes = in(playlist_exts, ext);
    g_free(ext);
    return yes;
}

char *kp_media_location(const char *location)
{
    char *path = g_str_has_prefix(location, "file://") ? g_filename_from_uri(location, NULL, NULL)
                 : location[0] == '/'                    ? g_strdup(location)
                                                         : NULL;
    if (!path)
        return g_strdup(location);
    char *q = strrchr(path, '?');
    if (q && q[1] && q[1 + strspn(q + 1, "0123456789")] == '\0' &&
        !g_file_test(path, G_FILE_TEST_EXISTS)) {
        *q = '\0';
        if (g_file_test(path, G_FILE_TEST_EXISTS)) {
            char *uri = g_filename_to_uri(path, NULL, NULL);
            char *with = uri ? g_strdup_printf("%s?%s", uri, q + 1) : NULL;
            g_free(uri);
            if (with) {
                g_free(path);
                return with;
            }
        }
        *q = '?';
    }
    return path;
}
