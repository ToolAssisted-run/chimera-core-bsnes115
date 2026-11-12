#!/bin/bash
# The core-level equivalence gate: the sandboxed core must produce
# byte-identical video, audio, lag and memory-domain digests to the native
# reference build (the same cinterface.cpp compiled natively), and must
# survive a whole-machine savestate round-trip around every frame.
#
# The tests run two homebrew games that may be passed on (tests/roms), with
# the input of two movies recorded for chimera-core-snes9x: bsnes plays them
# differently, but the input is the same in both builds, which is all the
# comparison needs. Games that may not be passed on run from local files
# through tests/run-local.sh.
#
# Usage: ./run-gate.sh [-n <native build dir>] [-g <guest build dir>]
set -u

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
nat="$root/build/meson-native"
gst="$root/build/meson-guest"
while getopts "n:g:" opt; do
	case "$opt" in
		n) nat="$OPTARG" ;;
		g) gst="$OPTARG" ;;
		*) exit 2 ;;
	esac
done

[ -x "$nat/run-native" ] && [ -x "$nat/run-wbx" ] || {
	echo "native build missing: meson setup build/meson-native && ninja -C build/meson-native" >&2; exit 1; }
[ -f "$gst/core.wbx" ] || {
	echo "guest build missing: sh waterbox/setup-guest.sh && ninja -C build/meson-guest" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
digests() { grep -E '^(frames|vsync|videoHash|audioHash|lagFrames|domain\[)'; }
# What a turbo run can be held to: everything except the whole-run video hash,
# which a run that skipped the first half cannot possibly match - the second
# half it did draw is compared instead.
turboDigests() { grep -E '^(frames|vsync|tailVideoHash|audioHash|lagFrames|domain\[)'; }

ok=0
failed=0
report() { printf "%-30s %-6s %s\n" "$1" "$2" "$3"; case "$2" in PASS) ok=$((ok+1)) ;; *) failed=$((failed+1)) ;; esac; }
printf "%-30s %-6s %s\n" "Check" "Result" "Detail"
printf "%-30s %-6s %s\n" "-----" "------" "------"

# name rom ctl1 ctl2 movie(- = pad exercise)
tests=(
	"christmasCraze Christmas_Craze.smc joypad joypad christmasCraze.playaround.sol"
	"theLastSuper TheLastSuper.sfc joypad none theLastSuper.playaround.sol"
	"exercise Christmas_Craze.smc joypad none -"
	"accuratePPU TheLastSuper.sfc joypad none theLastSuper.playaround.sol"
)

for t in "${tests[@]}"; do
	read -r name rom ctl1 ctl2 movie <<< "$t"

	wd="$work/$name"
	mkdir -p "$wd"
	cp "$root/tests/roms/$rom" "$wd/"
	printf '{"game":["%s"]}' "$rom" > "$wd/slots"
	python3 - "$wd/settings" "$ctl1" "$ctl2" "$name" <<'PYSET'
import json, sys
names = {"joypad": "gamepad", "none": "none"}
settings = {"leftPort": names[sys.argv[2]], "rightPort": names[sys.argv[3]]}
# the cycle-based picture processor, bsnes's other one: a test of its own
if sys.argv[4] == "accuratePPU":
    settings["fastPPU"] = False
json.dump(settings, open(sys.argv[1], "w"))
PYSET

	args=(--ctl1 "$ctl1" --ctl2 "$ctl2")
	if [ "$movie" = "-" ]; then
		args+=(--frames 600 --exercise)
	else
		args+=(--sol "$root/tests/movies/$movie")
	fi

	if ! "$nat/run-native" "$wd" "${args[@]}" 2>"$work/err" | digests > "$work/nat.txt"; then
		report "$name:equivalence" FAIL "native runner error: $(head -1 "$work/err")"; continue
	fi
	if ! "$nat/run-wbx" "$gst/core.wbx" "$wd" "${args[@]}" 2>"$work/err" | digests > "$work/box.txt"; then
		report "$name:equivalence" FAIL "waterbox runner error: $(head -1 "$work/err")"; continue
	fi
	frames="$(sed -n 's/^frames=//p' "$work/box.txt")"
	cp "$work/box.txt" "$work/box.$name.txt"
	if cmp -s "$work/nat.txt" "$work/box.txt"; then
		report "$name:equivalence" PASS "$frames frames, native == waterboxed"
	else
		report "$name:equivalence" FAIL "$(diff "$work/nat.txt" "$work/box.txt" | tr '\n' ' ' | head -c 120)"
		continue
	fi

	# a hollow pass cannot sneak through: the input schedule must have shaped
	# the machine - an idle run of the same length must differ
	if [ "$movie" = "-" ]; then
		"$nat/run-wbx" "$gst/core.wbx" "$wd" --ctl1 "$ctl1" --ctl2 "$ctl2" \
			--frames "$frames" 2>/dev/null | digests > "$work/idle.txt"
		if cmp -s "$work/box.txt" "$work/idle.txt"; then
			report "$name:input-shaped" FAIL "the pad exercise changed nothing"
		else
			report "$name:input-shaped" PASS "input visibly shaped the machine"
		fi
	fi

	# Turbo: the core's drawing switched off for the first half of the run and
	# back on for the second. The machine, the sound, the lag count and every
	# picture of that second half must be what they would have been.
	# And the first half really must have gone undrawn. The comparison below is
	# blind to that on its own: it is held over tailVideoHash, the second half
	# only, so a SetRenderingEnabled that did nothing at all would leave every
	# compared digest identical and this leg would report "half of them undrawn"
	# about a run that drew every frame. That is not hypothetical - stubbing the
	# export to a no-op in the sibling stella core and rebuilding gave a fully
	# green gate. The whole-run videoHash is the witness, and the harness
	# already prints it: it covers the first half too, so the two runs cannot
	# agree on it unless the picture was never switched off.
	"$nat/run-wbx" "$gst/core.wbx" "$wd" "${args[@]}" 2>/dev/null > "$work/tnorm.raw"
	if "$nat/run-wbx" "$gst/core.wbx" "$wd" "${args[@]}" --turbo 2>/dev/null > "$work/tturbo.raw"; then
		turboDigests < "$work/tnorm.raw" > "$work/tnorm.txt"
		turboDigests < "$work/tturbo.raw" > "$work/turbo.txt"
		nvh="$(grep -m1 '^videoHash=' "$work/tnorm.raw")"
		tvh="$(grep -m1 '^videoHash=' "$work/tturbo.raw")"
		if ! cmp -s "$work/tnorm.txt" "$work/turbo.txt"; then
			report "$name:turbo" FAIL "$(diff "$work/tnorm.txt" "$work/turbo.txt" | tr '\n' ' ' | head -c 120)"
		elif [ -z "$nvh" ] || [ -z "$tvh" ]; then
			report "$name:turbo" FAIL "no whole-run videoHash to tell a skipped frame from a drawn one"
		elif [ "$nvh" = "$tvh" ]; then
			report "$name:turbo" FAIL "the turbo run drew every frame - nothing was skipped"
		else
			report "$name:turbo" PASS "$frames frames, half of them really undrawn, same machine and same pictures"
		fi
	else
		report "$name:turbo" FAIL "turbo runner error"
	fi

if ! "$nat/run-wbx" "$gst/core.wbx" "$wd" "${args[@]}" --rerecord 2>/dev/null | digests > "$work/rr.txt"; then
		report "$name:savestate" FAIL "rerecord runner error"; continue
	fi
	if cmp -s "$work/box.txt" "$work/rr.txt"; then
		report "$name:savestate" PASS "per-frame round-trip is lossless"
	else
		report "$name:savestate" FAIL "$(diff "$work/box.txt" "$work/rr.txt" | tr '\n' ' ' | head -c 120)"
	fi
done

# ---- save data: a save put in the "save" slot must be the cartridge's
# battery memory at boot, and an export must give it back. The Last Super has
# 2 KiB of it and writes none in the first frames, so a save with a pattern
# no blank memory has is what proves the way in and the way out. ----
wd="$work/savedata"
mkdir -p "$wd/out.nat" "$wd/out.box"
cp "$root/tests/roms/TheLastSuper.sfc" "$wd/"
python3 -c 'import sys; open(sys.argv[1], "wb").write(bytes((i * 7 + 3) & 255 for i in range(2048)))' "$wd/game.srm"
printf '{"game":["TheLastSuper.sfc"],"save":["game.srm"]}' > "$wd/slots"
"$nat/run-native" "$wd" --frames 60 --dump-domain CARTRAM "$work/sd.nat.ram" --savedata-out "$wd/out.nat" >/dev/null 2>&1
"$nat/run-wbx" "$gst/core.wbx" "$wd" --frames 60 --dump-domain CARTRAM "$work/sd.box.ram" --savedata-out "$wd/out.box" >/dev/null 2>&1
if ! cmp -s "$work/sd.box.ram" "$wd/game.srm"; then
	report "savedata:import" FAIL "the cartridge's memory is not the save the project gave"
elif ! cmp -s "$work/sd.nat.ram" "$work/sd.box.ram"; then
	report "savedata:import" FAIL "native and sandbox loaded different memory"
else
	report "savedata:import" PASS "the save slot's 2048 bytes are the cartridge's memory at boot"
fi
if ! cmp -s "$wd/out.box/save.ram" "$wd/game.srm"; then
	report "savedata:export" FAIL "the export is not the cartridge's memory"
elif ! diff -r "$wd/out.nat" "$wd/out.box" >/dev/null 2>&1; then
	report "savedata:export" FAIL "native and sandbox exported different files"
else
	report "savedata:export" PASS "save.ram comes out as it went in, native == sandbox"
fi

# ---- settings leg: leftPort=none must reach the guest - the movie's 1425
# P1 input frames become no-ops on an unplugged port, so the run diverges
# from the recorded one (and still matches ITS native reference) ----
wd="$work/portset"
mkdir -p "$wd"
cp "$root/tests/roms/Christmas_Craze.smc" "$wd/"
printf '{"game":["Christmas_Craze.smc"]}' > "$wd/slots"
printf '{"leftPort":"none","rightPort":"none"}' > "$wd/settings"
solpath="$root/tests/movies/christmasCraze.playaround.sol"
"$nat/run-native" "$wd" --sol "$solpath" --ctl2 joypad 2>/dev/null | digests > "$work/pn.txt"
"$nat/run-wbx" "$gst/core.wbx" "$wd" --sol "$solpath" --ctl2 joypad 2>/dev/null | digests > "$work/pb.txt"
if ! cmp -s "$work/pn.txt" "$work/pb.txt"; then
	report "settings:leftPort" FAIL "$(diff "$work/pn.txt" "$work/pb.txt" | tr '\n' ' ' | head -c 120)"
elif cmp -s "$work/pb.txt" "$work/box.christmasCraze.txt" 2>/dev/null; then
	report "settings:leftPort" FAIL "unplugging the pads changed nothing"
else
	report "settings:leftPort" PASS "leftPort=none reached the guest and unplugged the pad"
fi

# ---- device legs: the multitap and the right-port analog devices. Their
# SEMANTICS need games that read them (the local movie set's job); these
# legs prove the plumbing - the same schedule lands on the same machine in
# both flavors, and survives per-frame savestate round-trips (the mouse's
# accumulated position is machine state).
device_leg() {
	# tag, the settings under test, and the settings the result must NOT match.
	local tag="$1" settings="$2" unlike="$3" unlikeName="$4"
	shift 4
	local wd="$work/dev.$tag"
	mkdir -p "$wd"
	cp "$root/tests/roms/Christmas_Craze.smc" "$wd/"
	printf '{"game":["Christmas_Craze.smc"]}' > "$wd/slots"
	printf '%s' "$settings" > "$wd/settings"
	if ! "$nat/run-native" "$wd" "$@" 2>"$work/err" | digests > "$work/dn.txt"; then
		report "$tag:equivalence" FAIL "native runner error: $(head -1 "$work/err")"; return
	fi
	if ! "$nat/run-wbx" "$gst/core.wbx" "$wd" "$@" 2>"$work/err" | digests > "$work/db.txt"; then
		report "$tag:equivalence" FAIL "waterbox runner error: $(head -1 "$work/err")"; return
	fi
	if ! cmp -s "$work/dn.txt" "$work/db.txt"; then
		report "$tag:equivalence" FAIL "$(diff "$work/dn.txt" "$work/db.txt" | tr '\n' ' ' | head -c 120)"; return
	fi
	# Both sides of that comparison were built from the same settings, so it
	# says the two flavors agree and nothing at all about WHAT they agreed on.
	# The parser answers a device name it does not know with a gamepad and a
	# line on stderr (bsnes.cpp, "unknown %s '%s', using a gamepad") that
	# nothing here reads - so a device missing from it would leave both builds
	# making a gamepad and agreeing perfectly (the same leg in
	# chimera-core-snes9x went green that way). So each leg also names a
	# machine its own must not be: a plain pad for the analog devices, which
	# is what the broken parser falls back to, and an empty port for the
	# multitap. The multitap's test drives player 2, the multitap's first pad:
	# Christmas_Craze reads only the first pad of each port, so driving
	# player 3 left a multitap and an empty port byte-identical (measured),
	# and for the same reason it cannot tell a multitap from a single pad.
	printf '%s' "$unlike" > "$wd/settings"
	"$nat/run-wbx" "$gst/core.wbx" "$wd" "$@" 2>/dev/null | digests > "$work/dp.txt"
	printf '%s' "$settings" > "$wd/settings"
	if cmp -s "$work/db.txt" "$work/dp.txt"; then
		report "$tag:equivalence" FAIL "the device built the machine $unlikeName builds - did the name reach the guest at all?"; return
	fi
	report "$tag:equivalence" PASS "native == waterboxed, and not the machine $unlikeName makes"
	if "$nat/run-wbx" "$gst/core.wbx" "$wd" "$@" --rerecord 2>/dev/null | digests > "$work/dr.txt" \
		&& cmp -s "$work/db.txt" "$work/dr.txt"; then
		report "$tag:savestate" PASS "per-frame round-trip is lossless"
	else
		report "$tag:savestate" FAIL "rerecord differs"
	fi
}
device_leg "multitap" '{"leftPort":"gamepad","rightPort":"multitap"}' \
	'{"leftPort":"gamepad","rightPort":"none"}' "an empty port" --frames 300 --exercise --exercise-pad 2
device_leg "mouse" '{"rightPort":"mouse"}' \
	'{"leftPort":"gamepad","rightPort":"gamepad"}' "a plain pad" --frames 300 --exercise --wiggle-axes
device_leg "leftMouse" '{"leftPort":"mouse","rightPort":"none"}' \
	'{"leftPort":"gamepad","rightPort":"none"}' "a plain pad" --frames 300 --exercise --wiggle-axes
device_leg "superScope" '{"rightPort":"superScope"}' \
	'{"leftPort":"gamepad","rightPort":"gamepad"}' "a plain pad" --frames 300 --exercise --wiggle-axes
device_leg "justifier" '{"rightPort":"justifier"}' \
	'{"leftPort":"gamepad","rightPort":"gamepad"}' "a plain pad" --frames 300 --exercise --wiggle-axes
device_leg "justifiers" '{"rightPort":"justifiers"}' \
	'{"rightPort":"justifier"}' "one Justifier" --frames 300 --exercise --wiggle-axes

# ---- MSU1: the expansion pack must be FOUND by both flavors (a game that
# never reads it leaves no trace in the digests, so the witness is the
# core's own boot report), and the machine must still run identically.
wd="$work/msu1"
mkdir -p "$wd"
cp "$root/tests/roms/Christmas_Craze.smc" "$wd/"
python3 "$here/tests/gen-msu1.py" "$wd" Christmas_Craze >/dev/null
printf '{"game":["Christmas_Craze.smc"],"msu1":["Christmas_Craze.msu","Christmas_Craze-1.pcm"]}' > "$wd/slots"
"$nat/run-native" "$wd" --frames 300 --exercise 2>"$work/mn.err" | digests > "$work/mn.txt"
"$nat/run-wbx" "$gst/core.wbx" "$wd" --frames 300 --exercise 2>"$work/mb.err" | digests > "$work/mb.txt"
if ! grep -q 'MSU1 present' "$work/mn.err"; then
	report "msu1:detected" FAIL "the native run did not find the pack"
elif ! grep -q 'MSU1 present' "$work/mb.err"; then
	report "msu1:detected" FAIL "the sandboxed run did not find the pack"
elif ! cmp -s "$work/mn.txt" "$work/mb.txt"; then
	report "msu1:detected" FAIL "$(diff "$work/mn.txt" "$work/mb.txt" | tr '\n' ' ' | head -c 120)"
else
	report "msu1:detected" PASS "both flavors mounted the pack and ran identically"
fi
rm -f "$wd/Christmas_Craze.msu" "$wd/Christmas_Craze-1.pcm"
printf '{"game":["Christmas_Craze.smc"]}' > "$wd/slots"
"$nat/run-wbx" "$gst/core.wbx" "$wd" --frames 5 2>"$work/mo.err" >/dev/null
if grep -q 'MSU1 absent' "$work/mo.err"; then
	report "msu1:absent" PASS "without a pack the machine reports MSU1 absent"
else
	report "msu1:absent" FAIL "the pack-less machine still claims MSU1"
fi

# ---- coprocessor firmware: a cartridge whose header says it carries a
# DSP-1 (The Last Super with that one byte changed) runs on bsnes's own DSP-1
# when no firmware is given, and on the firmware when it is. The game never
# talks to the chip, so the digests cannot tell the two apart: the witness is
# the boot line, which says which one the cartridge got. The firmware is
# 8 KiB of zeros, nobody's work. ----
wd="$work/dsp"
mkdir -p "$wd"
python3 - "$root/tests/roms/TheLastSuper.sfc" "$wd/dsp.sfc" <<'PYDSP'
import sys
d = bytearray(open(sys.argv[1], "rb").read())
d[0x7fd6] = 0x03  # the header's cartridge type: ROM and a DSP
open(sys.argv[2], "wb").write(d)
PYDSP
printf '{"game":["dsp.sfc"]}' > "$wd/slots"
"$nat/run-wbx" "$gst/core.wbx" "$wd" --frames 60 --exercise 2>"$work/dsp.hle.err" >/dev/null
head -c 8192 /dev/zero > "$wd/dsp1b.rom"
"$nat/run-native" "$wd" --frames 300 --exercise 2>"$work/dsp.nat.err" | digests > "$work/dsp.nat.txt"
"$nat/run-wbx" "$gst/core.wbx" "$wd" --frames 300 --exercise 2>"$work/dsp.box.err" | digests > "$work/dsp.box.txt"
if ! grep -q "a coprocessor of bsnes's own" "$work/dsp.hle.err"; then
	report "coprocessor:firmware" FAIL "with no firmware the DSP-1 was not bsnes's own: $(grep -m1 bsnes115 "$work/dsp.hle.err")"
elif ! grep -q "a coprocessor from its firmware" "$work/dsp.box.err" || ! grep -q "a coprocessor from its firmware" "$work/dsp.nat.err"; then
	report "coprocessor:firmware" FAIL "dsp1b.rom did not reach the DSP-1: $(grep -m1 bsnes115 "$work/dsp.box.err")"
elif ! cmp -s "$work/dsp.nat.txt" "$work/dsp.box.txt"; then
	report "coprocessor:firmware" FAIL "$(diff "$work/dsp.nat.txt" "$work/dsp.box.txt" | tr '\n' ' ' | head -c 120)"
else
	report "coprocessor:firmware" PASS "no firmware: bsnes's own DSP-1; dsp1b.rom given: the chip runs it, native == sandbox"
fi

# ------------------------------------------------- Chimera's engine
# Each read of the controls is reported to Chimera as it happens, so a Lua
# script's event.oninputpoll runs there (chimera#344): every frame the game
# read them has at least one, and counting them moves nothing. Needs
# CHIMERA_ROOT, a Chimera checkout with this core's package installed in it.
runner="${CHIMERA_ROOT:-}/build/dll/chimera-run"
package="${CHIMERA_ROOT:-}/build/Cores/bsnes115.chimeraCore"
if [ -n "${CHIMERA_ROOT:-}" ] && [ -x "$runner" ] && [ -f "$package" ]; then
	# a game runs only in a project (Chimera, 2026-10-26): one of the cartridge
	# with no input, to record 300 idle frames in, then one of those frames
	cart="$root/tests/roms/Christmas_Craze.smc"
	"$runner" --new-project "$work/none.chimeraProject" "$package" "$cart" > "$work/none.make.log" 2>&1
	"$runner" --project "$work/none.chimeraProject" "$package" --files "$(dirname "$cart")" \
		--frames 300 --record "$work/idle.txt" > "$work/idle.log" 2>&1
	"$runner" --new-project "$work/idle.chimeraProject" "$package" "$cart" "$work/idle.txt" > "$work/idle.make.log" 2>&1
	"$runner" --project "$work/idle.chimeraProject" "$package" --files "$(dirname "$cart")" \
		--dump "WRAM=$work/plain.ram" > "$work/plain.log" 2>&1
	"$runner" --project "$work/idle.chimeraProject" "$package" --files "$(dirname "$cart")" --count-input-polls \
		--dump "WRAM=$work/polls.ram" --meta "$work/polls.meta" > "$work/polls.log" 2>&1
	polls=$(sed -n 's/^inputPolls=//p' "$work/polls.meta" 2> /dev/null)
	lags=$(sed -n 's/^lagFrames=//p' "$work/polls.meta" 2> /dev/null)
	frames=$(sed -n 's/^frames=//p' "$work/polls.meta" 2> /dev/null)
	if [ "${polls:-none}" != none ] && [ "${frames:-0}" = 300 ] && [ "${lags:-300}" -lt 300 ] \
		&& [ "${polls:-0}" -ge $(( 300 - ${lags:-300} )) ] && [ -s "$work/plain.ram" ] && cmp -s "$work/plain.ram" "$work/polls.ram"; then
		report "engine:input-polls" PASS "the game's reads of its pads reach Chimera as they happen: $polls reads in 300 frames, $lags of them lag frames; WRAM is the run's without counting"
	else
		report "engine:input-polls" FAIL "inputPolls=${polls:-missing} frames=${frames:-?} lagFrames=${lags:-?}, or counting moved WRAM; the runner said: $(tail -1 "$work/polls.log" 2> /dev/null)"
	fi
	# the firmware a cartridge needs, asked before it boots (gameFirmware)
	"$runner" --new-project "$work/dsp.chimeraProject" "$package" "$work/dsp/dsp.sfc" > "$work/dsp.make.log" 2>&1
	"$runner" --project "$work/dsp.chimeraProject" "$package" --files "$work/dsp" --suggest > "$work/dsp.suggest" 2>&1
	if grep -q '"id":"dsp1b.rom"' "$work/dsp.suggest" && grep -q '"requiredWhen":{"not":{"setting":"coprocessorPreferHLE","is":true}}' "$work/dsp.suggest"; then
		report "engine:game-firmware" PASS "a DSP-1 cartridge asks for dsp1b.rom unless HLE is preferred"
	else
		report "engine:game-firmware" FAIL "the suggestion was: $(tail -1 "$work/dsp.suggest")"
	fi
else
	printf "%-30s %-6s %s\n" "engine:game-firmware" SKIP "needs CHIMERA_ROOT with build/dll/chimera-run and build/Cores/bsnes115.chimeraCore"
	printf "%-30s %-6s %s\n" "engine:input-polls" SKIP "needs CHIMERA_ROOT with build/dll/chimera-run and build/Cores/bsnes115.chimeraCore"
fi

echo ""
echo "$ok ok, $failed failed"
[ "$failed" -gt 0 ] && exit 1
exit 0
