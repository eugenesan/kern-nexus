#ifndef KERN_NFC_H
#define KERN_NFC_H

#include <stddef.h>
#include <stdint.h>

#define KERN_NFC_DEV "/dev/bcm2079x"
#define KERN_NFC_MAX_PACKET 259

typedef struct {
  int fd;
} kern_nfc_t;

/* Low-level BCM2079x transport.  One read() returns one whole NCI packet
 * (HCIT type + 3-byte NCI header + payload). */
int kern_nfc_open(kern_nfc_t *nfc);
void kern_nfc_close(kern_nfc_t *nfc);
int kern_nfc_power(kern_nfc_t *nfc, int on);
int kern_nfc_wake(kern_nfc_t *nfc, int state);
int kern_nfc_write(kern_nfc_t *nfc, const uint8_t *data, size_t len);
/* Returns bytes read, 0 on timeout, -1 on error. */
int kern_nfc_read(kern_nfc_t *nfc, uint8_t *buf, size_t cap, int timeout_ms);

/* The APDU transport the card layer uses.  Implemented in nfc_nci.c. */
typedef struct {
  int (*transmit)(void *ctx, const uint8_t *apdu, size_t apdu_len,
                  uint8_t *resp, size_t *resp_len, uint8_t *sw1,
                  uint8_t *sw2);
} kern_nfc_transport_t;

int kern_nfc_bring_up(kern_nfc_t *nfc);
int kern_nfc_transceive(kern_nfc_t *nfc, const uint8_t *apdu, size_t apdu_len,
                        uint8_t *resp, size_t *resp_len, uint8_t *sw1,
                        uint8_t *sw2);

#endif
