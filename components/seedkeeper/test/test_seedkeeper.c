/*
 * Host tests for the SeedKeeper plaintext applet layer.
 *
 * A mock NFC transport records every APDU and replays scripted card
 * responses, so the wire format is checked without a card.  The expected
 * fingerprints come from mbedTLS SHA-256, the same primitive the card uses.
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

typedef struct {
  uint8_t data[512];
  size_t len;
  uint8_t sw1;
  uint8_t sw2;
} mock_response_t;

typedef struct {
  mock_response_t resp[32];
  int resp_count;
  int resp_index;
  int fail_at;
  uint8_t apdu[32][300];
  size_t apdu_len[32];
  int apdu_count;
} mock_t;

static int mock_transmit(void *ctx, const uint8_t *apdu, size_t apdu_len,
                         uint8_t *resp, size_t *resp_len, uint8_t *sw1,
                         uint8_t *sw2) {
  mock_t *m = (mock_t *)ctx;
  mock_response_t *r;

  if (m->apdu_count < 32) {
    if (apdu_len > sizeof(m->apdu[0]))
      return -1;
    memcpy(m->apdu[m->apdu_count], apdu, apdu_len);
    m->apdu_len[m->apdu_count] = apdu_len;
    m->apdu_count++;
  }

  if (m->fail_at == m->resp_index) {
    m->resp_index++;
    return -1;
  }
  if (m->resp_index >= m->resp_count)
    return -1;
  r = &m->resp[m->resp_index++];
  if (r->len > *resp_len)
    return -1;
  memcpy(resp, r->data, r->len);
  *resp_len = r->len;
  *sw1 = r->sw1;
  *sw2 = r->sw2;
  return 0;
}

static void mock_reset(mock_t *m) {
  memset(m, 0, sizeof(*m));
  m->fail_at = -1;
}

static void mock_add(mock_t *m, const uint8_t *data, size_t len, uint16_t sw) {
  mock_response_t *r = &m->resp[m->resp_count++];
  if (len > sizeof(r->data))
    abort();
  if (len)
    memcpy(r->data, data, len);
  r->len = len;
  r->sw1 = (uint8_t)(sw >> 8);
  r->sw2 = (uint8_t)(sw & 0xFF);
}

static void mock_ok(mock_t *m) { mock_add(m, NULL, 0, 0x9000); }

static void mock_sw(mock_t *m, uint16_t sw) { mock_add(m, NULL, 0, sw); }

static void mock_data(mock_t *m, const uint8_t *data, size_t len, uint16_t sw) {
  mock_add(m, data, len, sw);
}

static void make_header(uint8_t *h, uint16_t id, uint8_t type, uint8_t subtype,
                        const char *label) {
  size_t n = strlen(label);
  memset(h, 0, 15 + n);
  h[0] = (uint8_t)(id >> 8);
  h[1] = (uint8_t)(id & 0xFF);
  h[2] = type;
  h[3] = 0x01;
  h[4] = SEEDKEEPER_EXPORT_PLAINTEXT_ALLOWED;
  h[8] = 0xAA;
  h[9] = 0xBB;
  h[10] = 0xCC;
  h[11] = 0xDD;
  h[12] = subtype;
  h[14] = (uint8_t)n;
  memcpy(h + 15, label, n);
}

static void test_select_and_pin(void) {
  mock_t m;
  kern_seedkeeper_t sk;
  kern_seedkeeper_card_status_t st;
  char text[64];
  static const uint8_t select_apdu[] = {0x00, 0xA4, 0x04, 0x00, 0x0A,
                                        0x53, 0x65, 0x65, 0x64, 0x4B,
                                        0x65, 0x65, 0x70, 0x65, 0x72};
  static const uint8_t pin_apdu[] = {0xB0, 0x42, 0x00, 0x00, 0x04,
                                     0x31, 0x32, 0x33, 0x34};

  mock_reset(&m);
  seedkeeper_init(&sk, (kern_nfc_transport_t){mock_transmit}, &m);

  uint8_t fci[14] = {0x6F, 0x0B, 0x84, 0x0A, 'S', 'e', 'e', 'd',
                     'K', 'e', 'e', 'p', 'e', 'r'};
  mock_data(&m, fci, sizeof(fci), 0x9000);
  CHECK(seedkeeper_select(&sk) == 0);
  CHECK(m.apdu_count == 1);
  CHECK(m.apdu_len[0] == sizeof(select_apdu));
  CHECK(memcmp(m.apdu[0], select_apdu, sizeof(select_apdu)) == 0);
  CHECK(sk.selected == 1);

  CHECK(seedkeeper_verify_pin(&sk, "12") == SEEDKEEPER_ERR_ARGUMENT);
  CHECK(sk.pin_verified == 0);

  mock_ok(&m);
  CHECK(seedkeeper_verify_pin(&sk, "1234") == 0);
  CHECK(sk.pin_verified == 1);
  CHECK(strcmp(sk.pin, "1234") == 0);
  CHECK(m.apdu_count == 2);
  CHECK(m.apdu_len[1] == sizeof(pin_apdu));
  CHECK(memcmp(m.apdu[1], pin_apdu, sizeof(pin_apdu)) == 0);

  mock_sw(&m, 0x63C2);
  CHECK(seedkeeper_verify_pin(&sk, "9999") == SEEDKEEPER_ERR_STATUS);
  CHECK(sk.last_sw == 0x63C2);
  CHECK(sk.pin_verified == 0);
  CHECK(seedkeeper_describe_status(sk.last_sw, text, sizeof(text)) > 0);
  CHECK(strcmp(text, "PIN is incorrect, 2 tries left") == 0);

  /* Card status: versions, tries left, setup done, secure channel. */
  {
    uint8_t data[12] = {0x01, 0x02, 0x03, 0x04, 0x05, 0,
                        0,    0,    0,    0,    0x01, 0x00};
    mock_reset(&m);
    seedkeeper_init(&sk, (kern_nfc_transport_t){mock_transmit}, &m);
    mock_data(&m, data, sizeof(data), 0x9000);
    CHECK(seedkeeper_card_status(&sk, &st) == 0);
    CHECK(st.protocol_major == 1 && st.protocol_minor == 2);
    CHECK(st.applet_major == 3 && st.applet_minor == 4);
    CHECK(st.pin_tries_left == 5);
    CHECK(st.setup_done == 1);
    CHECK(st.needs_secure_channel == 0);
    CHECK(sk.needs_secure_channel == 0);

    data[10] = 0x00;
    data[11] = 0x01;
    data[4] = 0x00;
    mock_reset(&m);
    seedkeeper_init(&sk, (kern_nfc_transport_t){mock_transmit}, &m);
    mock_data(&m, data, sizeof(data), 0x9000);
    CHECK(seedkeeper_card_status(&sk, &st) == 0);
    CHECK(st.setup_done == 0);
    CHECK(st.needs_secure_channel == 1);
    CHECK(sk.needs_secure_channel == 1);
  }

  /* Old applet without the trailing fields. */
  {
    uint8_t data[4] = {0x00, 0x0B, 0x00, 0x07};
    kern_seedkeeper_card_status_t old;
    mock_reset(&m);
    seedkeeper_init(&sk, (kern_nfc_transport_t){mock_transmit}, &m);
    mock_data(&m, data, sizeof(data), 0x9000);
    CHECK(seedkeeper_card_status(&sk, &old) == 0);
    CHECK(old.pin_tries_left == -1);
    CHECK(old.setup_done == 1);
    CHECK(old.needs_secure_channel == 0);
  }
}

static void test_memory_status(void) {
  mock_t m;
  kern_seedkeeper_t sk;
  kern_seedkeeper_memory_status_t mem;
  uint8_t data[] = {0x00, 0x03, 0x01, 0x00, 0x00, 0x40, 0x00, 0x05, 0x00, 0x02};

  mock_reset(&m);
  seedkeeper_init(&sk, (kern_nfc_transport_t){mock_transmit}, &m);
  mock_data(&m, data, sizeof(data), 0x9000);
  CHECK(seedkeeper_memory_status(&sk, &mem) == 0);
  CHECK(mem.nb_secrets == 3);
  CHECK(mem.total_memory == 256);
  CHECK(mem.free_memory == 64);
  CHECK(mem.nb_logs_total == 5);
  CHECK(mem.nb_logs_avail == 2);

  mock_reset(&m);
  seedkeeper_init(&sk, (kern_nfc_transport_t){mock_transmit}, &m);
  mock_data(&m, data, 6, 0x9000);
  CHECK(seedkeeper_memory_status(&sk, &mem) == 0);
  CHECK(mem.nb_logs_total == -1 && mem.nb_logs_avail == -1);
}

static void test_list_headers(void) {
  mock_t m;
  kern_seedkeeper_t sk;
  kern_seedkeeper_header_t headers[4];
  size_t count;
  uint8_t h1[32];
  uint8_t h2[32];
  uint8_t bad[24];

  make_header(h1, 0x0102, SEEDKEEPER_TYPE_BIP39_MNEMONIC,
              SEEDKEEPER_SUBTYPE_BIP39, "wallet");
  /* A label another implementation wrote that is not UTF-8. */
  make_header(h2, 0x0007, SEEDKEEPER_TYPE_DATA, 0x00, "raw");
  memcpy(bad, h2, 15 + 3);
  bad[15] = 0xFF;
  bad[16] = 0x00;
  bad[17] = 0x41;

  mock_reset(&m);
  seedkeeper_init(&sk, (kern_nfc_transport_t){mock_transmit}, &m);
  mock_data(&m, h1, 15 + 6, 0x9000);
  mock_data(&m, bad, 15 + 3, 0x9000);
  mock_sw(&m, SEEDKEEPER_SW_NO_MORE_SECRETS);

  count = 4;
  CHECK(seedkeeper_list_headers(&sk, headers, &count) == 0);
  CHECK(count == 2);
  CHECK(headers[0].id == 0x0102);
  CHECK(headers[0].type == SEEDKEEPER_TYPE_BIP39_MNEMONIC);
  CHECK(headers[0].subtype == SEEDKEEPER_SUBTYPE_BIP39);
  CHECK(strcmp(headers[0].label, "wallet") == 0);
  CHECK(headers[0].label_valid == 1);
  CHECK(headers[0].fingerprint[0] == 0xAA);
  CHECK(headers[1].id == 0x0007);
  CHECK(strcmp(headers[1].label, "\xFF\x00\x41") == 0);
  CHECK(headers[1].label_valid == 0);

  CHECK(m.apdu_count == 3);
  CHECK(m.apdu[0][1] == SEEDKEEPER_INS_LIST_HEADERS);
  CHECK(m.apdu[0][3] == 0x01);
  CHECK(m.apdu[1][3] == 0x02);

  /* A one-entry array must report the required size, not truncate. */
  mock_reset(&m);
  seedkeeper_init(&sk, (kern_nfc_transport_t){mock_transmit}, &m);
  mock_data(&m, h1, 15 + 6, 0x9000);
  mock_data(&m, bad, 15 + 3, 0x9000);
  mock_sw(&m, SEEDKEEPER_SW_NO_MORE_SECRETS);
  count = 1;
  CHECK(seedkeeper_list_headers(&sk, headers, &count) == SEEDKEEPER_ERR_SPACE);
  CHECK(count == 2);
}

static void test_import(void) {
  mock_t m;
  kern_seedkeeper_t sk;
  uint8_t secret[200];
  uint8_t digest[32];
  uint8_t resp[6];
  uint16_t id = 0;
  uint8_t fp[SEEDKEEPER_FINGERPRINT_SIZE];
  size_t i;
  size_t payload_len;

  for (i = 0; i < sizeof(secret); i++)
    secret[i] = (uint8_t)i;
  mbedtls_sha256(secret, sizeof(secret), digest, 0);

  mock_reset(&m);
  seedkeeper_init(&sk, (kern_nfc_transport_t){mock_transmit}, &m);
  mock_ok(&m);
  mock_ok(&m);
  resp[0] = 0x12;
  resp[1] = 0x34;
  memcpy(resp + 2, digest, 4);
  mock_data(&m, resp, sizeof(resp), 0x9000);

  CHECK(seedkeeper_import_secret(&sk, SEEDKEEPER_TYPE_BIP39_MNEMONIC,
                                 SEEDKEEPER_SUBTYPE_BIP39, "test", secret,
                                 sizeof(secret), &id, fp) == 0);
  CHECK(id == 0x1234);
  CHECK(memcmp(fp, digest, 4) == 0);
  CHECK(m.apdu_count == 3);

  /* First APDU: 15 + label(4) + size(2) = 21 bytes, padded size 208. */
  payload_len = 13 + 4 + 2; /* 13 fixed + label + size */
  CHECK(m.apdu[0][0] == SEEDKEEPER_CLA);
  CHECK(m.apdu[0][1] == SEEDKEEPER_INS_IMPORT_SECRET);
  CHECK(m.apdu[0][2] == 0x01 && m.apdu[0][3] == 0x01);
  CHECK(m.apdu[0][4] == payload_len);
  CHECK(m.apdu[0][5] == SEEDKEEPER_TYPE_BIP39_MNEMONIC);
  CHECK(m.apdu[0][6] == 0x00);
  CHECK(m.apdu[0][7] == SEEDKEEPER_EXPORT_PLAINTEXT_ALLOWED);
  CHECK(m.apdu[0][15] == SEEDKEEPER_SUBTYPE_BIP39);
  CHECK(m.apdu[0][17] == 4);
  CHECK(memcmp(m.apdu[0] + 18, "test", 4) == 0);
  CHECK(m.apdu[0][22] == 0x00 && m.apdu[0][23] == 208);

  /* Chunk: 0x0080 + 128 bytes, then 72 remaining bytes. */
  CHECK(m.apdu[1][2] == 0x01 && m.apdu[1][3] == 0x02);
  CHECK(m.apdu[1][4] == 130);
  CHECK(m.apdu[1][5] == 0x00 && m.apdu[1][6] == 128);
  CHECK(memcmp(m.apdu[1] + 7, secret, 128) == 0);
  CHECK(m.apdu[2][2] == 0x01 && m.apdu[2][3] == 0x03);
  CHECK(m.apdu[2][4] == 74);
  CHECK(m.apdu[2][5] == 0x00 && m.apdu[2][6] == 72);
  CHECK(memcmp(m.apdu[2] + 7, secret + 128, 72) == 0);

  /* A card that files it under another fingerprint is an error. */
  mock_reset(&m);
  seedkeeper_init(&sk, (kern_nfc_transport_t){mock_transmit}, &m);
  mock_ok(&m);
  resp[2] ^= 0xFF;
  mock_data(&m, resp, sizeof(resp), 0x9000);
  CHECK(seedkeeper_import_secret(&sk, SEEDKEEPER_TYPE_DATA, 0x00, "x", secret,
                                 32, NULL, NULL) ==
        SEEDKEEPER_ERR_FINGERPRINT);
}

static void test_export(void) {
  mock_t m;
  kern_seedkeeper_t sk;
  uint8_t secret[200];
  uint8_t data[512];
  uint8_t out[200];
  uint8_t h[32];
  size_t out_len;
  size_t i;

  for (i = 0; i < sizeof(secret); i++)
    secret[i] = (uint8_t)(0xA0 + (i & 0x0F));
  make_header(h, 0x0042, SEEDKEEPER_TYPE_BIP39_MNEMONIC,
              SEEDKEEPER_SUBTYPE_BIP39, "seed");

  mock_reset(&m);
  seedkeeper_init(&sk, (kern_nfc_transport_t){mock_transmit}, &m);
  mock_data(&m, h, 15 + 4, 0x9000);
  data[0] = 0;
  data[1] = 128;
  memcpy(data + 2, secret, 128);
  mock_data(&m, data, 130, 0x9000);
  data[0] = 0;
  data[1] = 72;
  memcpy(data + 2, secret + 128, 72);
  memset(data + 74, 0xEE, 64); /* the card's signature on the last chunk */
  mock_data(&m, data, 74 + 64, 0x9000);

  out_len = sizeof(out);
  CHECK(seedkeeper_export_secret(&sk, 0x0042, out, &out_len) == 0);
  CHECK(out_len == 200);
  CHECK(memcmp(out, secret, 200) == 0);
  CHECK(m.apdu_count == 3);
  CHECK(m.apdu[0][1] == SEEDKEEPER_INS_EXPORT_SECRET);
  CHECK(m.apdu[0][2] == 0x01 && m.apdu[0][3] == 0x01);
  CHECK(m.apdu[0][5] == 0x00 && m.apdu[0][6] == 0x42);
  CHECK(m.apdu[1][3] == 0x02);

  /* Too small an output buffer must not overflow or lose the length. */
  mock_reset(&m);
  seedkeeper_init(&sk, (kern_nfc_transport_t){mock_transmit}, &m);
  mock_data(&m, h, 15 + 4, 0x9000);
  data[0] = 0;
  data[1] = 72;
  memcpy(data + 2, secret + 128, 72);
  mock_data(&m, data, 74, 0x9000);
  out_len = 10;
  CHECK(seedkeeper_export_secret(&sk, 0x0042, out, &out_len) ==
        SEEDKEEPER_ERR_SPACE);
}

static void test_reset(void) {
  mock_t m;
  kern_seedkeeper_t sk;

  mock_reset(&m);
  seedkeeper_init(&sk, (kern_nfc_transport_t){mock_transmit}, &m);
  mock_ok(&m);
  CHECK(seedkeeper_reset_secret(&sk, 0x0102) == 0);
  CHECK(m.apdu_count == 1);
  CHECK(m.apdu[0][1] == SEEDKEEPER_INS_RESET_SECRET);
  CHECK(m.apdu[0][4] == 2);
  CHECK(m.apdu[0][5] == 0x01 && m.apdu[0][6] == 0x02);

  mock_reset(&m);
  seedkeeper_init(&sk, (kern_nfc_transport_t){mock_transmit}, &m);
  mock_sw(&m, SEEDKEEPER_SW_SECRET_NOT_FOUND);
  CHECK(seedkeeper_reset_secret(&sk, 7) == 1);

  mock_reset(&m);
  seedkeeper_init(&sk, (kern_nfc_transport_t){mock_transmit}, &m);
  mock_sw(&m, 0x9C03);
  CHECK(seedkeeper_reset_secret(&sk, 7) == SEEDKEEPER_ERR_STATUS);
  CHECK(sk.last_sw == 0x9C03);
}

static void test_bip39(void) {
  uint8_t seed[64];
  uint8_t entropy[32];
  uint8_t payload[256];
  uint8_t got_entropy[32];
  char pass[64];
  size_t payload_len;
  size_t got_len;
  size_t pass_len;
  size_t i;

  for (i = 0; i < sizeof(seed); i++)
    seed[i] = (uint8_t)(i * 3);
  for (i = 0; i < sizeof(entropy); i++)
    entropy[i] = (uint8_t)(0xFF - i);

  CHECK(seedkeeper_pack_bip39(seed, sizeof(seed), entropy, sizeof(entropy),
                              "pass phrase", payload, sizeof(payload),
                              &payload_len) == 0);
  CHECK(payload[0] == 64);
  CHECK(payload[65] == SEEDKEEPER_WORDLIST_ENGLISH);
  CHECK(payload[66] == 32);
  CHECK(seedkeeper_unpack_bip39(payload, payload_len, got_entropy,
                                sizeof(got_entropy), &got_len, pass,
                                sizeof(pass), &pass_len) == 0);
  CHECK(got_len == 32 && memcmp(got_entropy, entropy, 32) == 0);
  CHECK(pass_len == 11 && strcmp(pass, "pass phrase") == 0);

  CHECK(seedkeeper_pack_bip39(seed, sizeof(seed), entropy, sizeof(entropy),
                              "x", payload, 10, &payload_len) ==
        SEEDKEEPER_ERR_SPACE);
  CHECK(seedkeeper_unpack_bip39(payload, 5, got_entropy, sizeof(got_entropy),
                                &got_len, pass, sizeof(pass), &pass_len) ==
        SEEDKEEPER_ERR_PARSE);

  CHECK(seedkeeper_unpack_bip39(payload, payload_len, got_entropy, 4, &got_len,
                                pass, sizeof(pass), &pass_len) ==
        SEEDKEEPER_ERR_SPACE);

  payload[65] = 0x09; /* not the English wordlist */
  CHECK(seedkeeper_unpack_bip39(payload, payload_len, got_entropy,
                                sizeof(got_entropy), &got_len, pass,
                                sizeof(pass), &pass_len) ==
        SEEDKEEPER_ERR_PARSE);
}

static void test_errors(void) {
  mock_t m;
  kern_seedkeeper_t sk;

  /* No applet on the card. */
  mock_reset(&m);
  seedkeeper_init(&sk, (kern_nfc_transport_t){mock_transmit}, &m);
  mock_sw(&m, SEEDKEEPER_SW_APPLET_NOT_FOUND);
  CHECK(seedkeeper_select(&sk) == SEEDKEEPER_ERR_STATUS);
  CHECK(sk.last_sw == SEEDKEEPER_SW_APPLET_NOT_FOUND);
  CHECK(sk.selected == 0);

  /* One lost command is retried by refresh(), a status word is not. */
  mock_reset(&m);
  seedkeeper_init(&sk, (kern_nfc_transport_t){mock_transmit}, &m);
  m.fail_at = 0;
  mock_ok(&m);
  mock_ok(&m);
  CHECK(seedkeeper_refresh(&sk) == 0);

  mock_reset(&m);
  seedkeeper_init(&sk, (kern_nfc_transport_t){mock_transmit}, &m);
  mock_sw(&m, 0x6A82);
  mock_ok(&m);
  CHECK(seedkeeper_refresh(&sk) == SEEDKEEPER_ERR_STATUS);

  CHECK(seedkeeper_describe_status(0x9000, NULL, 0) == 0);
  CHECK(seedkeeper_describe_status(0x1234, NULL, 0) == 0);
}

int main(void) {
  test_select_and_pin();
  test_memory_status();
  test_list_headers();
  test_import();
  test_export();
  test_reset();
  test_bip39();
  test_errors();

  if (failures) {
    fprintf(stderr, "seedkeeper: %d checks failed\n", failures);
    return 1;
  }
  printf("seedkeeper: checks passed\n");
  return 0;
}
