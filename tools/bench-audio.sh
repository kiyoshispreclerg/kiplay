#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Music engine against mpv, on the same files: own memory (Private_Dirty)
# after a few seconds and CPU over a stretch of playback.
#
#   tools/bench-audio.sh [-d null|default] [-s SECONDS] FILE...
#
# -d default plays through the real sound device (PipeWire/PulseAudio), at
# volume 0: nothing is heard, but the sound server's costs count, as in real
# use. -d null (the default) uses ALSA's null sink and mpv's null output.
# Files may be "game.nsf?3" (song 3); mpv then plays song 1.
set -eu

dev=null
secs=20
while getopts d:s: o; do
    case $o in
    d) dev=$OPTARG ;;
    s) secs=$OPTARG ;;
    *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -gt 0 ] || { echo "usage: $0 [-d null|default] [-s SECONDS] FILE..." >&2; exit 2; }

here=$(cd "$(dirname "$0")/.." && pwd)
engine=$here/build/kiplay-audio
[ -x "$engine" ] || { echo "build it first: make" >&2; exit 1; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
hz=$(getconf CLK_TCK)

# pid -> "MEM_kB CPU%" (memory at 5 s, CPU from 5 s to 5+secs s); "- -"
# if it went away (a short file played out: use a longer one).
measure() {
    sleep 5
    mem=$(awk '/^Private_Dirty/ {print $2}' "/proc/$1/smaps_rollup" 2>/dev/null) || mem=
    t1=$(awk '{print $14 + $15}' "/proc/$1/stat" 2>/dev/null) || t1=
    sleep "$secs"
    t2=$(awk '{print $14 + $15}' "/proc/$1/stat" 2>/dev/null) || t2=
    if [ -z "$mem" ] || [ -z "$t1" ] || [ -z "$t2" ]; then
        echo "- -"
    else
        echo "$mem $(echo "scale=2; ($t2 - $t1) * 100 / $hz / $secs" | bc)"
    fi
}

if [ "$dev" = null ]; then mpv_ao=null; else mpv_ao=pulse,alsa; fi

printf '%-40s %22s %22s\n' "file" "kiplay-audio kB / CPU%" "mpv kB / CPU%"
for f in "$@"; do
    # kiplay-audio, fed through a fifo so it keeps running
    mkfifo "$tmp/in"
    "$engine" -d "$dev" <"$tmp/in" >/dev/null &
    epid=$!
    exec 3>"$tmp/in"
    printf 'volume 0\nload 0 %s\n' "$f" >&3
    e=$(measure $epid)
    echo quit >&3
    exec 3>&-
    wait $epid 2>/dev/null || true
    rm -f "$tmp/in"

    # mpv as Kiplay runs it, minus the window
    plain=${f%\?[0-9]*}
    mpv --no-config --no-terminal --no-video --idle=yes --keep-open=yes --volume=0 --ao=$mpv_ao \
        --input-ipc-server="$tmp/mpv.sock" "$plain" >/dev/null 2>&1 &
    mpid=$!
    m=$(measure $mpid)
    kill $mpid 2>/dev/null || true
    wait $mpid 2>/dev/null || true

    name=$(basename "$f" | cut -c1-40)
    printf '%-40s %13s / %5s%% %13s / %5s%%\n' "$name" ${e% *} ${e#* } ${m% *} ${m#* }
done
