# Regression lane: content that restates what the game already is must
# not change it.  Each mode runs the title and demo sequence twice, each
# run in a process of its own, and requires the same frames and the same
# saved state size from both; same_frames_test.c says what each mode
# loads and what it guards.
#
#   make -C tests/same_frames check IWAD=/abs/path/to/freedoom1.wad
#
# A Doom 1 IWAD is needed; CI fetches Freedoom.  SAN= builds the harness
# under a sanitizer, to be paired with a core built the same way
# (make CC="cc -fsanitize=address,undefined -g").

CORE   ?= ../../prboom_libretro.so
IWAD   ?= freedoom1.wad
RA     := ../..
CFLAGS ?= -O1 -g -Wall -W
CFLAGS += -I$(RA)/libretro/libretro-common/include
ifneq ($(SAN),)
CFLAGS += -fsanitize=$(SAN) -fno-omit-frame-pointer
endif

all: same_frames_test

same_frames_test: same_frames_test.c
	$(CC) $(CFLAGS) -o $@ $< -ldl

check: all
	@test -f $(IWAD) || { echo "no IWAD at $(IWAD); pass IWAD=<path>"; exit 2; }
	./same_frames_test $(CORE) $(IWAD) pointer
	./same_frames_test $(CORE) $(IWAD) mapinfo
	./same_frames_test $(CORE) $(IWAD) bits

clean:
	rm -f same_frames_test identity.deh *.m3u zmapinfo.wad nomonsters.wad nomonsters_bits.wad

.PHONY: all check clean
