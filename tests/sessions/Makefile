# Regression lane: every content load has to behave like the first.
# Session state that outlives a teardown shows up here as a session
# that renders nothing, or one that asks the frontend to shut down.
#
#   make -C tests/sessions check IWAD=/abs/path/to/freedoom1.wad
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

clean:
	rm -f sessions_test

.PHONY: all check clean
