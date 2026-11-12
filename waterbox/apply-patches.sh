#!/bin/sh
# Applies patches/ to the extern/bsnes submodule's working tree. The pinned
# commit stays upstream's v115; every change is a numbered patch here, and
# this runs at every meson configure. A patch already applied is left alone.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
sub="$root/extern/bsnes"

for p in "$root"/patches/*.patch; do
	if git -C "$sub" apply --reverse --check "$p" 2>/dev/null; then
		continue
	fi
	git -C "$sub" apply "$p"
	echo "applied $(basename "$p")"
done
