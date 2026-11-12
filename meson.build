# chimera-core-bsnes115: bsnes v115 in miniBox's sandbox, as a Chimera core.
#
# Native reference build (run-native, and run-wbx, which links miniBox's host
# library out of the same meson-cpp build dir):
#     meson setup build/meson-native && ninja -C build/meson-native
# Guest (core.wbx) build, through the cross file waterbox/setup-guest.sh
# writes (needs miniBox's C++ guest toolchain, build/meson-cpp):
#     sh waterbox/setup-guest.sh && ninja -C build/meson-guest core.wbx
#
# The sources are the ones upstream's GNUmakefiles build for its libretro
# core (bsnes/GNUmakefile, sfc/, gb/ and processor/; the coprocessors'
# processors are compiled inside sfc/coprocessor), with libco replaced by
# waterbox/libco.c and the frontend by waterbox/bsnes.cpp. Both builds
# compile the same files; only emulibc.h differs.
project('chimera-core-bsnes115', ['c', 'cpp'],
  default_options: ['c_std=gnu11', 'cpp_std=gnu++17', 'optimization=2', 'debug=false', 'warning_level=0'])

b = 'extern/bsnes'
bs = b / 'bsnes'

if not import('fs').is_file(bs / 'sfc/sfc.hpp')
  error('extern/bsnes is empty - run: git submodule update --init')
endif

# the submodule pin is upstream's v115; patches/ go on top (idempotent)
run_command(files('waterbox/apply-patches.sh'), check: true)

mb = get_option('minibox_dir')
if mb == ''
  mb_candidate = meson.current_source_dir() / '../chimera/extern/chimera-common-minibox'
  if import('fs').is_dir(mb_candidate)
    mb = mb_candidate
  else
    error('pass -Dminibox_dir=<miniBox checkout> (or keep one at ../chimera/extern/chimera-common-minibox)')
  endif
endif

# Upstream's flags for a build (nall/GNUmakefile, bsnes/GNUmakefile,
# gb/GNUmakefile), less OpenMP: the fast picture processor draws its lines in
# parallel when built with it, and a core has one thread.
add_project_arguments(
  '-DBUILD_RELEASE',
  '-fno-strict-aliasing', '-fwrapv',
  '-Wno-narrowing', '-Wno-multichar',
  language: ['c', 'cpp'])
# nall (2019) leans on <stdexcept> arriving through other standard headers,
# which a newer libstdc++ no longer does.
add_project_arguments('-include', 'stdexcept', language: 'cpp')
add_project_arguments('-DGB_INTERNAL', '-DDISABLE_DEBUGGER', '-D_GNU_SOURCE', language: 'c')
# On aarch64 GCC fuses a multiply and an add into one instruction unless told
# not to, which rounds once where the guest (built with -ffp-contract=off)
# rounds twice; the sound is mixed in floating point. Both keep them apart.
if not meson.is_cross_build() and host_machine.cpu_family() == 'aarch64'
  add_project_arguments('-ffp-contract=off', language: ['c', 'cpp'])
endif

inc = include_directories('waterbox', b, bs, b / 'libco')

# Database/Super Famicom.bml, upstream's database of verified cartridges,
# compiled in: the guest has no folder to read it from.
database_h = custom_target('database.h',
  input: bs / 'Database/Super Famicom.bml',
  output: 'database.h',
  command: [find_program('python3'), files('waterbox/embed.py'), '@INPUT@', '@OUTPUT@', 'SuperFamicomDatabase'])

core_sources = files(
  bs / 'emulator/emulator.cpp',
  bs / 'sfc/interface/interface.cpp',
  bs / 'sfc/system/system.cpp',
  bs / 'sfc/controller/controller.cpp',
  bs / 'sfc/cartridge/cartridge.cpp',
  bs / 'sfc/memory/memory.cpp',
  bs / 'sfc/cpu/cpu.cpp',
  bs / 'sfc/smp/smp.cpp',
  bs / 'sfc/dsp/dsp.cpp',
  bs / 'sfc/ppu/ppu.cpp',
  bs / 'sfc/ppu-fast/ppu.cpp',
  bs / 'sfc/expansion/expansion.cpp',
  bs / 'sfc/coprocessor/coprocessor.cpp',
  bs / 'sfc/slot/slot.cpp',
  bs / 'processor/arm7tdmi/arm7tdmi.cpp',
  bs / 'processor/spc700/spc700.cpp',
  bs / 'processor/wdc65816/wdc65816.cpp',
  bs / 'gb/Core/apu.c',
  bs / 'gb/Core/camera.c',
  bs / 'gb/Core/display.c',
  bs / 'gb/Core/gb.c',
  bs / 'gb/Core/joypad.c',
  bs / 'gb/Core/mbc.c',
  bs / 'gb/Core/memory.c',
  bs / 'gb/Core/printer.c',
  bs / 'gb/Core/random.c',
  bs / 'gb/Core/rewind.c',
  bs / 'gb/Core/save_state.c',
  bs / 'gb/Core/sgb.c',
  bs / 'gb/Core/sm83_cpu.c',
  bs / 'gb/Core/symbol_hash.c',
  bs / 'gb/Core/timing.c',
  'waterbox/libco.c',
  'waterbox/bsnes.cpp',
) + [database_h]

if meson.is_cross_build()
  # ---- the guest: core.wbx --------------------------------------------------
  # miniBox's musl/libstdc++ toolchain arrives through the cross file; the
  # link recipe is the C++ guest kit's (cxxglue for the unwinder, weak
  # pthread pulls for libgcc_eh), as chimera-core-snes9x links.
  mbdir = meson.get_external_property('minibox_dir')
  mbuild = meson.get_external_property('minibox_build')
  sr = meson.get_external_property('guest_sysroot')

  executable('core', core_sources,
    name_suffix: 'wbx',
    include_directories: inc,
    link_args: [
      '-static', '-no-pie',
      '-Wl,--eh-frame-hdr', '-Wl,-O2', '-Wl,--no-relax', '-Wl,-z,stack-size=8388608',
      '-T', mbdir / 'source/guest/linkscript.T',
      '-Wl,-u,pthread_once', '-Wl,-u,pthread_cond_wait',
      '-Wl,-u,pthread_cond_broadcast', '-Wl,-u,pthread_key_create',
      mbuild / 'source/guest/cxxglue.c.o',
      mbuild / 'source/guest/emulibc.c.o',
      '-L' + sr / 'lib', '-lstdc++', '-lgcc', '-lgcc_eh', '-lc'])
else
  # ---- the native reference -------------------------------------------------
  m = meson.get_compiler('c').find_library('m', required: false)
  executable('run-native', core_sources + files('waterbox/run-native.c'),
    include_directories: [include_directories('waterbox/native-shim'), inc],
    c_args: ['-I' + mb / 'source/guest/include', '-I' + mb / 'extern/jsmn'],
    cpp_args: ['-I' + mb / 'source/guest/include', '-I' + mb / 'extern/jsmn'],
    dependencies: [m])

  # The host library comes from the same miniBox build dir as the guest
  # toolchain: meson-cpp (-Dguest_cpp=true), the one CI builds.
  mbhost = mb / 'build/meson-cpp/source/host'
  executable('run-wbx', files('waterbox/run-wbx.c'),
    c_args: ['-I' + mb / 'source/host'],
    link_args: [mbhost / 'libminiboxhost.so', '-Wl,-rpath,' + mbhost])
endif
