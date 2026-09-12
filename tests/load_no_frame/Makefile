# Regression lane: the core must not call the video refresh callback
# from inside retro_load_game.  A frontend may have no video driver up
# for the whole of a load, so a frame pushed from there reaches a
# driver with a NULL instance handle.
#
#   make -C tests/load_no_frame check IWAD=/abs/path/to/freedoom1.wad
#
# Any IWAD the core accepts will do; CI fetches Freedoom.  SAN= builds
# the harness under a sanitizer, to be paired with a core built the
# same way (make CC="cc -fsanitize=address,undefined -g").

CORE   ?= ../../prboom_libretro.so
IWAD   ?= freedoom1.wad
RA     := ../..
CFLAGS ?= -O1 -g -Wall -W
CFLAGS += -I$(RA)/libretro/libretro-common/include
ifneq ($(SAN),)
CFLAGS += -fsanitize=$(SAN) -fno-omit-frame-pointer
endif

all: load_no_frame_test

load_no_frame_test: load_no_frame_test.c
	$(CC) $(CFLAGS) -o $@ $< -ldl

check: all
	@test -f $(IWAD) || { echo "no IWAD at $(IWAD); pass IWAD=<path>"; exit 2; }
	./load_no_frame_test $(CORE) $(IWAD)

clean:
	rm -f load_no_frame_test

.PHONY: all check clean
