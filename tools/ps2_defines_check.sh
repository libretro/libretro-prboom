#!/bin/sh
# Emacs style mode select   -*- sh -*-
#-----------------------------------------------------------------------------
#
#  PrBoom: a Doom port merged with LxDoom and LSDLDoom
#  based on BOOM, a modified and improved DOOM engine
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
# DESCRIPTION:
#      The PS2 target compiles with no ps2sdk include path, so a header that
#      reaches for an SDK header under -DPS2 fails there and nowhere else --
#      a vendored libretro-common unit can pick one up without any other
#      build noticing.  This runs the host compiler over the PS2 source set
#      with the PS2 define set and fails on a header that cannot be found.
#
#      It answers "does this configuration have all its headers", not "does
#      it generate correct EE code"; the toolchain build remains the only
#      answer to that.  Run from the repo root:
#
#        sh tools/ps2_defines_check.sh
#
#-----------------------------------------------------------------------------

CC=${CC:-cc}

PS2_DEFINES="-DPS2 -DRETRO_ATOMIC_FORCE_VOLATILE -DSTATIC_LINKING -DMEMORY_LOW
 -DHAVE_LOW_MEMORY -DHAVE_STRLWR -DABGR1555 -DNO_FAST_SQRT -DINLINE=inline
 -DHAVE_RVORBIS -DHAVE_RMP3 -DHAVE_RMODTRACKER -DHAVE_RWAV -DHAVE_RPNG
 -DHAVE_RJPEG"

INCLUDES="-I. -Isrc -Ilibretro/libretro-common/include"

if [ ! -f Makefile.common ]; then
   echo "ps2_defines_check: run me from the repo root" >&2
   exit 1
fi

# Everything Makefile.common hands the PS2 build, minus the units its
# HAVE_THREADS=0 and STATIC_LINKING=1 leave out.
sources=$(grep -oE '\$\(LIBRETRO_COMM_DIR\)/[a-z0-9_/]+\.c' Makefile.common |
   sed 's|\$(LIBRETRO_COMM_DIR)|libretro/libretro-common|' |
   grep -v -e 'rthreads/' -e 'vfs/vfs_hybrid.c' -e 'file/retro_dirent.c' |
   sort -u)
sources="$sources libretro/libretro.c libretro/libretro_sound.c $(ls src/*.c src/heretic/*.c src/hexen/*.c)"

status=0
checked=0

for src in $sources; do
   [ -f "$src" ] || continue
   checked=$((checked + 1))
   missing=$($CC $PS2_DEFINES $INCLUDES -fsyntax-only "$src" 2>&1 |
      grep "No such file or directory")
   if [ -n "$missing" ]; then
      echo "FAIL $src"
      echo "$missing" | sed 's/^/     /'
      status=1
   fi
done

if [ "$status" -eq 0 ]; then
   echo "ps2_defines_check: $checked sources, every header resolved"
fi

exit $status
