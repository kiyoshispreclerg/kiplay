#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Copies Audacious's playlists into a Kiplay config folder, for the
comparison tests (docs/BENCH.md). Not a Kiplay feature: Kiplay doesn't
import Audacious playlists. Audacious's files are only read.

    tools/audpl2m3u8.py [-a ~/.config/audacious] [--no-tags] OUT

writes OUT/kiplay/playlists/<n>.m3u8 (1, 2, ... in Audacious's tab order)
and OUT/kiplay/kiplay.ini with that order. Run Kiplay with
XDG_CONFIG_HOME=OUT. --no-tags leaves titles and lengths out, so Kiplay has
to read them itself.
"""
import argparse
import os
import sys
from urllib.parse import unquote


def read_audpl(path):
    """(name, [entry dicts]) from an .audpl: "key=value" lines, values
    URL-escaped, every entry starting at uri=."""
    name, entries, cur = None, [], None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            key, sep, value = line.rstrip("\n").partition("=")
            if not sep:
                continue
            if key == "uri":
                cur = {"uri": value}
                entries.append(cur)
            elif cur is None:
                if key == "title":
                    name = unquote(value)
            else:
                cur[key] = unquote(value)
    return name, entries


def location(uri):
    """Plain path for local files; the URI when it has a "?N" subtune
    (Kiplay keeps those as file:// URIs) or isn't a file."""
    if uri.startswith("file://"):
        path, q, sub = uri[7:].rpartition("?")
        if q and sub.isdigit():
            return uri
        return unquote(uri[7:])
    return uri


def clean(s):
    return " ".join(s.split())


def m3u8(name, entries, tags):
    out = ["#EXTM3U", "#PLAYLIST:" + clean(name)]
    for e in entries:
        if tags:
            title = clean(e.get("title", ""))
            artist = clean(e.get("artist", ""))
            album = clean(e.get("album", ""))
            length = int(e["length"]) // 1000 if e.get("length", "").isdigit() else -1
            shown = f"{artist} - {title}" if artist and title else title
            if shown or length >= 0:
                out.append(f"#EXTINF:{length},{shown}")
            if artist:
                out.append("#EXTART:" + artist)
            if album:
                out.append("#EXTALB:" + album)
        out.append(location(e["uri"]))
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-a", "--audacious", default=os.path.expanduser("~/.config/audacious"))
    ap.add_argument("--no-tags", action="store_true")
    ap.add_argument("out")
    args = ap.parse_args()

    src = os.path.join(args.audacious, "playlists")
    order_file = os.path.join(src, "order")
    if os.path.exists(order_file):
        with open(order_file) as f:
            order = f.read().split()
    else:
        order = sorted(f[:-6] for f in os.listdir(src) if f.endswith(".audpl"))

    dst = os.path.join(args.out, "kiplay", "playlists")
    os.makedirs(dst, exist_ok=True)
    total = 0
    for n, stem in enumerate(order, 1):
        name, entries = read_audpl(os.path.join(src, stem + ".audpl"))
        with open(os.path.join(dst, f"{n}.m3u8"), "w", encoding="utf-8") as f:
            f.write(m3u8(name or stem, entries, not args.no_tags))
        total += len(entries)
        print(f"{n:3} {len(entries):6}  {name}")

    ini = os.path.join(args.out, "kiplay", "kiplay.ini")
    lines = []
    if os.path.exists(ini):
        with open(ini) as f:
            lines = [l for l in f.read().splitlines() if not l.startswith("order")]
    if "[playlists]" not in lines:
        lines += ["[playlists]"]
    i = lines.index("[playlists]")
    lines.insert(i + 1, "order = " + ",".join(str(n) for n in range(1, len(order) + 1)))
    with open(ini, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{len(order)} playlists, {total} entries -> {dst}", file=sys.stderr)


if __name__ == "__main__":
    main()
