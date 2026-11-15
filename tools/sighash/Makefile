CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
CORE = ../../kern/main/core
SRC = $(CORE)/unified_sha256.c $(CORE)/unified_sighash.c vectors.c test_unified.c
HDR = $(CORE)/unified_sha256.h $(CORE)/unified_sighash.h
test_unified: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -I$(CORE) -o $@ $(SRC)
	./$@
clean:
	rm -f test_unified
.PHONY: clean
