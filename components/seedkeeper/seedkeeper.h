#ifndef KERN_SEEDKEEPER_H
#define KERN_SEEDKEEPER_H

#include <stddef.h>
#include <stdint.h>

#include "../nfc/nfc.h"

/* SeedKeeper/Satochip applet layer.  The transport is the NFC component. */
typedef struct {
  kern_nfc_transport_t transport;
  void *ctx;
  int selected;
  int pin_verified;
} kern_seedkeeper_t;

int seedkeeper_select(kern_seedkeeper_t *sk);
int seedkeeper_verify_pin(kern_seedkeeper_t *sk, const char *pin);
int seedkeeper_get_status(kern_seedkeeper_t *sk);
int seedkeeper_list_headers(kern_seedkeeper_t *sk, void *out, size_t *out_len);
int seedkeeper_import_secret(kern_seedkeeper_t *sk, uint8_t type,
                             uint8_t subtype, const char *label,
                             const uint8_t *secret, size_t secret_len);
int seedkeeper_export_secret(kern_seedkeeper_t *sk, uint16_t id,
                             uint8_t *out, size_t *out_len);
int seedkeeper_reset_secret(kern_seedkeeper_t *sk, uint16_t id);

#endif
