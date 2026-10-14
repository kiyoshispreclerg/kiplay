#!/bin/sh
# Checks an installed kiplay: the version matches, every library the
# binaries need is there, the engine plays a file of silence on the ALSA
# null device, and the window opens on a virtual X screen and exits.
# Usage: tools/smoke-test.sh      (from the repository root; needs xvfb-run)
# Touches nothing of the user's: HOME lives in a temporary folder.
set -eu

[ -f config.mk ] || { echo "run it from the repository root" >&2; exit 2; }
version=$(sed -n 's/^VERSION *= *//p' config.mk)

work=$(mktemp -d /tmp/kp-smoke.XXXXXX)
trap 'rm -rf "$work"' EXIT
export HOME=$work
unset XDG_DATA_HOME XDG_CONFIG_HOME XDG_RUNTIME_DIR

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

echo "== version"
got=$(kiplay --version 2>&1) || fail "kiplay --version: $got"
[ "$got" = "kiplay $version" ] || fail "kiplay --version: \"$got\", expected \"kiplay $version\""

echo "== libraries"
for b in kiplay kiplay-audio; do
	p=$(command -v "$b") || fail "$b is not installed"
	if ldd "$p" | grep "not found"; then
		fail "$b needs libraries the package does not depend on"
	fi
done

echo "== engine"
printf 'tick 1\n' | timeout 10 kiplay-audio -d null >/dev/null || fail "kiplay-audio did not start"

echo "== window"
xvfb-run -a -s "-screen 0 1280x720x24" timeout 30 env NO_AT_BRIDGE=1 kiplay --exit-after-map \
	|| fail "kiplay --exit-after-map exited with an error"
echo "OK"
