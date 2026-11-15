CC = gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
CPPFLAGS += -I..

BIN = test_xbt
SRC = ../blake2b.c ../block_header.c test_xbt.c
HDR = ../blake2b.h ../block_header.h

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRC)

test: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all test clean
