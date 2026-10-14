#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Kiplay × Audacious with the same playlists (docs/BENCH.md): time to open,
# memory and CPU idle, and playing the same entry of the same playlist.
# Test tool, not part of Kiplay.
#
#   tools/bench-compare.sh -k KIPLAY_HOME -a AUDACIOUS_HOME[:name] [-a ...]
#                          [-d :78] [-w 10] [-s 20] [-n 3] [-o OUT.tsv] SCENARIOS.tsv
#
# KIPLAY_HOME and AUDACIOUS_HOME are XDG_CONFIG_HOME folders: the first
# made by tools/audpl2m3u8.py, the second a copy of ~/.config/audacious
# (never the real one: Audacious writes its config when it quits). More
# than one -a compares Audacious set up in different ways (the name goes
# in the "player" column), e.g. with and without the spectrum of its Qt
# window ([qtui] infoarea_show_vis).
# SCENARIOS.tsv has one line per test, tab-separated:
#
#   label <TAB> tab number (1...) <TAB> position (1...) <TAB> location
#
# "idle" as the label (tab, the rest empty) measures both stopped with
# that tab shown. Sound goes to $PULSE_SINK (make a null sink with
# `pactl load-module module-null-sink sink_name=kpbench`), so nothing is
# heard and both are paced by a real clock.
#
# Memory: Private_Dirty of the player alone and with all its children
# (kiplay-audio, mpv, the tag reader), after -w seconds (the tag reader
# goes 5 s after its last file: warm up longer than that). CPU: their
# utime+stime over the next -s seconds.
set -u

display=:78 warm=10 secs=20 runs=3 out=/dev/stdout kp_home=
aud_homes=()
while getopts "k:a:d:w:s:n:o:" o; do
    case $o in
    k) kp_home=$OPTARG ;; a) aud_homes+=("$OPTARG") ;; d) display=$OPTARG ;;
    w) warm=$OPTARG ;; s) secs=$OPTARG ;; n) runs=$OPTARG ;; o) out=$OPTARG ;;
    *) sed -n '4,25p' "$0"; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
scenarios=${1:?scenarios file}
[ -n "$kp_home" ] && [ ${#aud_homes[@]} -gt 0 ] || { echo "need -k and -a" >&2; exit 2; }
[ -n "${PULSE_SINK:-}" ] || { echo "set PULSE_SINK (a null sink)" >&2; exit 2; }
here=$(cd "$(dirname "$0")/.." && pwd)
kiplay=$here/build/kiplay
export DISPLAY=$display NO_AT_BRIDGE=1 GTK2_RC_FILES=
W=1280 H=780
hz=$(getconf CLK_TCK)

tree() { # pid and its descendants
    local p=$1
    echo "$p"
    for c in $(pgrep -P "$p"); do tree "$c"; done
}
mem_kb() { # Private_Dirty, kB
    local t=0 v
    for p in $(tree "$1"); do
        v=$(awk '/^Private_Dirty/{print $2}' "/proc/$p/smaps_rollup" 2>/dev/null)
        t=$((t + ${v:-0}))
    done
    echo $t
}
ticks() { # utime+stime of the tree; children that ended count too (cutime)
    local t=0 v
    for p in $(tree "$1"); do
        v=$(awk '{print $14+$15+$16+$17}' "/proc/$p/stat" 2>/dev/null)
        t=$((t + ${v:-0}))
    done
    echo $t
}
measure() { # pid -> "main_MB all_MB cpu_%"
    local m a t0 t1
    m=$(awk '/^Private_Dirty/{print $2}' "/proc/$1/smaps_rollup")
    a=$(mem_kb "$1")
    t0=$(ticks "$1")
    sleep "$secs"
    t1=$(ticks "$1")
    awk -v m="$m" -v a="$a" -v d=$((t1 - t0)) -v hz="$hz" -v s="$secs" \
        'BEGIN { printf "%.1f\t%.1f\t%.2f", m / 1024, a / 1024, 100 * d / hz / s }'
}
now() { date +%s.%N; }
wait_window() { # xdotool search args...; prints ms since $t0
    # (--sync polls every half second: too coarse)
    for _ in $(seq 1500); do
        xdotool search --onlyvisible "$@" > /dev/null 2>&1 && break
        sleep 0.02
    done
    awk -v a="$t0" -v b="$(now)" 'BEGIN { printf "%.0f", (b - a) * 1000 }'
}
ini_set() { # file section key value
    python3 - "$@" << 'EOF'
import sys
path, sec, key, val = sys.argv[1:]
lines = open(path).read().splitlines()
out, cur, done = [], None, False
for l in lines:
    s = l.strip()
    if s.startswith("["):
        if cur == sec and not done:
            out.append(f"{key} = {val}"); done = True
        cur = s[1:-1]
    elif cur == sec and s.split("=")[0].strip() == key:
        if not done:
            out.append(f"{key} = {val}"); done = True
        continue
    out.append(l)
if not done:
    if cur != sec:
        out.append(f"[{sec}]")
    out.append(f"{key} = {val}")
open(path, "w").write("\n".join(out) + "\n")
EOF
}

# ---- Kiplay ------------------------------------------------------------------

kp_ini=$kp_home/kiplay/kiplay.ini
ini_set "$kp_ini" playback device default
ini_set "$kp_ini" mpv extra ""
ini_set "$kp_ini" window width $W
ini_set "$kp_ini" window height $H

kp_start() { # tab -> pid in $kp_pid, open ms in $kp_open
    ini_set "$kp_ini" playlists active "$1"
    t0=$(now)
    XDG_CONFIG_HOME=$kp_home "$kiplay" > /dev/null 2>&1 &
    kp_pid=$!
    kp_open=$(wait_window --pid "$kp_pid" --name '^Kiplay')
}
kp_stop() {
    kill "$kp_pid" 2> /dev/null
    wait "$kp_pid" 2> /dev/null
}

# ---- Audacious ---------------------------------------------------------------

aud_start() { # tab pos|"" -> $aud_pid, $aud_open
    eval "$(dbus-launch --sh-syntax)"
    t0=$(now)
    XDG_CONFIG_HOME=$aud_home audacious > /dev/null 2>&1 &
    aud_pid=$!
    aud_open=$(wait_window --pid "$aud_pid" --class audacious)
    local w
    w=$(xdotool search --onlyvisible --pid "$aud_pid" | head -1)
    [ -n "$w" ] && xdotool windowmove "$w" 0 0 windowsize "$w" $W $H
    # audtool waits for the D-Bus interface to be up.
    for _ in $(seq 50); do audtool current-playlist > /dev/null 2>&1 && break; sleep 0.1; done
    audtool set-current-playlist "$1"
}
aud_stop() {
    audtool shutdown 2> /dev/null || kill "$aud_pid"
    wait "$aud_pid" 2> /dev/null
    kill "$DBUS_SESSION_BUS_PID" 2> /dev/null
}

# ---- the tests ----------------------------------------------------------------

printf 'label\tplayer\topen_ms\tmain_MB\tall_MB\tcpu_%%\n' > "$out"
while IFS=$'\t' read -r label tab pos loc; do
    [ -z "$label" ] || [ "${label:0:1}" = "#" ] && continue
    for run in $(seq "$runs"); do
        kp_start "$tab"
        if [ "$label" != idle ]; then
            XDG_CONFIG_HOME=$kp_home "$kiplay" "$loc" > /dev/null 2>&1
            sleep 1
            echo "kiplay $label: $(for p in $(tree "$kp_pid"); do tr '\0' ' ' < /proc/$p/cmdline | cut -c1-60; echo -n ' | '; done)" >&2
        fi
        sleep "$warm"
        r=$(measure "$kp_pid")
        printf '%s\tkiplay\t%s\t%s\n' "$label" "$kp_open" "$r" >> "$out"
        kp_stop

        for a in "${aud_homes[@]}"; do
            aud_home=${a%%:*}
            name=audacious
            [ "$a" != "$aud_home" ] && name=${a#*:}
            aud_start "$tab"
            if [ "$label" != idle ]; then
                audtool playlist-jump "$pos"
                audtool play-current-playlist
                sleep 1
                echo "$name $label: $(audtool current-song-filename)" >&2
            fi
            sleep "$warm"
            r=$(measure "$aud_pid")
            printf '%s\t%s\t%s\t%s\n' "$label" "$name" "$aud_open" "$r" >> "$out"
            aud_stop
        done
    done
done < "$scenarios"
