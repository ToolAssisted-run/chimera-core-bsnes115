# chimera-core-bsnes115

**bsnes v115 as a Chimera core.** Upstream
[bsnes](https://github.com/bsnes-emu/bsnes) at its v115 release, built into
[miniBox](https://github.com/ToolAssisted-run/chimera-common-minibox)'s
sandbox and packaged for [Chimera](https://github.com/ToolAssisted-run/chimera),
a frontend for tool-assisted speedruns. It plays Super Nintendo and Super
Famicom cartridges, and Game Boy cartridges in a Super Game Boy.

It is v115 on purpose, and stays there: a movie made on bsnes v115
elsewhere needs the same emulator to play the same way. Chimera's other
Super Nintendo core is [chimera-core-snes9x](https://github.com/ToolAssisted-run/chimera-core-snes9x).

## Using it in Chimera

Chimera ships no cores and downloads none. Download
`bsnes115-<version>-<cpu>.chimeraCore` from this repository's
[Releases](https://github.com/ToolAssisted-run/chimera-core-bsnes115/releases)
page (dated `nightly-YYYY-MM-DD` builds) and put it in the `Cores` folder
beside Chimera's program. Tools > Core Manager... lists the cores there.

You provide the games, and for some of them firmware:

- **A Game Boy game** is played in a Super Game Boy: the project asks for
  the Super Game Boy's cartridge and its boot ROM (or the Super Game Boy
  2's, by the "Super Game Boy" setting).
- **A cartridge with a coprocessor** (DSP-1 to DSP-4, ST010, ST011, ST018,
  Cx4) asks for that chip's firmware, as one file (`dsp1b.rom`, `st018.rom`
  and so on). bsnes has its own versions of all but the ST011 and ST018:
  turn on "Prefer Coprocessor HLE" to use them instead.

Not offered: the Satellaview (BS-X) and the Sufami Turbo.

## Build and test

[docs/BUILDING.md](docs/BUILDING.md) has every step from a fresh clone, and
[AGENTS.md](AGENTS.md) the short version. What was decided, and why, is in
[docs/PLAN.md](docs/PLAN.md).

## Licence

This repository's own files are MIT; its patches to bsnes are GPL-3.0-only.
The package contains bsnes and is distributed under the GPL version 3. See
[LICENSE](LICENSE).
