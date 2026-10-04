# Golden lane for the SC-55 emulator: src/sc55.c has to build as strict
# C89 with no warnings, and has to produce the audio and machine state
# recorded from the build that matched the Nuked-SC55 backend frame for
# frame.  sc55_test.c says what is run; no ROMs are needed.
#
#   make -C tests/sc55 check
#
# SAN= builds it under a sanitizer.

RA     := ../..
CFLAGS := -O2 -std=c89 -pedantic -Wall -Wextra -Werror -Wno-long-long \
          -Werror=declaration-after-statement -DSC55_TEST -I$(RA)/src
ifneq ($(SAN),)
CFLAGS += -g -fsanitize=$(SAN) -fno-omit-frame-pointer
endif

all: sc55_test

sc55_test: sc55_test.c $(RA)/src/sc55.c $(RA)/src/sc55.h
	$(CC) $(CFLAGS) -o $@ sc55_test.c $(RA)/src/sc55.c

check: all
	./sc55_test

clean:
	rm -f sc55_test

.PHONY: all check clean
