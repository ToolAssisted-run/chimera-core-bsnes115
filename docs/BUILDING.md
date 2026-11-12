# Building chimera-core-bsnes115

From a fresh clone to a package Chimera opens, and the tests that prove it.
`.github/workflows/chimera.yml` runs the same steps and is the authority if
the two ever disagree.

## What you need

- Linux on x86_64 or aarch64. A package is built on the CPU it is for; it is
  never cross-built.
- `meson`, `ninja`, `gcc`/`g++` and `python3`:

      sudo apt-get install -y --no-install-recommends meson ninja-build build-essential python3

- A Chimera checkout, for miniBox (the sandbox and its guest toolchain, a
  submodule of Chimera), for Chimera's own programs that the frontend tests
  run, and as the place the package is installed to.

## The sources

    git clone --recursive https://github.com/ToolAssisted-run/chimera-core-bsnes115.git
    cd chimera-core-bsnes115

`extern/bsnes` is upstream bsnes at the `v115` tag, unchanged. Every change
to it is a numbered file in `patches/`, which `waterbox/apply-patches.sh`
applies to the submodule's working tree at every meson configure (a patch
already applied is left alone):

- `0001` lets the memory viewer read the cycle-based picture processor's
  video memory.
- `0002` starts bsnes's power-on randomness from a fixed seed instead of the
  host's processor clock, so a movie plays the same on every machine.
- `0003` takes the Super Game Boy's boot ROM from the person's firmware.
  bsnes v115 compiles Nintendo's boot ROMs in; this package does not carry
  them.

## miniBox

    CHIMERA=/path/to/chimera
    git -C "$CHIMERA" submodule update --init extern/chimera-common-minibox
    MB="$CHIMERA/extern/chimera-common-minibox"
    meson setup "$MB/build/meson-cpp" "$MB" -Dguest_cpp=true
    meson compile -C "$MB/build/meson-cpp"

bsnes is C++, so the guest needs miniBox's C++ toolchain (`-Dguest_cpp=true`),
which builds libstdc++ for the sandbox. The first build downloads the GCC
source that matches the installed gcc, once.

## The two builds

The same sources are built twice: natively, as a reference, and for the
sandbox. Both compile `waterbox/bsnes.cpp` (the adapter) and
`waterbox/libco.c` (the coroutine library) unchanged; only `emulibc.h`
differs (`waterbox/native-shim/` for the native build).

    # the native reference (run-native) and the sandbox driver (run-wbx)
    meson setup build/meson-native -Dminibox_dir="$MB"
    ninja -C build/meson-native

    # the guest, core.wbx
    sh waterbox/setup-guest.sh -m "$MB"
    ninja -C build/meson-guest core.wbx

On aarch64 a freshly linked `core.wbx` must have its thread-pointer reads
rewritten before the sandbox takes it (`build-package.sh` does it):

    python3 "$MB/source/guest/wbx-tp-rewrite.py" build/meson-guest/core.wbx

## The package

    ./waterbox/build-package.sh -m "$MB" -r "$CHIMERA"

It builds the guest if needed, checks it with miniBox's `check-wbx.sh`, and
writes `$CHIMERA/build/Cores/bsnes115.chimeraCore`: `core.wbx`,
`waterbox.config`, `file_slots.json`, `default_keybinds.json`, the language
tables, the controller picture, `build.json` and the licence texts. The
package is the same bytes every time for the same commit: the zip's entries
are sorted and dated 1980, and the script builds it twice to check. A
package built from a commit says `<commit>+local` as its version; CI sets
`CORE_VERSION` to the commit.

## The tests

**The core gate** compares the two builds:

    ./waterbox/run-gate.sh

For each test it runs the same input through `run-native` and through the
sandbox (`run-wbx`), and compares a hash of every picture, of the sound, the
count of frames the game did not read its controllers, and every memory
view. Then the same in the sandbox with a save and load of the whole machine
around every frame, and with the picture switched off for the first half of
the run (Chimera's turbo). It runs the two homebrew games in `tests/roms`:
with two movies recorded for chimera-core-snes9x (bsnes plays them
differently, but they are the same input in both builds), with a pattern of
presses, with the cycle-based picture processor, and with every device the
ports take. Each device test also checks that its machine is not the one the
parser's fallback would build. Then a save in and out, an MSU-1 pack, and a
DSP-1 cartridge with and without firmware (The Last Super with one header
byte changed, and 8 KiB of zeros as the firmware). Its last two tests,
`engine:input-polls` and `engine:game-firmware`, run projects through
Chimera's `chimera-run` and need `CHIMERA_ROOT=$CHIMERA` with the package
installed; without it, they say SKIP.

**The frontend gate** runs the package in Chimera's window, headless:

    ./waterbox/tests/run-frontend.sh --chimera-root "$CHIMERA"

A boot whose work RAM must equal the native build's, buttons held through
the frontend against the same input as a movie in the native build, a port
setting that must reach the machine, and the package's key bindings being
the window's when nobody changed them. It needs Chimera built
(`$CHIMERA/tools/ci-chimera.sh`, or the steps in Chimera's AGENTS.md).

**Chimera's package tests** read the package and need no game:

    "$CHIMERA/build/meson-linux/source/ui/test-packages" "$CHIMERA/build/Cores"

**Games that may not be passed on** run from `tests/roms-local/`, which git
ignores:

    ./waterbox/tests/run-local.sh

The script lists the file names it looks for. Each game present must come
out the same in both builds and with a save and load around every frame;
one absent says SKIP.

## Troubleshooting

- `'runtime_error' is not a member of 'std'`: nall from 2019 counted on
  `<stdexcept>` arriving through other headers. `meson.build` adds
  `-include stdexcept`; a build that lost it fails this way.
- `reads its thread pointer from TPIDR_EL0`: an aarch64 `core.wbx` that was
  not rewritten (see "The two builds").
- `multiple definition of Processor::GSU::...`: the GSU, HG51B and uPD96050
  are compiled inside `sfc/coprocessor`, as upstream's makefiles do; do not
  list them again.
- The native build's sound differs from the sandbox's on aarch64 only: the
  native build lost `-ffp-contract=off` (GCC fuses multiply-adds otherwise).
