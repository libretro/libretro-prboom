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
# Any IWAD the core accepts will do; CI fetches Freedoom.  SAN= builds
# the harness under a sanitizer, to be paired with a core built the
# same way (make CC="cc -fsanitize=address,undefined -g").

CORE     ?= ../../prboom_libretro.so
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

clean:
	rm -f sessions_test demo1.lmp

.PHONY: all check clean
