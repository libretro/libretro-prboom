# Regression lane: HDR10 output is negotiated through the versioned
# RETRO_ENVIRONMENT_GET_HDR_METADATA query, and only when the frontend
# reports native 10-bit source support.
#
#   make -C tests/hdr_negotiate check IWAD=/abs/path/to/freedoom1.wad
#
# Any IWAD the core accepts will do.  SAN= builds the harness under a
# sanitizer, to be paired with a core built the same way.

RA     := ../..
IWAD   ?= freedoom1.wad
CFLAGS ?= -O1 -g -Wall -W
CFLAGS += -std=c89 -I$(RA)/libretro/libretro-common/include
ifneq ($(SAN),)
CFLAGS += -fsanitize=$(SAN) -fno-omit-frame-pointer
endif

ifeq ($(OS),Windows_NT)
CORE   ?= ../../prboom_libretro.dll
EXE    := hdr_negotiate_test.exe
LIBS   :=
else
CORE   ?= ../../prboom_libretro.so
EXE    := hdr_negotiate_test
LIBS   := -ldl
endif

all: $(EXE)

$(EXE): hdr_negotiate_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

check: all
	@test -f $(IWAD) || { echo "no IWAD at $(IWAD); pass IWAD=<path>"; exit 2; }
	./$(EXE) $(CORE) $(IWAD) yes
	./$(EXE) $(CORE) $(IWAD) no
	./$(EXE) $(CORE) $(IWAD) refuse

clean:
	rm -f hdr_negotiate_test hdr_negotiate_test.exe

.PHONY: all check clean
