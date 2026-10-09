/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include <string.h>

#include "../harness.h"
#include "util.h"

TEST(pathf_fails_instead_of_truncating)
{
    char buf[8];
    CHECK(kp_pathf(buf, sizeof buf, "%s", "1234567"));
    CHECK_STR_EQ(buf, "1234567");
    CHECK(!kp_pathf(buf, sizeof buf, "%s", "12345678"));
    CHECK_STR_EQ(buf, "");
}

TEST(durations_like_players)
{
    char s[32];
    kp_format_duration(s, sizeof s, 0);
    CHECK_STR_EQ(s, "0:00");
    kp_format_duration(s, sizeof s, 198556);
    CHECK_STR_EQ(s, "3:18");
    kp_format_duration(s, sizeof s, 3723000);
    CHECK_STR_EQ(s, "1:02:03");
    kp_format_duration(s, sizeof s, -1);
    CHECK_STR_EQ(s, "--:--");
}

TEST(config_path_follows_xdg)
{
    char p[KP_PATH_MAX];
    const char *old_xdg = getenv("XDG_CONFIG_HOME");
    char *saved = old_xdg ? strdup(old_xdg) : NULL;

    setenv("XDG_CONFIG_HOME", "/x/cfg", 1);
    CHECK(kp_config_path(p, sizeof p, "kiplay.ini"));
    CHECK_STR_EQ(p, "/x/cfg/kiplay/kiplay.ini");
    /* Relative XDG paths are invalid per the spec: fall back to $HOME. */
    setenv("XDG_CONFIG_HOME", "rel", 1);
    setenv("HOME", "/h", 1);
    CHECK(kp_config_path(p, sizeof p, "kiplay.ini"));
    CHECK_STR_EQ(p, "/h/.config/kiplay/kiplay.ini");

    if (saved)
        setenv("XDG_CONFIG_HOME", saved, 1);
    else
        unsetenv("XDG_CONFIG_HOME");
    free(saved);
}

int main(void)
{
    RUN(pathf_fails_instead_of_truncating);
    RUN(durations_like_players);
    RUN(config_path_follows_xdg);
    return harness_report();
}
