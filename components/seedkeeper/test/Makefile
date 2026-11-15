# Host tests for the SeedKeeper plaintext applet layer.  No card needed.
CC ?= cc
MBED ?= ../../../kern/third_party/mbedtls
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -I.. -I../../nfc \
          -I$(MBED)/include -I$(MBED)/library
SRC = test_seedkeeper.c ../seedkeeper.c $(MBED)/library/sha256.c \
      $(MBED)/library/platform_util.c
HDR = ../seedkeeper.h ../../nfc/nfc.h

test_seedkeeper: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC)

test: test_seedkeeper
	./test_seedkeeper

clean:
	rm -f test_seedkeeper

.PHONY: test clean
