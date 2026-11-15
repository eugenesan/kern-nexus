# Host tests for the SeedKeeper applet layer.  No card needed.
CC ?= cc
MBED ?= ../../../kern/third_party/mbedtls
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -I.. -I../../nfc \
          -I$(MBED)/include -I$(MBED)/library
COMMON = ../seedkeeper.c $(MBED)/library/sha256.c $(MBED)/library/platform_util.c
HDR = ../seedkeeper.h ../../nfc/nfc.h

all: test_seedkeeper test_seedkeeper_flow

test_seedkeeper: test_seedkeeper.c $(COMMON) $(HDR)
	$(CC) $(CFLAGS) -o $@ test_seedkeeper.c $(COMMON)

test_seedkeeper_flow: test_seedkeeper_flow.c $(COMMON) $(HDR)
	$(CC) $(CFLAGS) -o $@ test_seedkeeper_flow.c $(COMMON)

test: all
	./test_seedkeeper
	./test_seedkeeper_flow

clean:
	rm -f test_seedkeeper test_seedkeeper_flow

.PHONY: all test clean
