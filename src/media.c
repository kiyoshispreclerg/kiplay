/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "media.h"

#include <glib.h>
#include <string.h>

/* Plain audio formats, and game music (libgme), go to kiplay-audio. */
static const char *const audio_exts[] = {
    "mp3", "mp2", "mp1", "ogg", "oga", "opus", "flac", "wav", "m4a", "aac", "wma", "ape",
    "wv",  "mpc", "spx", "aif", "aiff", "tta", "ac3", "dts", "mka", "au",  "snd", "w64",
    "nsf", "nsfe", "spc", "gbs", "vgm", "vgz", "gym", "hes", "kss", "ay",  "sap", NULL};

/* Video, and audio only mpv plays (MIDI goes to mpv until kiplay-audio
 * has a synthesizer; it fails there too, but the file is the user's). */
static const char *const video_exts[] = {
    "mp4", "m4v", "mkv", "webm", "avi", "mov", "wmv", "asf", "flv", "mpg", "mpeg", "mpe",
    "ts",  "m2ts", "mts", "vob", "ogv", "ogm", "3gp", "3g2", "rm", "rmvb", "divx", "f4v",
    "dv",  "nsv", "mid", "midi", "mod", "xm", "it", "s3m", NULL};

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

bool kp_media_is_playlist(const char *location)
{
    char *ext = extension(location);
    bool yes = in(playlist_exts, ext);
    g_free(ext);
    return yes;
}
