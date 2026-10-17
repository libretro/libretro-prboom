# Regression lane: the "auto" internal resolution follows the panel size
# reported by RETRO_ENVIRONMENT_GET_DISPLAY_METADATA.
#
#   make -C tests/auto_resolution check IWAD=/abs/path/to/freedoom1.wad
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
EXE    := auto_resolution_test.exe
LIBS   :=
else
CORE   ?= ../../prboom_libretro.so
EXE    := auto_resolution_test
LIBS   := -ldl
endif

all: $(EXE)

$(EXE): auto_resolution_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

# panel size -> expected 4:3-reference internal size
check: all
	@test -f $(IWAD) || { echo "no IWAD at $(IWAD); pass IWAD=<path>"; exit 2; }
	./$(EXE) $(CORE) $(IWAD) 1920x1080 1440x900
	./$(EXE) $(CORE) $(IWAD) 2560x1440 1920x1200
	./$(EXE) $(CORE) $(IWAD) 1280x1024 1280x800
	./$(EXE) $(CORE) $(IWAD) 3840x2160 2560x1600
	./$(EXE) $(CORE) $(IWAD) 640x480   640x400
	./$(EXE) $(CORE) $(IWAD) 320x240   320x200
	./$(EXE) $(CORE) $(IWAD) none      320x200

clean:
	rm -f auto_resolution_test auto_resolution_test.exe

.PHONY: all check clean
