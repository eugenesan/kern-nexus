# Static ARM probe for the Nexus 5.  Push to /data/local/tmp and run as root.
CC = arm-linux-gnueabihf-gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -march=armv7-a -mtune=cortex-a15 \
          -mfpu=neon-vfpv4 -mfloat-abi=hard -static -I..

nfc_probe: nfc_probe.c ../nfc_nci.c ../nfc_bcm2079x.c ../nfc.h
	$(CC) $(CFLAGS) -o $@ nfc_probe.c ../nfc_nci.c ../nfc_bcm2079x.c

clean:
	rm -f nfc_probe

.PHONY: clean
