#!/bin/sh
# Builds a .deb from the current tree, on the machine it runs on: the
# dependencies are computed by dpkg-shlibdeps against the libraries
# installed here, so build on the oldest target distro of each family.
# Usage: tools/build-deb.sh [OUTDIR]   (needs build-essential, dpkg-dev,
# gettext and the -dev packages of GTK 2, GLib, ALSA and libav*).
set -eu

cd "$(dirname "$0")/.."
root=$(pwd)
out=${1:-$root/dist}
version=$(sed -n 's/^VERSION *= *//p' config.mk)
arch=$(dpkg --print-architecture)
maint=${DEB_MAINTAINER:-Kiyoshi Spreclerg <kiyoshi_pip@protonmail.com>}

mkdir -p "$out"
out=$(cd "$out" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
pkg=$work/pkg

make clean
make -j"$(nproc)" PREFIX=/usr
make PREFIX=/usr DESTDIR="$pkg" install
strip --strip-unneeded "$pkg"/usr/bin/kiplay*
find "$pkg" -type d -exec chmod 755 {} +

# dpkg-shlibdeps insists on a debian/control in the current directory.
mkdir -p "$work/debian"
printf 'Source: kiplay\n\nPackage: kiplay\nArchitecture: any\n' > "$work/debian/control"
(
	cd "$work"
	set --
	for b in "$pkg"/usr/bin/kiplay*; do set -- "$@" "-e$b"; done
	dpkg-shlibdeps -O "$@" > "$work/substvars"
)
depends=$(sed -n 's/^shlibs:Depends=//p' "$work/substvars")

mkdir -p "$pkg/DEBIAN"
size=$(du -sk "$pkg" | cut -f1)
cat > "$pkg/DEBIAN/control" <<CTL
Package: kiplay
Version: $version-1
Architecture: $arch
Maintainer: $maint
Installed-Size: $size
Depends: $depends
Recommends: mpv, curl
Section: video
Priority: optional
Homepage: https://github.com/${GITHUB_REPOSITORY:-kiplay/kiplay}
Description: lightweight audio and video player
 Playlists in tabs like Audacious, SMPlayer-style settings, video through
 mpv or MPlayer and music through its own gapless engine. Shows styled
 YouTube captions and recorded live chat.
CTL

deb=$out/kiplay_${version}-1_${arch}.deb
dpkg-deb --root-owner-group --build "$pkg" "$deb"
echo "$deb"
