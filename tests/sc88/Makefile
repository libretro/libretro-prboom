# Golden lane for the SC-88 emulator: src/sc88.c has to build as strict
# C89 with no warnings, and has to produce the audio recorded from the
# build that matched 88emu's C++ sample for sample.  sc88_test.c says
# what is run; no ROMs are needed.
#
#   make -C tests/sc88 check
#
# SAN= builds it under a sanitizer.

RA     := ../..
CFLAGS := -O2 -std=c89 -pedantic -Wall -Wextra -Werror -Wno-long-long \
          -Werror=declaration-after-statement -DSC88_TEST -I$(RA)/src \
          -DSC88_GOLD_A=0x6163d659c291e8b2ull -DSC88_GOLD_B=0xb0811dc9d95ab9dfull
ifneq ($(SAN),)
CFLAGS += -g -fsanitize=$(SAN) -fno-omit-frame-pointer
endif

all: sc88_test

sc88_test: sc88_test.c $(RA)/src/sc88.c $(RA)/src/sc88.h
	$(CC) $(CFLAGS) -o $@ sc88_test.c $(RA)/src/sc88.c

check: all
	./sc88_test

clean:
	rm -f sc88_test

.PHONY: all check clean
