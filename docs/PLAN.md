# chimera-core-bsnes115: the plan and what was decided

The design log of this core. Newest entries last.

## Why a core of its own (user-decided, 2026-11-12)

Chimera issue 572 asked for bsnes v115: a movie begun on another frontend's
bsnes v115 core is meant to be finished in Chimera, and a movie plays the
same only on the same emulator. Chimera already had two Super Nintendos
(chimera-core-snes9x, and ares's, which descends from bsnes but is years
newer), and neither is v115. So this core is upstream's v115 release, kept
there, as the Godot cores are one per Godot version. The owner asked for
this core to be built.

Nothing was taken from any other frontend's bsnes port: the adapter is
written from upstream's own two frontends, `target-bsnes` (the desktop
program) and `target-libretro`.

## The answers a core request needs (2026-11-12)

- **Licence**: bsnes is GPL-3.0-only; libco and nall are ISC; SameBoy,
  inside it for the Super Game Boy, is MIT. All allow modified binary
  redistribution. The package is distributed under the GPL version 3.
- **One thread, one clock**: bsnes runs every chip as a coroutine (libco)
  on one thread and schedules them by their clocks. Built without OpenMP
  (with it, the fast picture processor draws lines on several threads).
- **The picture**: the emulator's own framebuffer, 15-bit colour, up to 512
  by 480. No GPU.
- **No recompiler.**
- **What the user brings**: the games; for a Game Boy game, the Super Game
  Boy's cartridge and boot ROM; for some coprocessors, their firmware.
- **What CI can run**: two homebrew games (tests/roms), as
  chimera-core-snes9x does.

## How it is built (2026-11-12)

- The sources are the ones upstream's makefiles list for its libretro core:
  `emulator`, `sfc/*` (one unit per directory, as upstream compiles them),
  the WDC65816, SPC700 and ARM7TDMI processors (the GSU, HG51B and uPD96050
  are compiled inside `sfc/coprocessor`), and SameBoy's `gb/Core`.
- `-include stdexcept` for C++: nall (2019) counted on `<stdexcept>` coming
  through other standard headers, which a current libstdc++ does not do.
- Upstream's game database (`Database/Super Famicom.bml`, 377 KB) is
  compiled in by `waterbox/embed.py`; the guest has no folder to read it
  from. The board database and the sound processor's 64-byte boot ROM come
  from `target-libretro/resources.hpp`, as upstream's libretro core takes
  them.
- **libco is this repository's** (`waterbox/libco.c`). Upstream's takes each
  chip's stack from malloc; the sandbox must be told a stack is a stack
  (`MAP_STACK`), as the ares and DOSBox-X cores found. Upstream's aarch64
  switch also does not keep d8-d15, which the AArch64 calling convention
  requires a callee to keep; this one does (its switch is ares's). bsnes
  restarts a chip at power-on with `co_derive` on the chip's own stack,
  which this libco supports. `co_serializable()` is 0, so bsnes never
  copies stacks into a state of its own: Chimera's savestates are the
  sandbox's whole memory and never go through bsnes's serializer.

## The patches (2026-11-12)

- **0001** - the cycle-based picture processor's video memory is private
  in bsnes; an accessor lets the memory viewer show it.
- **0002** - bsnes seeds its power-on randomness ("entropy") from `clock()`,
  the host's processor time. In the sandbox that is miniBox's clock, the
  same every run, but natively it is not, and the two builds must agree. The
  seed is now 0. The pattern of memory at power-on is still the one the
  entropy setting picks; it is the same on every run and every machine.
  Whether another frontend's bsnes v115 seeds the same way is not known: a
  movie that depends on it may not sync with Entropy at Low or High.
- **0003** - bsnes v115 compiles in two 256-byte Super Game Boy boot ROMs
  (`sfc/coprocessor/icd/boot-roms.cpp`). Their SHA-1s,
  AA2F50A77DFB4823DA96BA99309085A3C6278515 and
  93407EA10D2F30AB96A314D8ECA44FE160AEA734, are those of Nintendo's own SGB
  and SGB2 boot ROMs, so the package must not carry them. The patch drops
  the file from the build and has the Super Game Boy read its boot ROM from
  the frontend, which mounts the person's firmware. The patch removes only
  the `#include`: the bytes are not in this repository.

## The adapter (2026-11-12)

`waterbox/bsnes.cpp` is the part of a frontend bsnes needs:

- **Loading**: the desktop program's recipe. The ROM's copier header is
  dropped, the game is looked up in the database by the SHA-256 of its ROM
  and otherwise described by upstream's heuristics, and the desktop
  program's per-game adjustments (`hackCompatibility`: fast joypad polling
  for three games, the cycle-based picture processor or sound chip for
  about a dozen, entropy None for two with hotfixes on) are applied after
  the project's settings. The boot line on stderr says verified or not, the
  title, the region, which picture processor, and whether an MSU-1 pack was
  found.
- **Settings** are upstream's own options (`Hacks/...`) with upstream's
  defaults: entropy Low, hotfixes on, fast picture processor on with
  deinterlace, fast sound chip on, coprocessor delayed sync on, HLE
  preferred off, every overclock 100. HD Mode 7 is not offered (it changes
  the picture's size and is not the console). Blur emulation is off.
- **Firmware**: a Game Boy game in a Super Game Boy takes the cartridge and
  boot ROM as package firmware (`sgb1.sfc` and `sgb1.boot.rom`, or `sgb2.*`
  by the "Super Game Boy" setting). Coprocessor firmware depends on the
  game, so `SuggestSettings` reads the cartridge's board from the database
  and the heuristics and returns what it needs ("gameFirmware"): one file
  per chip, `<identifier>.rom`, program and data together, which is how
  bsnes splits firmware appended to a ROM. For the chips bsnes has its own
  versions of (DSP-1 to 4, ST010, Cx4) the firmware is asked for only while
  "Prefer Coprocessor HLE" is off.
- **Controllers**: the ports take what bsnes offers - port 1 a gamepad, a
  mouse or nothing; port 2 also the multitap (players 2 to 5), the Super
  Scope, a Justifier or two. The pads' columns are in chimera-core-snes9x's
  order. Mice and guns move by an amount each frame, which is how bsnes
  takes them.
- **Lag**: a frame in which bsnes never asked for input. The Lua input-poll
  callback runs at the first read of a frame.
- **Sound**: bsnes's own resampler, to 44100 Hz.
- **Picture**: 256 or 512 wide, 224 lines (448 interlaced), 240 or 480 with
  "Show Overscan". Turbo skips only the conversion to the frontend's
  format; the emulator draws either way.
- **Memory views**: WRAM, CARTRAM (the SA-1's or Super FX's RAM when the
  cartridge has one), SA1 IRAM, VRAM, APURAM, CARTROM.
- **Save data**: an export runs bsnes's own save, which writes its files
  through the frontend, and hands Chimera what it wrote (`save.ram`, and
  for a Super Game Boy `gameboy.save.ram`). The "save" slot puts a save
  back at boot.

## What was measured (2026-11-12)

- **The core gate**, 33 tests, all pass: native == sandbox == a save and
  load around every frame == half the run undrawn, over Christmas Craze
  (3101 frames of input) and The Last Super (3771, also with the
  cycle-based picture processor); every device the ports take, each also
  told apart from the machine a broken parser would build; a save going in
  through the "save" slot and back out through the export; an MSU-1 pack
  found and not; a DSP-1 cartridge (The Last Super with its header's
  cartridge type changed) on bsnes's own chip and, given firmware, on the
  firmware; and through Chimera's `chimera-run`, the input-poll callback
  and the firmware a DSP-1 cartridge asks for. The last four were each
  broken on purpose once and seen to fail.
- **The frontend gate**, 4 tests, all pass, in Chimera's window run
  headless.
- **Chimera's package tests**, 25, all pass.
- **Games that may not be passed on** (`waterbox/tests/run-local.sh`), all
  native == sandbox == rerecord: Super Mario World (3000 frames, and again
  with the cycle-based picture processor and sound chip), Breath of Fire,
  Super Mario Collection, Prince of Persia (a 117181-frame movie), Arkanoid
  - Doh It Again, and Kirby's Hoshinoka-Bi in the Super Game Boy and in the
  Super Game Boy 2.
- **Windows**: the same package through Chimera's Windows `chimera-run`
  (run from WSL) gave the same work RAM and the same picture as Linux for
  Super Mario World after 900 frames, for Kirby in the Super Game Boy, and
  for a 3000-frame movie with input that reaches Super Mario World's first
  message box; that movie also replayed the same in two processes on Linux.
- **aarch64**: built and tested by CI only.

## Not done (2026-11-12)

- The Satellaview (BS-X) and the Sufami Turbo: their loaders and slots.
- No game with a coprocessor has been run, with firmware or without: none
  was at hand. The way firmware reaches the chip is tested (above), not a
  game on it.
- Whether a movie made on another frontend's bsnes v115 core plays the same
  here is not known: none was tried. The power-on seed (patch 0002) and the
  controller wiring are the places it could differ.
- HD Mode 7, the picture filters, run-ahead and bsnes's own rewind and
  savestates are frontend features Chimera does not need.
