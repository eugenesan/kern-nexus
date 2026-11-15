/*
 * Host-side tests for the NCI framing/validation helpers in nfc_nci.c.
 *
 * No hardware is touched: synthetic response buffers are fed straight to the
 * same helpers the driver uses.  Build and run with `make test` here.
 */

#include <stdio.h>
#include <string.h>

#include "../nfc.h"
#include "../nfc_nci_internal.h"

static int checks;
static int failures;

#define CHECK(cond)                                                           \
  do {                                                                        \
    checks++;                                                                 \
    if (!(cond)) {                                                            \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      failures++;                                                             \
    }                                                                         \
  } while (0)

#define CHECK_BYTES(got, got_len, want)                                       \
  do {                                                                        \
    CHECK((got_len) == sizeof(want));                                         \
    CHECK((got_len) == sizeof(want) &&                                        \
          memcmp((got), (want), sizeof(want)) == 0);                          \
  } while (0)

static void test_build_packet(void) {
  uint8_t out[KERN_NFC_MAX_PACKET];
  uint8_t payload[256];
  size_t n;

  /* CORE_RESET with reset type 0x01, exactly as nfc.py sends it. */
  {
    static const uint8_t want[] = {0x10, 0x20, 0x00, 0x01, 0x01};
    n = kern_nfc_nci_build_packet(0x20, 0x00, (const uint8_t[]){0x01}, 1, out,
                                  sizeof(out));
    CHECK_BYTES(out, n, want);
  }

  /* CORE_INIT has an empty payload: 10 20 01 00. */
  {
    static const uint8_t want[] = {0x10, 0x20, 0x01, 0x00};
    n = kern_nfc_nci_build_packet(0x20, 0x01, NULL, 0, out, sizeof(out));
    CHECK_BYTES(out, n, want);
  }

  /* An ISO-DEP APDU is MT_DATA (0x00), OID 0x00, then the APDU bytes. */
  {
    static const uint8_t apdu[] = {0x00, 0xa4, 0x04, 0x00};
    static const uint8_t want[] = {0x10, 0x00, 0x00, 0x04,
                                   0x00, 0xa4, 0x04, 0x00};
    n = kern_nfc_nci_build_packet(0x00, 0x00, apdu, sizeof(apdu), out,
                                  sizeof(out));
    CHECK_BYTES(out, n, want);
  }

  /* A 57-byte SET_CONFIG payload gets length byte 0x39 (the LPTD trace). */
  memset(payload, 0xa5, sizeof(payload));
  n = kern_nfc_nci_build_packet(0x20, 0x02, payload, 57, out, sizeof(out));
  CHECK(n == 61);
  CHECK(out[0] == 0x10);
  CHECK(out[1] == 0x20);
  CHECK(out[2] == 0x02);
  CHECK(out[3] == 0x39);

  /* A full 255-byte payload is the largest single NCI packet. */
  n = kern_nfc_nci_build_packet(0x00, 0x00, payload, 255, out, sizeof(out));
  CHECK(n == 259);
  CHECK(out[3] == 0xff);
  CHECK(out[4] == 0xa5);
  CHECK(out[258] == 0xa5);
  CHECK(kern_nfc_nci_validate_packet(out, n) == 0);

  /* One byte more does not fit in the one-byte length field. */
  CHECK(kern_nfc_nci_build_packet(0x20, 0x00, payload, 256, out, sizeof(out)) ==
        0);
  /* Buffer too small. */
  CHECK(kern_nfc_nci_build_packet(0x20, 0x00, payload, 10, out, 13) == 0);
  /* Missing payload pointer. */
  CHECK(kern_nfc_nci_build_packet(0x20, 0x00, NULL, 1, out, sizeof(out)) == 0);
  /* Missing output pointer. */
  CHECK(kern_nfc_nci_build_packet(0x20, 0x00, NULL, 0, NULL, 0) == 0);
}

static void test_validate_packet(void) {
  static const uint8_t good[] = {0x10, 0x20, 0x00, 0x01, 0x01};
  static const uint8_t bad_type[] = {0x11, 0x20, 0x00, 0x01, 0x01};
  static const uint8_t short_payload[] = {0x10, 0x20, 0x00, 0x02, 0x01};
  static const uint8_t extra_byte[] = {0x10, 0x20, 0x00, 0x01, 0x01, 0x00};
  static const uint8_t empty_payload[] = {0x10, 0x20, 0x01, 0x00};

  CHECK(kern_nfc_nci_validate_packet(good, sizeof(good)) == 0);
  CHECK(kern_nfc_nci_validate_packet(empty_payload, sizeof(empty_payload)) ==
        0);
  CHECK(kern_nfc_nci_validate_packet(NULL, sizeof(good)) == -1);
  CHECK(kern_nfc_nci_validate_packet(good, 3) == -1);
  CHECK(kern_nfc_nci_validate_packet(bad_type, sizeof(bad_type)) == -1);
  CHECK(kern_nfc_nci_validate_packet(short_payload, sizeof(short_payload)) ==
        -1);
  CHECK(kern_nfc_nci_validate_packet(extra_byte, sizeof(extra_byte)) == -1);
}

static void test_is_tag(void) {
  /* This chip's two-byte OID 0x06 tag notification. */
  static const uint8_t tag_oid6[] = {0x10, 0x60, 0x06, 0x02, 0x01, 0x00};
  /* A real deactivate notification carries three bytes. */
  static const uint8_t deactivate[] = {0x10, 0x60, 0x06, 0x03, 0x01, 0x00,
                                       0x00};
  /* RF_DISCOVER_NTF, payload length >= 2. */
  static const uint8_t discover[] = {0x10, 0x60, 0x03, 0x02, 0x01, 0x04};
  static const uint8_t discover_short[] = {0x10, 0x60, 0x03, 0x01, 0x01};
  /* RF_INTF_ACTIVATED_NTF. */
  static const uint8_t activated[] = {0x10, 0x60, 0x05, 0x01, 0x01};
  /* A response and a data packet are not tag notifications. */
  static const uint8_t response[] = {0x10, 0x40, 0x05, 0x01, 0x00};
  static const uint8_t data[] = {0x10, 0x00, 0x06, 0x02, 0x01, 0x00};

  CHECK(kern_nfc_nci_is_tag(tag_oid6, sizeof(tag_oid6)) == 1);
  CHECK(kern_nfc_nci_is_tag(deactivate, sizeof(deactivate)) == 0);
  CHECK(kern_nfc_nci_is_tag(discover, sizeof(discover)) == 1);
  CHECK(kern_nfc_nci_is_tag(discover_short, sizeof(discover_short)) == 0);
  CHECK(kern_nfc_nci_is_tag(activated, sizeof(activated)) == 1);
  CHECK(kern_nfc_nci_is_tag(response, sizeof(response)) == 0);
  CHECK(kern_nfc_nci_is_tag(data, sizeof(data)) == 0);
  CHECK(kern_nfc_nci_is_tag(tag_oid6, 3) == 0);
  CHECK(kern_nfc_nci_is_tag(NULL, 0) == 0);
}

static void test_select_params(void) {
  uint8_t out[3];

  /* Default is discovery id 1, ISO-DEP protocol 4, ISO-DEP interface 2. */
  {
    static const uint8_t want[] = {0x01, 0x04, 0x02};
    memset(out, 0, sizeof(out));
    CHECK(kern_nfc_nci_select_params(NULL, 0, out) == 0);
    CHECK_BYTES(out, sizeof(out), want);
  }

  /* An activated notification keeps the default. */
  {
    static const uint8_t ntf[] = {0x10, 0x60, 0x05, 0x03, 0x01, 0x04, 0x02};
    static const uint8_t want[] = {0x01, 0x04, 0x02};
    CHECK(kern_nfc_nci_select_params(ntf, sizeof(ntf), out) == 0);
    CHECK_BYTES(out, sizeof(out), want);
  }

  /* A discover notification names the target: id, protocol, interface. */
  {
    static const uint8_t ntf[] = {0x10, 0x60, 0x03, 0x02, 0x02, 0x04};
    static const uint8_t want[] = {0x02, 0x04, 0x02};
    CHECK(kern_nfc_nci_select_params(ntf, sizeof(ntf), out) == 0);
    CHECK_BYTES(out, sizeof(out), want);
  }
  {
    static const uint8_t ntf[] = {0x10, 0x60, 0x03, 0x02, 0x03, 0x05};
    static const uint8_t want[] = {0x03, 0x05, 0x01};
    CHECK(kern_nfc_nci_select_params(ntf, sizeof(ntf), out) == 0);
    CHECK_BYTES(out, sizeof(out), want);
  }

  /* Too short to carry the pair: default. */
  {
    static const uint8_t ntf[] = {0x10, 0x60, 0x03, 0x01, 0x02};
    static const uint8_t want[] = {0x01, 0x04, 0x02};
    CHECK(kern_nfc_nci_select_params(ntf, sizeof(ntf), out) == 0);
    CHECK_BYTES(out, sizeof(out), want);
  }

  CHECK(kern_nfc_nci_select_params(NULL, 0, NULL) == -1);
}

static void test_parse_data(void) {
  const uint8_t *data = NULL;
  size_t data_len = 0;
  uint8_t sw1 = 0;
  uint8_t sw2 = 0;

  /* Data plus SW1/SW2. */
  {
    static const uint8_t pkt[] = {0x10, 0x00, 0x00, 0x04, 0xde, 0xad,
                                  0x90, 0x00};
    CHECK(kern_nfc_nci_parse_data(pkt, sizeof(pkt), &data, &data_len, &sw1,
                                   &sw2) == 0);
    CHECK(data_len == 2);
    CHECK(data && data[0] == 0xde && data[1] == 0xad);
    CHECK(sw1 == 0x90);
    CHECK(sw2 == 0x00);
  }

  /* No data, only a status word. */
  {
    static const uint8_t pkt[] = {0x10, 0x00, 0x00, 0x02, 0x6a, 0x82};
    CHECK(kern_nfc_nci_parse_data(pkt, sizeof(pkt), &data, &data_len, &sw1,
                                   &sw2) == 0);
    CHECK(data_len == 0);
    CHECK(sw1 == 0x6a);
    CHECK(sw2 == 0x82);
  }

  /* A body shorter than SW1/SW2 is an error. */
  {
    static const uint8_t pkt[] = {0x10, 0x00, 0x00, 0x01, 0x90};
    CHECK(kern_nfc_nci_parse_data(pkt, sizeof(pkt), &data, &data_len, &sw1,
                                   &sw2) == -1);
  }

  /* Not MT_DATA. */
  {
    static const uint8_t pkt[] = {0x10, 0x40, 0x00, 0x04, 0xde, 0xad,
                                  0x90, 0x00};
    CHECK(kern_nfc_nci_parse_data(pkt, sizeof(pkt), &data, &data_len, &sw1,
                                   &sw2) == -1);
  }

  /* Inconsistent length field. */
  {
    static const uint8_t pkt[] = {0x10, 0x00, 0x00, 0x05, 0xde, 0xad,
                                  0x90, 0x00};
    CHECK(kern_nfc_nci_parse_data(pkt, sizeof(pkt), &data, &data_len, &sw1,
                                   &sw2) == -1);
  }
}

static void test_response_status(void) {
  uint8_t status = 0xff;

  {
    static const uint8_t rsp[] = {0x10, 0x40, 0x00, 0x03, 0x00, 0x02, 0x01};
    CHECK(kern_nfc_nci_response_status(rsp, sizeof(rsp), &status) == 0);
    CHECK(status == 0x00);
  }
  {
    static const uint8_t rsp[] = {0x10, 0x40, 0x02, 0x01, 0x06};
    CHECK(kern_nfc_nci_response_status(rsp, sizeof(rsp), &status) == 0);
    CHECK(status == 0x06);
  }
  /* No status byte. */
  {
    static const uint8_t rsp[] = {0x10, 0x40, 0x00, 0x00};
    CHECK(kern_nfc_nci_response_status(rsp, sizeof(rsp), &status) == -1);
  }
  /* Malformed. */
  {
    static const uint8_t rsp[] = {0x10, 0x40, 0x00, 0x02, 0x00};
    CHECK(kern_nfc_nci_response_status(rsp, sizeof(rsp), &status) == -1);
  }
  CHECK(kern_nfc_nci_response_status(NULL, 0, &status) == -1);
}

static void test_terminal_error(void) {
  static const uint8_t iface_error[] = {0x10, 0x60, 0x08, 0x01, 0xb1};
  static const uint8_t generic_error[] = {0x10, 0x60, 0x07, 0x01, 0x01};
  static const uint8_t rf_deactivate[] = {0x10, 0x61, 0x06, 0x01, 0x00};
  static const uint8_t conn_credits[] = {0x10, 0x60, 0x06, 0x01, 0x01};
  static const uint8_t data[] = {0x10, 0x00, 0x00, 0x02, 0x90, 0x00};
  static const uint8_t response[] = {0x10, 0x40, 0x08, 0x01, 0xb1};

  CHECK(kern_nfc_nci_is_terminal_error(iface_error, sizeof(iface_error)) == 1);
  CHECK(kern_nfc_nci_is_terminal_error(generic_error,
                                       sizeof(generic_error)) == 1);
  CHECK(kern_nfc_nci_is_terminal_error(rf_deactivate,
                                       sizeof(rf_deactivate)) == 1);
  CHECK(kern_nfc_nci_is_terminal_error(conn_credits, sizeof(conn_credits)) ==
        0);
  CHECK(kern_nfc_nci_is_terminal_error(data, sizeof(data)) == 0);
  CHECK(kern_nfc_nci_is_terminal_error(response, sizeof(response)) == 0);
  CHECK(kern_nfc_nci_is_terminal_error(NULL, 0) == 0);
}

int main(void) {
  test_build_packet();
  test_validate_packet();
  test_is_tag();
  test_select_params();
  test_parse_data();
  test_response_status();
  test_terminal_error();

  if (failures != 0) {
    fprintf(stderr, "%d/%d checks failed\n", failures, checks);
    return 1;
  }
  printf("nfc_nci: %d checks passed\n", checks);
  return 0;
}
