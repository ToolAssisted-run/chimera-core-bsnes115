# The movies in this folder

Input for the tests, one frame a line, in quickerSnes9x's `.sol` format.
They were recorded for chimera-core-snes9x, on Snes9x: bsnes plays them
differently, so they are not runs of the games here, only the same input
given to both builds. `christmasCraze.playaround.sol` and
`theLastSuper.playaround.sol` drive the homebrew games in `tests/roms`;
`princeOfPersia.anyPercent.sol` and `arkanoid.anyPercent.sol` drive games
that may not be passed on, which `waterbox/tests/run-local.sh` runs from
`tests/roms-local/`.
