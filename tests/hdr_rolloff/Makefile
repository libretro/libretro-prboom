# Regression lane: HDR10 highlight rolloff toward the frontend's peak and
# the automatic emissive boost, tested against src/vid_mode.c directly.
#
#   make -C tests/hdr_rolloff check
#
# SAN= builds under a sanitizer.

RA     := ../..
CFLAGS ?= -O1 -g -Wall -W
CFLAGS += -std=c89 -I$(RA)/src -I$(RA)/libretro/libretro-common/include
ifneq ($(SAN),)
CFLAGS += -fsanitize=$(SAN) -fno-omit-frame-pointer
endif

ifeq ($(OS),Windows_NT)
EXE    := hdr_rolloff_test.exe
else
EXE    := hdr_rolloff_test
endif

all: $(EXE)

$(EXE): hdr_rolloff_test.c $(RA)/src/vid_mode.c $(RA)/src/vid_mode.h
	$(CC) $(CFLAGS) -o $@ hdr_rolloff_test.c $(RA)/src/vid_mode.c -lm

check: all
	./$(EXE)

clean:
	rm -f hdr_rolloff_test hdr_rolloff_test.exe

.PHONY: all check clean
