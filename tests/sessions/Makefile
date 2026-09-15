# Regression lane: every content load has to behave like the first.
# Session state that outlives a teardown shows up here as a session
# that renders nothing, or one that asks the frontend to shut down.
# Both content paths are driven: the iwad on its own (title screen and
# demo sequence) and a DEMO1 lump extracted from it (-playdemo).
#
#   make -C tests/sessions check IWAD=/abs/path/to/freedoom1.wad
#
# A third mode, not in check because it needs a second IWAD, alternates
# two content files across sessions so consecutive loads build different
# lump tables -- the content switch a frontend actually performs, and the
# case where an index a teardown failed to drop addresses the wrong lump.
# Each content keeps its own reference frames:
#
#   ./sessions_test ../../prboom_libretro.so freedoom1.wad 4 300 alt freedoom2.wad
#
# And state mode, also out of check: it saves the state mid-run, plays
# on, restores, and replays the same stretch, which is what run-ahead
# and rewind do to every frame.
#
# It compares frames, and frames are an imperfect proxy here.  The
# simulation across a restore is exact -- gametic, leveltime, the tic
# fraction and the player's position and angle come back identical at
# every save point tested.  What differs is interpolation: PrevX/PrevY/
# PrevZ sit at the end of mobj_t and are left out of the savegame on
# purpose, to keep the prboom 2.4.4 format, so every mobj except the
# view player (whose values extra_serialize carries separately)
# interpolates from a stale origin until its next tic.  How many frames
# that shows for depends on the sub-tic phase when the state was taken
# and on how much is moving, which is why the verdict moves with the run
# count: on the demo content, saving at frame 120 replays exactly, 130
# diverges six frames later, 150 differs only on the first frame, and
# 100 differs throughout.
#
# So it stays a diagnostic.  It is worth running against a change to the
# state format -- it is what caught the demo read position going missing,
# where the simulation really did diverge -- but a frame difference here
# is not by itself a fault:
#
#   ./sessions_test ../../prboom_libretro.so freedoom1.wad 3 300 state
#
# Any IWAD the core accepts will do; CI fetches Freedoom.  SAN= builds
# the harness under a sanitizer, to be paired with a core built the
# same way (make CC="cc -fsanitize=address,undefined -g").

CORE     ?= ../../prboom_libretro.so
# The nodes lane needs the title screen's demo to have reached its map
# before the run ends; the replacement map is not loaded until then, and
# the lane fails rather than passing vacuously if it has not.  That takes
# somewhere under 300 runs with Freedoom, so this carries real margin
# instead of sitting on the boundary.
NODERUNS ?= 600
SESSIONS ?= 3
RUNS     ?= 300
IWAD   ?= freedoom1.wad
RA     := ../..
CFLAGS ?= -O1 -g -Wall -W
CFLAGS += -I$(RA)/libretro/libretro-common/include
ifneq ($(SAN),)
CFLAGS += -fsanitize=$(SAN) -fno-omit-frame-pointer
endif

all: sessions_test

sessions_test: sessions_test.c
	$(CC) $(CFLAGS) -o $@ $< -ldl

check: all
	@test -f $(IWAD) || { echo "no IWAD at $(IWAD); pass IWAD=<path>"; exit 2; }
	./sessions_test $(CORE) $(IWAD) $(SESSIONS) $(RUNS)
	./sessions_test $(CORE) $(IWAD) $(SESSIONS) $(RUNS) demo
	./sessions_test $(CORE) $(IWAD) 1 $(NODERUNS) nodes

clean:
	rm -f sessions_test demo1.lmp bad_short.wad bad_magic.wad bad_dir.wad
	rm -f nodes_good.wad nodes_trunc.wad nodes_count.wad nodes_line.wad
	rm -f nodes_vert.wad

.PHONY: all check clean
