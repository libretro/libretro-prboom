# AGENTS.md

Instructions for anyone, human or agent, changing libretro-prboom.
Read the section that covers the code you touch before touching it.

## Ground rules

- **C89 for new code.** A new source file is C89 that MSVC compiles:
  `/* */` comments, declarations at the top of a block.
- **No locks, no sleeps.** Threads hand work over through retro_atomic,
  SPSC rings and eventcount park/wake. No mutex, no spinning, no sleep
  or wall-clock poll: a thread waits on a real event.
- **Zero-copy.** Content is read in place where it already is - a
  mapping, a decoder's buffer - and lent rather than copied. Every copy
  on a data path is one to remove.
- **Nothing allocated during play.** Buffers are sized and allocated at
  load or on a resolution change, never per frame.
- **Self-contained build.** The core builds from this tree with no
  system development packages; what it needs is in the tree.
- **Every fix ships with its test**, which fails on the broken code and
  passes on the fix.
- **Both toolchains.** A change is built on Linux and cross-compiled for
  Windows with mingw-w64 before it goes in.

## libretro-common: RetroArch first

`libretro/libretro-common/` is a copy of RetroArch's `libretro-common/`
(https://github.com/libretro/RetroArch), file for file. No file there is
ever edited in this core.

- **Upstream first.** Anything the core needs changed in libretro-common
  - a fix, a new function, a new mode of an existing one - is made in
  RetroArch's `libretro-common/` first, as a patch against RetroArch,
  with its test under `libretro-common/samples/`. Once RetroArch has it,
  the core takes it by a resync. A core adaptation that is not a
  libretro-common change goes in the core's own files.
- **A resync is a copy, never a merge.** Every file here that RetroArch
  has is replaced by RetroArch's, so a change made only here is deleted
  by the next resync. Before one, compare the two: anything this copy has
  that RetroArch's lacks goes upstream first.
- **Bring in what the new files need.** A synced file that now needs
  another (a new source, a new header) brings it along, and the new
  source is added to every build that lists its neighbour: the
  Makefiles, the Android and CMake builds, the test scripts.
- **Follow moved declarations.** Where a function moved to another
  header, every call site includes the new one. A clean build of the
  core has no implicit-declaration warning: GCC 14 makes it an error, and
  older compilers cut the returned pointer to an int.

## Tests

Each directory under `tests/` builds a headless host and runs it against
`prboom_libretro.so` with `make -C tests/<name> check`. The ones that
load a game need an IWAD: `IWAD=<path to freedoom1.wad>`.

- `same_frames`, `load_no_frame`, `auto_resolution` - frame output and
  the first frame after load.
- `sessions` - repeated load/unload, demo playback, reinit, PWAD node
  loading and save games, compared session to session.
- `hdr_negotiate`, `hdr_rolloff` - HDR negotiation and tone mapping.
- `sc55`, `sc88` - the SC-55 and SC-88 MIDI synths.
