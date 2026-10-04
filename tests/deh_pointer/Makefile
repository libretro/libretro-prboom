# Regression lane: a DeHackEd "Pointer" block has to hand a frame the
# action the named frame had before any patch ran.  A patch pointing
# every frame at itself is therefore a no-op, and the title and demo
# sequence must draw the same frames with it as without it.
#
#   make -C tests/deh_pointer check IWAD=/abs/path/to/freedoom1.wad
#
# Any Doom IWAD the core accepts will do; CI fetches Freedoom.  SAN=
# builds the harness under a sanitizer, to be paired with a core built
# the same way (make CC="cc -fsanitize=address,undefined -g").

CORE   ?= ../../prboom_libretro.so
IWAD   ?= freedoom1.wad
RA     := ../..
CFLAGS ?= -O1 -g -Wall -W
CFLAGS += -I$(RA)/libretro/libretro-common/include
ifneq ($(SAN),)
CFLAGS += -fsanitize=$(SAN) -fno-omit-frame-pointer
endif

all: deh_pointer_test

deh_pointer_test: deh_pointer_test.c
	$(CC) $(CFLAGS) -o $@ $< -ldl

check: all
	@test -f $(IWAD) || { echo "no IWAD at $(IWAD); pass IWAD=<path>"; exit 2; }
	./deh_pointer_test $(CORE) $(IWAD)

clean:
	rm -f deh_pointer_test identity.deh identity.m3u

.PHONY: all check clean
