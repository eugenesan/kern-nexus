/*
 * Standalone BCM2079x/NCI probe for the Nexus 5.
 *
 * Runs the real bring-up (chip setup, discovery, activation) and prints what
 * happened.  With no card present it exits after the 30 s tag timeout, which
 * still proves the driver and the NCI command/response path.  With a card it
 * also selects the SeedKeeper applet and reports the status word.
 */

#include <stdio.h>

#include "../nfc.h"

static const unsigned char seedkeeper_select[] = {
    0x00, 0xA4, 0x04, 0x00, 0x0A, 0x53, 0x65, 0x65, 0x64, 0x4B,
    0x65, 0x65, 0x70, 0x65, 0x72};

int main(void) {
  kern_nfc_t nfc = {.fd = -1};
  unsigned char resp[256];
  size_t resp_len = sizeof(resp);
  unsigned char sw1 = 0;
  unsigned char sw2 = 0;
  int rc;

  printf("nfc: opening %s\n", KERN_NFC_DEV);
  fflush(stdout);
  if (kern_nfc_open(&nfc) != 0) {
    printf("nfc: open failed\n");
    return 1;
  }

  printf("nfc: polling (up to 30 s without a card)\n");
  fflush(stdout);
  rc = kern_nfc_bring_up(&nfc);
  if (rc != 0) {
    printf("nfc: no tag / bring-up failed (rc=%d)\n", rc);
    kern_nfc_close(&nfc);
    return rc == 0 ? 0 : 2;
  }
  printf("nfc: tag active\n");

  rc = kern_nfc_transceive(&nfc, seedkeeper_select, sizeof(seedkeeper_select),
                           resp, &resp_len, &sw1, &sw2);
  if (rc != 0) {
    printf("nfc: SELECT failed (rc=%d)\n", rc);
    kern_nfc_close(&nfc);
    return 3;
  }
  printf("nfc: SeedKeeper SELECT -> %02X%02X (%u data bytes)\n", sw1, sw2,
         (unsigned)resp_len);

  kern_nfc_close(&nfc);
  return 0;
}
