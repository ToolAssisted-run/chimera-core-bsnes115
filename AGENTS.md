# AGENTS.md - bsnes v115 core for Chimera

This repository builds upstream bsnes v115 as a sandboxed guest for Chimera,
a frontend for tool-assisted speedruns. It produces one file,
`bsnes115.chimeraCore`. `docs/BUILDING.md` has every step explained;
`docs/PLAN.md` has what was decided and why.

## Layout

- `extern/bsnes/` - upstream at the `v115` tag, a submodule, never edited in
  place.
- `patches/` - every change to upstream, numbered, applied by
  `waterbox/apply-patches.sh` at every meson configure.
- `meson.build` - one build description, two builds: native (`run-native`,
  `run-wbx`) and guest (`core.wbx`).
- `waterbox/bsnes.cpp` - the adapter: what upstream's own frontends do
  around the emulator (loading, firmware, controllers, picture, sound,
  memory views, save data), behind Chimera's guest exports.
- `waterbox/libco.c` - the coroutine library, in place of upstream's.
- `waterbox/embed.py` - compiles upstream's game database in.
- `waterbox/run-native.c`, `waterbox/run-wbx.c`, `waterbox/gate-harness.h`
  - the two test drivers and the loop they share.
- `waterbox/waterbox.config`, `file_slots.json`, `default_keybinds.json`,
  `package-licenses.json`, `languages/` - what the package declares.
- `waterbox/run-gate.sh` - the core gate. `waterbox/tests/run-frontend.sh` -
  the frontend gate. `waterbox/tests/run-local.sh` - games that may not be
  passed on, from `tests/roms-local/`.
- `tests/roms/`, `tests/movies/` - two homebrew games and input for them.
- `.github/workflows/chimera.yml` - CI and publishing.

## Commands, in order

    CHIMERA=/path/to/chimera; MB="$CHIMERA/extern/chimera-common-minibox"
    git submodule update --init
    meson setup "$MB/build/meson-cpp" "$MB" -Dguest_cpp=true && meson compile -C "$MB/build/meson-cpp"
    meson setup build/meson-native -Dminibox_dir="$MB" && ninja -C build/meson-native
    ./waterbox/build-package.sh -m "$MB" -r "$CHIMERA"
    CHIMERA_ROOT="$CHIMERA" ./waterbox/run-gate.sh
    ./waterbox/tests/run-frontend.sh --chimera-root "$CHIMERA"
    "$CHIMERA/build/meson-linux/source/ui/test-packages" "$CHIMERA/build/Cores"
    ./waterbox/tests/run-local.sh

## Rules of this repository

- **v115 stays v115.** The point of the core is to be that emulator. A
  change to how the machine behaves is a patch, says why, and is put to the
  owner first.
- **No firmware and no commercial game is committed**, uploaded to CI or
  attached to an issue. bsnes v115 compiles Nintendo's Super Game Boy boot
  ROMs in; patch 0003 keeps them out of the package, and it must stay so.
- **Both builds compile the same adapter.** Anything that differs between
  native and sandbox is a bug the core gate should catch; a new test is
  broken on purpose once and seen to fail.
- **Every text a person reads is a key** of `waterbox/languages/*.json`, in
  every language Chimera has.
- Commits: `type(scope): what changed`, a short body with what was tested,
  the trailers the Chimera project uses. Stage paths by name.
