# Host-side NCI framing tests.  No chip or target toolchain needed.
CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -I..
SRC = test_nfc_nci.c ../nfc_nci.c ../nfc_bcm2079x.c
HDR = ../nfc.h ../nfc_nci_internal.h

test_nfc_nci: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC)

test: test_nfc_nci
	./test_nfc_nci

clean:
	rm -f test_nfc_nci

.PHONY: test clean
