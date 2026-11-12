#!/bin/bash
# Runs games that may not be passed on, from tests/roms-local/ (which git
# ignores): each must come out the same in the native build, in the sandbox,
# and in the sandbox with a save and load of the whole machine around every
# frame. A game whose files are absent is skipped, and said so.
#
# Copy the files in under the names below; for a Game Boy game, the Super
# Game Boy's cartridge and boot ROM as sgb1.sfc and sgb1.boot.rom (or sgb2.*).
#
# Usage: ./run-local.sh [<frames for the games with no movie, default 3000>]
set -u

here="$(cd "$(dirname "$0")" && pwd)"
wb="$(cd "$here/.." && pwd)"
root="$(cd "$wb/.." && pwd)"
nat="$root/build/meson-native"
gst="$root/build/meson-guest"
frames="${1:-3000}"

[ -x "$nat/run-native" ] && [ -x "$nat/run-wbx" ] && [ -f "$gst/core.wbx" ] || {
	echo "build both first (docs/BUILDING.md)" >&2; exit 1; }

romdir="$root/tests/roms-local"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
digests() { grep -E '^(frames|vsync|videoHash|audioHash|lagFrames|domain\[)'; }

ok=0
failed=0
skipped=0
report() {
	printf "%-28s %-6s %s\n" "$1" "$2" "$3"
	case "$2" in PASS) ok=$((ok+1)) ;; SKIP) skipped=$((skipped+1)) ;; *) failed=$((failed+1)) ;; esac
}
printf "%-28s %-6s %s\n" "Check" "Result" "Detail"
printf "%-28s %-6s %s\n" "-----" "------" "------"

# name | game | other files | settings | movie (tests/movies) or - for the
# pad exercise. The movies were recorded on chimera-core-snes9x: bsnes plays
# them differently, but they are the same input in every build.
games=(
	"smw|Super Mario World (U) [!].smc||{}|-"
	"breathOfFire|Breath of Fire (U) [!].smc||{}|-"
	"marioCollection|Super Mario Collection (V1.0) (J).smc||{}|-"
	"princeOfPersia|Prince of Persia (J).smc||{\"rightPort\":\"none\"}|princeOfPersia.anyPercent.sol"
	"arkanoid|Arkanoid - Doh It Again (J) [!].smc||{}|arkanoid.anyPercent.sol"
	"smwAccuratePPU|Super Mario World (U) [!].smc||{\"fastPPU\":false,\"fastDSP\":false}|-"
	"sgbKirby|Kirby's Hoshinoka-Bi (J) (V1.1).gb|sgb1.sfc sgb1.boot.rom|{}|-"
	"sgb2Kirby|Kirby's Hoshinoka-Bi (J) (V1.1).gb|sgb2.sfc sgb2.boot.rom|{\"superGameBoy\":\"SGB2\"}|-"
)

for g in "${games[@]}"; do
	IFS='|' read -r name game others settings movie <<< "$g"
	missing=""
	for f in "$game" $others; do
		[ -f "$romdir/$f" ] || missing="$f"
	done
	[ -z "$missing" ] || { report "$name" SKIP "copy '$missing' into tests/roms-local/"; continue; }

	wd="$work/$name"
	mkdir -p "$wd"
	cp "$romdir/$game" "$wd/"
	for f in $others; do cp "$romdir/$f" "$wd/"; done
	python3 -c 'import json,sys; json.dump({"game":[sys.argv[2]]}, open(sys.argv[1],"w"))' "$wd/slots" "$game"
	printf '%s' "$settings" > "$wd/settings"

	if [ "$movie" = "-" ]; then
		args=(--frames "$frames" --exercise)
	else
		ctl2=joypad
		case "$settings" in *'"rightPort":"none"'*) ctl2=none ;; esac
		args=(--sol "$root/tests/movies/$movie" --ctl2 "$ctl2")
	fi
	if ! "$nat/run-native" "$wd" "${args[@]}" 2>"$work/err.nat" | digests > "$work/nat.txt"; then
		report "$name" FAIL "native runner error: $(grep -v '^\[' "$work/err.nat" | head -1)"; continue
	fi
	if ! "$nat/run-wbx" "$gst/core.wbx" "$wd" "${args[@]}" 2>"$work/err.box" | digests > "$work/box.txt"; then
		report "$name" FAIL "sandbox runner error: $(grep -v '^\[' "$work/err.box" | head -1)"; continue
	fi
	n="$(sed -n 's/^frames=//p' "$work/box.txt")"
	boot="$(grep -m1 '^\[bsnes115\]' "$work/err.box" | sed 's/^\[bsnes115\] //')"
	if ! cmp -s "$work/nat.txt" "$work/box.txt"; then
		report "$name" FAIL "$(diff "$work/nat.txt" "$work/box.txt" | tr '\n' ' ' | head -c 110)"; continue
	fi
	if ! "$nat/run-wbx" "$gst/core.wbx" "$wd" "${args[@]}" --rerecord 2>/dev/null | digests > "$work/rr.txt"; then
		report "$name" FAIL "rerecord runner error"
	elif cmp -s "$work/box.txt" "$work/rr.txt"; then
		report "$name" PASS "$n frames, native == sandbox == rerecord; $boot"
	else
		report "$name" FAIL "rerecord differs"
	fi
done

echo ""
echo "$ok ok, $failed failed, $skipped skipped"
[ "$failed" -gt 0 ] && exit 1
exit 0
