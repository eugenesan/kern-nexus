/*
 * Host tests for the SeedKeeper flows the kern pages run.
 *
 * A small emulation of the SeedKeeper applet answers the APDUs, so the
 * sequences the pages send are checked end to end without a card:
 *
 *   new seed:   A0 generate -> A2 export -> A1 import -> A5 free
 *   load seed:  A6 list -> A2 export
 *   backup:     A1 import
 *   delete:     A5 reset -> A6 list
 *
 * The emulator follows the applet's rules that matter here: a generated
 * secret is a plain [size | bytes] blob, an import is padded to a multiple
 * of 16 bytes, the fingerprint is the first four bytes of the secret's
 * SHA-256, and an export strips the padding and signs the last chunk.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/sha256.h>

#include "../seedkeeper.h"

static int failures;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      failures++;                                                              \
    }                                                                          \
  } while (0)

#define MAX_SECRETS 8
#define SECRET_CAP 600
#define CHUNK 128

typedef struct {
  uint16_t id;
  uint8_t type;
  uint8_t subtype;
  uint8_t export_rights;
  char label[65];
  uint8_t data[SECRET_CAP];
  size_t len;
} applet_secret_t;

typedef struct {
  applet_secret_t secrets[MAX_SECRETS];
  int count;
  int pin_valid;
  int setup_done;
  uint16_t next_id;
  int list_index;
  /* import in progress */
  int import_open;
  uint16_t import_id;
  uint8_t import_header[64];
  size_t import_header_len;
  size_t import_padded;
  uint8_t import_data[SECRET_CAP];
  size_t import_len;
  /* export in progress */
  int export_open;
  uint16_t export_id;
  size_t export_offset;
  size_t export_total;
  int generated;
} applet_t;

static void applet_reset(applet_t *a) {
  memset(a, 0, sizeof(*a));
  a->next_id = 0x0001;
  a->setup_done = 1;
}

static applet_secret_t *applet_find(applet_t *a, uint16_t id) {
  for (int i = 0; i < a->count; i++)
    if (a->secrets[i].id == id)
      return &a->secrets[i];
  return NULL;
}

static applet_secret_t *applet_add(applet_t *a) {
  if (a->count >= MAX_SECRETS)
    return NULL;
  return &a->secrets[a->count++];
}

static void put_sw(uint8_t *sw1, uint8_t *sw2, uint16_t sw) {
  *sw1 = (uint8_t)(sw >> 8);
  *sw2 = (uint8_t)(sw & 0xFF);
}

static size_t fingerprint(const uint8_t *data, size_t len, uint8_t out[4]) {
  uint8_t digest[32];
  mbedtls_sha256(data, len, digest, 0);
  memcpy(out, digest, 4);
  return 4;
}

/* Writes a 15-byte header prefix plus the label. */
static size_t write_header(uint8_t *out, const applet_secret_t *s) {
  size_t label_len = strlen(s->label);
  memset(out, 0, 15 + label_len);
  out[0] = (uint8_t)(s->id >> 8);
  out[1] = (uint8_t)(s->id & 0xFF);
  out[2] = s->type;
  out[3] = 0x01; /* origin: on card */
  out[4] = s->export_rights;
  fingerprint(s->data, s->len, out + 8);
  out[12] = s->subtype;
  out[14] = (uint8_t)label_len;
  memcpy(out + 15, s->label, label_len);
  return 15 + label_len;
}

/* The size is in P1, so a separate entry point is clearer. */
static size_t applet_generate_seed(applet_t *a, uint8_t seed_size,
                                   const uint8_t *data, size_t len,
                                   uint8_t *resp) {
  applet_secret_t *s;
  size_t label_len;

  if (a->count >= MAX_SECRETS)
    return 0;
  s = applet_add(a);
  if (!s)
    return 0;
  memset(s, 0, sizeof(*s));
  s->id = a->next_id++;
  s->type = SEEDKEEPER_TYPE_MASTERSEED;
  s->subtype = 0;
  s->export_rights = SEEDKEEPER_EXPORT_PLAINTEXT_ALLOWED;
  label_len = data[0];
  if (label_len > 64 || label_len + 1 > len)
    return 0;
  memcpy(s->label, data + 1, label_len);
  s->label[label_len] = '\0';
  /* [size | bytes], with the bytes from a counter. */
  s->len = 1 + seed_size;
  s->data[0] = seed_size;
  for (size_t i = 0; i < seed_size; i++)
    s->data[1 + i] = (uint8_t)(0x40 + (a->generated++ & 0x3F));
  resp[0] = (uint8_t)(s->id >> 8);
  resp[1] = (uint8_t)(s->id & 0xFF);
  fingerprint(s->data, s->len, resp + 2);
  return 6;
}

static int applet_transmit(void *ctx, const uint8_t *apdu, size_t apdu_len,
                           uint8_t *resp, size_t *resp_len, uint8_t *sw1,
                           uint8_t *sw2) {
  applet_t *a = (applet_t *)ctx;
  uint8_t ins = apdu[1];
  uint8_t p1 = apdu[2];
  uint8_t p2 = apdu[3];
  const uint8_t *data = apdu + 5;
  size_t len = apdu[4];
  size_t out = 0;

  (void)apdu_len;
  put_sw(sw1, sw2, 0x9000);

  if (ins == 0xA4) { /* SELECT */
    a->pin_valid = 0;
    resp[0] = 0x6F;
    resp[1] = 0x0A;
    resp[2] = 0x84;
    resp[3] = 0x08;
    memcpy(resp + 4, data, 8);
    out = 12;
    goto done;
  }
  if (ins == 0x3C) { /* GET_STATUS: no PIN needed */
    resp[0] = 0x00;
    resp[1] = 0x02;
    resp[2] = 0x00;
    resp[3] = 0x01;
    resp[4] = 5;
    resp[5] = 0;
    resp[6] = 0;
    resp[7] = 0;
    resp[8] = 0;
    resp[9] = 1;
    resp[10] = a->setup_done;
    resp[11] = 0;
    resp[12] = 0;
    out = 13;
    goto done;
  }
  if (ins == 0x42) { /* VERIFY_PIN: the one command allowed without it */
    if (len >= 4 && len <= 16 && memcmp(data, "123456", len) == 0) {
      a->pin_valid = 1;
    } else {
      a->pin_valid = 0;
      put_sw(sw1, sw2, 0x63C2);
    }
    goto done;
  }
  if (!a->pin_valid) {
    put_sw(sw1, sw2, 0x9C06); /* PIN required */
    goto done;
  }
  switch (ins) {
  case 0xA0: /* GENERATE_MASTERSEED */
    out = applet_generate_seed(a, p1, data, len, resp);
    if (!out)
      put_sw(sw1, sw2, 0x9C01);
    break;

  case 0xA1: { /* IMPORT_SECRET */
    applet_secret_t *s;
    if (p2 == 0x01) {
      size_t label_len = data[12];
      size_t header = 13 + label_len;
      if (len < header + 2) {
        put_sw(sw1, sw2, 0x6A80);
        break;
      }
      a->import_open = 1;
      a->import_len = 0;
      a->import_padded = ((size_t)data[header] << 8) | data[header + 1];
      memcpy(a->import_header, data, 13);
      memcpy(a->import_header + 13, data + 13, label_len);
      a->import_header_len = header;
      s = applet_add(a);
      if (!s) {
        put_sw(sw1, sw2, 0x9C01);
        break;
      }
      memset(s, 0, sizeof(*s));
      s->id = a->next_id++;
      s->type = a->import_header[0];
      s->export_rights = a->import_header[2];
      s->subtype = a->import_header[10];
      memcpy(s->label, a->import_header + 13, label_len);
      s->label[label_len] = '\0';
      /* Keep the new secret aside until the final chunk. */
      a->import_id = s->id;
      break;
    }
    if (!a->import_open) {
      put_sw(sw1, sw2, 0x9CFF);
      break;
    }
    if (p2 == 0x02) {
      size_t chunk = ((size_t)data[0] << 8) | data[1];
      if (chunk > len - 2 || a->import_len + chunk > SECRET_CAP) {
        put_sw(sw1, sw2, 0x6A80);
        break;
      }
      memcpy(a->import_data + a->import_len, data + 2, chunk);
      a->import_len += chunk;
      break;
    }
    if (p2 == 0x03) {
      size_t chunk = ((size_t)data[0] << 8) | data[1];
      applet_secret_t *s;
      if (chunk > len - 2 || a->import_len + chunk > SECRET_CAP) {
        put_sw(sw1, sw2, 0x6A80);
        break;
      }
      memcpy(a->import_data + a->import_len, data + 2, chunk);
      a->import_len += chunk;
      s = applet_find(a, a->import_id);
      if (!s) {
        put_sw(sw1, sw2, 0x9C08);
        break;
      }
      memcpy(s->data, a->import_data, a->import_len);
      s->len = a->import_len;
      a->import_open = 0;
      resp[0] = (uint8_t)(s->id >> 8);
      resp[1] = (uint8_t)(s->id & 0xFF);
      fingerprint(s->data, s->len, resp + 2);
      out = 6;
      break;
    }
    put_sw(sw1, sw2, 0x9C10);
    break;
  }

  case 0xA2: { /* EXPORT_SECRET */
    applet_secret_t *s;
    if (p2 == 0x01) {
      uint16_t id = (uint16_t)((data[0] << 8) | data[1]);
      s = applet_find(a, id);
      if (!s) {
        put_sw(sw1, sw2, 0x9C08);
        break;
      }
      if (s->export_rights != SEEDKEEPER_EXPORT_PLAINTEXT_ALLOWED) {
        put_sw(sw1, sw2, 0x9C31);
        break;
      }
      a->export_open = 1;
      a->export_id = id;
      a->export_offset = 0;
      a->export_total = s->len;
      out = write_header(resp, s);
      break;
    }
    if (!a->export_open) {
      put_sw(sw1, sw2, 0x9CFF);
      break;
    }
    s = applet_find(a, a->export_id);
    if (!s) {
      put_sw(sw1, sw2, 0x9C08);
      break;
    }
    if (a->export_total - a->export_offset > CHUNK) {
      size_t chunk = CHUNK;
      resp[0] = (uint8_t)(chunk >> 8);
      resp[1] = (uint8_t)(chunk & 0xFF);
      memcpy(resp + 2, s->data + a->export_offset, chunk);
      a->export_offset += chunk;
      out = 2 + chunk;
      break;
    }
    {
      size_t chunk = a->export_total - a->export_offset;
      resp[0] = (uint8_t)(chunk >> 8);
      resp[1] = (uint8_t)(chunk & 0xFF);
      memcpy(resp + 2, s->data + a->export_offset, chunk);
      /* The applet appends a signature to the last chunk. */
      memset(resp + 2 + chunk, 0xEE, 64);
      out = 2 + chunk + 2 + 64;
      a->export_open = 0;
    }
    break;
  }

  case 0xA5: { /* RESET_SECRET */
    uint16_t id = (uint16_t)((data[0] << 8) | data[1]);
    for (int i = 0; i < a->count; i++) {
      if (a->secrets[i].id == id) {
        memmove(&a->secrets[i], &a->secrets[i + 1],
                sizeof(applet_secret_t) * (size_t)(a->count - i - 1));
        a->count--;
        goto done;
      }
    }
    put_sw(sw1, sw2, 0x9C08);
    break;
  }

  case 0xA6: { /* LIST_HEADERS */
    if (p2 == 0x01)
      a->list_index = 0;
    if (a->list_index >= a->count) {
      put_sw(sw1, sw2, 0x9C12);
      break;
    }
    out = write_header(resp, &a->secrets[a->list_index]);
    a->list_index++;
    break;
  }

  case 0xA7: { /* SEEDKEEPER_STATUS */
    size_t free = SECRET_CAP * MAX_SECRETS;
    for (int i = 0; i < a->count; i++)
      free -= 15 + strlen(a->secrets[i].label) + a->secrets[i].len;
    resp[0] = (uint8_t)(a->count >> 8);
    resp[1] = (uint8_t)(a->count & 0xFF);
    resp[2] = (uint8_t)((SECRET_CAP * MAX_SECRETS) >> 8);
    resp[3] = (uint8_t)((SECRET_CAP * MAX_SECRETS) & 0xFF);
    resp[4] = (uint8_t)(free >> 8);
    resp[5] = (uint8_t)(free & 0xFF);
    out = 6;
    break;
  }

  default:
    put_sw(sw1, sw2, 0x6D00);
    break;
  }

done:
  *resp_len = out;
  return 0;
}

/* --- the flows ---------------------------------------------------------- */

/* Mirrors step_generate() in seedkeeper_new_seed.c. */
static void test_new_seed_flow(void) {
  applet_t a;
  kern_seedkeeper_t sk;
  uint8_t secret[128];
  size_t secret_len;
  uint8_t entropy[32];
  size_t entropy_len = 0;
  uint8_t payload[256];
  size_t payload_len = 0;
  uint8_t digest[32];
  uint16_t id = 0, bip39_id = 0;
  uint8_t fp[SEEDKEEPER_FINGERPRINT_SIZE];

  applet_reset(&a);
  seedkeeper_init(&sk, (kern_nfc_transport_t){applet_transmit}, &a);

  CHECK(seedkeeper_refresh(&sk) == 0);
  CHECK(seedkeeper_verify_pin(&sk, "123456") == 0);

  /* A0: the card generates a 32-byte secret. */
  CHECK(seedkeeper_generate_masterseed(&sk, 32, "kern-entropy",
                                       SEEDKEEPER_EXPORT_PLAINTEXT_ALLOWED, &id,
                                       fp) == 0);
  CHECK(id != 0);
  CHECK(a.count == 1);
  CHECK(strcmp(a.secrets[0].label, "kern-entropy") == 0);
  CHECK(a.secrets[0].len == 33); /* [size | 32 bytes] */
  CHECK(a.secrets[0].data[0] == 32);

  /* A2: export it and take the bytes after the size byte. */
  secret_len = sizeof(secret);
  CHECK(seedkeeper_export_secret(&sk, id, secret, &secret_len) == 0);
  CHECK(secret_len == 33);
  CHECK(seedkeeper_plain_secret_bytes(secret, secret_len, entropy,
                                      sizeof(entropy), &entropy_len) == 0);
  CHECK(entropy_len == 32);
  CHECK(memcmp(entropy, a.secrets[0].data + 1, 32) == 0);

  /* The device turns the entropy into a mnemonic and its seed. */
  CHECK(seedkeeper_pack_bip39((const uint8_t *)"seed", 4, entropy, 32, "",
                              payload, sizeof(payload), &payload_len) == 0);

  /* A1: store the BIP39 Masterseed. */
  CHECK(seedkeeper_import_secret(&sk, SEEDKEEPER_TYPE_MASTERSEED,
                                 SEEDKEEPER_SUBTYPE_BIP39, "kern", payload,
                                 payload_len, &bip39_id, fp) == 0);
  CHECK(bip39_id != id);
  CHECK(a.count == 2);
  CHECK(a.secrets[1].len == payload_len);
  CHECK(memcmp(a.secrets[1].data, payload, payload_len) == 0);
  mbedtls_sha256(payload, payload_len, digest, 0);
  CHECK(memcmp(fp, digest, 4) == 0);

  /* A5: free the raw entropy secret, leaving one clean secret. */
  CHECK(seedkeeper_reset_secret(&sk, id) == 0);
  CHECK(a.count == 1);
  CHECK(a.secrets[0].id == bip39_id);

  /* The BIP39 secret reads back exactly as written. */
  secret_len = sizeof(secret);
  CHECK(seedkeeper_export_secret(&sk, bip39_id, secret, &secret_len) == 0);
  CHECK(secret_len == payload_len);
  CHECK(memcmp(secret, payload, payload_len) == 0);
}

/* Mirrors the backup step in seedkeeper_backup.c and the load path. */
static void test_backup_flow(void) {
  applet_t a;
  kern_seedkeeper_t sk;
  uint8_t payload[256];
  size_t payload_len = 0;
  uint8_t seed[64];
  uint8_t entropy[32];
  uint8_t out[256];
  size_t out_len;
  uint8_t fp[SEEDKEEPER_FINGERPRINT_SIZE];
  uint16_t id = 0;
  const char *passphrase = "correct horse";

  applet_reset(&a);
  seedkeeper_init(&sk, (kern_nfc_transport_t){applet_transmit}, &a);
  CHECK(seedkeeper_refresh(&sk) == 0);
  CHECK(seedkeeper_verify_pin(&sk, "123456") == 0);

  for (size_t i = 0; i < sizeof(seed); i++)
    seed[i] = (uint8_t)i;
  for (size_t i = 0; i < sizeof(entropy); i++)
    entropy[i] = (uint8_t)(0xF0 - i);

  CHECK(seedkeeper_pack_bip39(seed, sizeof(seed), entropy, sizeof(entropy),
                              passphrase, payload, sizeof(payload),
                              &payload_len) == 0);
  CHECK(seedkeeper_import_secret(&sk, SEEDKEEPER_TYPE_MASTERSEED,
                                 SEEDKEEPER_SUBTYPE_BIP39, "cold", payload,
                                 payload_len, &id, fp) == 0);
  CHECK(a.count == 1);

  /* The load path reads the BIP39 fields back, passphrase included. */
  out_len = sizeof(out);
  CHECK(seedkeeper_export_secret(&sk, id, out, &out_len) == 0);
  CHECK(out_len == payload_len);
  {
    uint8_t got_entropy[32];
    size_t got_len = 0;
    char got_pass[64];
    size_t got_pass_len = 0;
    uint8_t got_seed[64];
    size_t got_seed_len = 0;
    CHECK(seedkeeper_unpack_bip39(out, out_len, got_entropy,
                                  sizeof(got_entropy), &got_len, got_pass,
                                  sizeof(got_pass), &got_pass_len) == 0);
    CHECK(got_len == sizeof(entropy));
    CHECK(memcmp(got_entropy, entropy, sizeof(entropy)) == 0);
    CHECK(got_pass_len == strlen(passphrase));
    CHECK(strcmp(got_pass, passphrase) == 0);
    CHECK(seedkeeper_bip39_master_seed(out, out_len, got_seed,
                                       sizeof(got_seed), &got_seed_len) == 0);
    CHECK(got_seed_len == sizeof(seed));
    CHECK(memcmp(got_seed, seed, sizeof(seed)) == 0);
  }
}

/* List, delete and the card status, as the hub page does them. */
static void test_manage_flow(void) {
  applet_t a;
  kern_seedkeeper_t sk;
  kern_seedkeeper_header_t headers[MAX_SECRETS];
  size_t count;
  kern_seedkeeper_card_status_t status;
  kern_seedkeeper_memory_status_t mem;
  uint16_t id = 0;
  uint8_t fp[SEEDKEEPER_FINGERPRINT_SIZE];
  uint8_t blob[4] = {0x00, 0xAA, 0xBB, 0xCC};

  applet_reset(&a);
  seedkeeper_init(&sk, (kern_nfc_transport_t){applet_transmit}, &a);
  CHECK(seedkeeper_refresh(&sk) == 0);
  CHECK(seedkeeper_verify_pin(&sk, "123456") == 0);

  CHECK(seedkeeper_import_secret(&sk, SEEDKEEPER_TYPE_DATA, 0, "one", blob,
                                 sizeof(blob), &id, fp) == 0);
  CHECK(seedkeeper_import_secret(&sk, SEEDKEEPER_TYPE_DATA, 0, "two", blob,
                                 sizeof(blob), &id, fp) == 0);
  CHECK(a.count == 2);

  count = MAX_SECRETS;
  CHECK(seedkeeper_list_headers(&sk, headers, &count) == 0);
  CHECK(count == 2);
  CHECK(strcmp(headers[0].label, "one") == 0);
  CHECK(strcmp(headers[1].label, "two") == 0);
  CHECK(headers[0].type == SEEDKEEPER_TYPE_DATA);

  CHECK(seedkeeper_card_status(&sk, &status) == 0);
  CHECK(status.setup_done == 1);
  CHECK(status.pin_tries_left == 5);

  CHECK(seedkeeper_memory_status(&sk, &mem) == 0);
  CHECK(mem.nb_secrets == 2);

  /* Delete the first one and walk the list again. */
  CHECK(seedkeeper_reset_secret(&sk, headers[0].id) == 0);
  count = MAX_SECRETS;
  CHECK(seedkeeper_list_headers(&sk, headers, &count) == 0);
  CHECK(count == 1);
  CHECK(strcmp(headers[0].label, "two") == 0);

  /* Deleting a secret that is gone reports it, it does not fail. */
  CHECK(seedkeeper_reset_secret(&sk, 0x7777) == 1);
}

int main(void) {
  test_new_seed_flow();
  test_backup_flow();
  test_manage_flow();

  if (failures) {
    fprintf(stderr, "seedkeeper flows: %d checks failed\n", failures);
    return 1;
  }
  printf("seedkeeper flows: checks passed\n");
  return 0;
}
