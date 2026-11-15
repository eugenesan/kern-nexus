CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
SRC = sha256.c unified_sighash.c vectors.c test_unified.c
test_unified: $(SRC) sha256.h unified_sighash.h
	$(CC) $(CFLAGS) -I. -o $@ $(SRC)
	./$@
clean:
	rm -f test_unified
.PHONY: clean
