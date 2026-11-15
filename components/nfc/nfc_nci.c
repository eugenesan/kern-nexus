/*
 * NCI protocol for the BCM2079x on the Nexus 5, on top of nfc_bcm2079x.c.
 *
 * Source mapping: every constant, payload byte, command order, status check
 * and timeout below is taken from krux-nexus/krux/src/nfc.py (class
 * IsoDepTransport and its module constants), which replays the vendor HAL
 * trace.  The wire framing is documented in krux-nexus/docs/nfc-transport.md.
 * The SET_CONFIG/RF payloads are copied byte-for-byte from nfc.py; the
 * _Static_assert()s at the end of the constants verify their lengths against
 * the lengths in the captured trace.
 *
 * The public API is nfc.h.  The BCM2079x is one chip, so the NCI state
 * (queued notifications, whether a tag is activated) is process-global and
 * re-bound whenever the file descriptor changes.  The functions are not
 * thread-safe; drive one card from one thread.
 */

#define _POSIX_C_SOURCE 200809L

#include "nfc.h"
#include "nfc_nci_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/*
 * Bring-up trace.  Compile with -DKERN_NFC_TRACE for a standalone probe; the
 * signer build leaves it out.  The BCM2079x can wedge a command, so knowing
 * which step was last reached is the difference between a useful and a
 * silent failure.
 */
#ifdef KERN_NFC_TRACE
#include <stdio.h>
#define NFC_TRACE(...)                                                        \
  do {                                                                        \
    fprintf(stderr, __VA_ARGS__);                                             \
    fflush(stderr);                                                           \
  } while (0)
#else
#define NFC_TRACE(...)                                                        \
  do {                                                                        \
  } while (0)
#endif

/* --- framing (nfc.py lines 40-86) -------------------------------------- */

#define HCIT_NFC 0x10

#define MT_DATA 0x00
#define MT_CMD 0x20
#define MT_RSP 0x40
#define MT_NTF 0x60

#define MT_MASK 0xE0

#define GID_CORE 0x00
#define GID_RF 0x01
#define GID_PROP 0x0F

#define CORE_RESET 0x00
#define CORE_INIT 0x01
#define CORE_SET_CONFIG 0x02
#define CORE_SET_CLK 0x03
#define CORE_CONN_CREDITS 0x06
#define CORE_GENERIC_ERROR 0x07
#define CORE_INTERFACE_ERROR 0x08

#define RF_DISCOVER_MAP 0x00
#define RF_SET_ROUTING 0x01
#define RF_DISCOVER 0x03
#define RF_DISCOVER_SELECT 0x04
#define RF_INTF_ACTIVATED_NTF 0x05
#define RF_DEACTIVATE 0x06
#define RF_TAG_NTF 0x06

#define PROP_GET_BUILD_INFO 0x04
#define PROP_GET_PATCH_VERSION 0x2D

#define PROP_CMD_06 0x06
#define PROP_CMD_0C 0x0C

/* --- the vendor's configuration payloads (nfc.py lines 104-145) -------- */

static const uint8_t SET_CONFIG_LPTD[] = {
    0x01, 0xb9, 0x36, 0x01, 0x00, 0xff, 0xff, 0x08, 0x00, 0x00, 0x00, 0xa0,
    0x0f, 0x40, 0x00, 0x00, 0x12, 0x02, 0x10, 0x00, 0x00, 0x00, 0x2d, 0x0b,
    0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0x00, 0xd0, 0x07, 0x00, 0x00, 0x08, 0x07, 0x00,
    0x00, 0xc8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t SET_CONFIG_START_UP[] = {
    0x0c, 0xcb, 0x01, 0x01, 0xa5, 0x01, 0x01, 0xca, 0x17, 0x00, 0x00, 0x00,
    0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x00, 0xe0,
    0x67, 0x35, 0x00, 0x14, 0x01, 0x00, 0x00, 0x10, 0xb5, 0x03, 0x01, 0x02,
    0xff, 0x80, 0x01, 0x01, 0xc9, 0x03, 0x03, 0x0f, 0xab, 0x5b, 0x01, 0x00,
    0xb2, 0x04, 0xe8, 0x03, 0x00, 0x00, 0xcf, 0x02, 0x02, 0x08, 0xb1, 0x06,
    0x00, 0x20, 0x00, 0x00, 0x00, 0x12, 0xc2, 0x02, 0x00, 0xc8, 0xc8, 0x1e,
    0x06, 0x3f, 0x00, 0x0a, 0x03, 0x30, 0x00, 0x04, 0x24, 0x00, 0x1c, 0x00,
    0x75, 0x00, 0x77, 0x00, 0x76, 0x00, 0x1c, 0x00, 0x03, 0x00, 0x0a, 0x00,
    0x4c, 0x01, 0x00, 0x00, 0x40, 0x04,
};

static const uint8_t SET_CONFIG_POLL[] = {
    0x01, 0xb7, 0x03, 0x02, 0x00, 0x01,
};

static const uint8_t SET_CONFIG_RF[] = {
    0x02, 0x51, 0x08, 0x01, 0xfe, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0x58, 0x01, 0x70,
};

static const uint8_t SET_CONFIG_19[] = {
    0x01, 0x19, 0x01, 0x01,
};

static const uint8_t SET_CONFIG_80[] = {
    0x01, 0x80, 0x01, 0x01,
};

static const uint8_t SET_CONFIG_POLL_PARAMS[] = {
    0x05, 0x30, 0x01, 0x04, 0x31, 0x01, 0x00, 0x32,
    0x01, 0x20, 0x38, 0x01, 0x00, 0x50, 0x01, 0x00,
};

static const uint8_t SET_CONFIG_00[] = {
    0x01, 0x00, 0x02, 0xf4, 0x01,
};

static const uint8_t SET_CLK[] = {
    0x01, 0x52,
};

static const uint8_t PROP_0C[] = {0x00};
static const uint8_t PROP_06[] = {0x01};

static const uint8_t RF_ROUTING[] = {
    0x00, 0x04, 0x01, 0x03, 0x00, 0x01, 0x03, 0x01, 0x03, 0x00, 0x01,
    0x04, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x03, 0x00, 0x01, 0x02,
};

static const uint8_t DISCOVER_MAP_PARAMS[] = {
    0x02, 0x04, 0x03, 0x02, 0x05, 0x03, 0x03,
};

static const uint8_t DISCOVER_PARAMS[] = {
    0x07, 0x00, 0x01, 0x01, 0x01, 0x02, 0x01,
    0x80, 0x01, 0x06, 0x01, 0x74, 0x01, 0x77, 0x01,
};

static const uint8_t SELECT_PARAMS[] = {
    0x01, 0x04, 0x02,
};

/* --- timeouts (nfc.py lines 151-166) ------------------------------------ */

#define WAKE_WHEN_POLLING 1
#define DISCOVER_INTERVAL_MS 1000
#define POLL_WINDOW_MS 8000
#define POWER_CYCLE_MS 10
#define SETTLE_MS 300
#define ACTIVATE_SETTLE_MS 200
#define TAG_TIMEOUT_MS 30000
#define COMMAND_TIMEOUT_MS 2000
#define ANNOUNCE_TIMEOUT_MS 5000
#define ACTIVATE_NTF_TIMEOUT_MS 3000
#define APDU_TIMEOUT_MS 5000
#define DISCOVER_READ_MS 500

/* --- internal state ---------------------------------------------------- */

#define NTF_QUEUE_DEPTH 16

typedef struct {
  int fd;
  int connected;
  uint8_t q_pkt[NTF_QUEUE_DEPTH][KERN_NFC_MAX_PACKET];
  size_t q_len[NTF_QUEUE_DEPTH];
  unsigned q_head;
  unsigned q_count;
} nci_state_t;

/* The one BCM2079x.  nci_bind() clears everything when the fd changes. */
static nci_state_t g_nci = {.fd = -1};
static volatile int g_cancel;

/* --- small utilities --------------------------------------------------- */

static int64_t nci_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void nci_sleep_ms(int ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000;
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

static int nci_timeout_left(int64_t deadline) {
  int64_t left = deadline - nci_now_ms();
  if (left <= 0)
    return 0;
  if (left > INT_MAX)
    return INT_MAX;
  return (int)left;
}

static void nci_bind(nci_state_t *st, int fd) {
  if (st->fd == fd)
    return;
  memset(st, 0, sizeof(*st));
  st->fd = fd;
}

static void nci_queue_clear(nci_state_t *st) {
  st->q_head = 0;
  st->q_count = 0;
}

static void nci_queue_push(nci_state_t *st, const uint8_t *pkt, size_t len) {
  unsigned slot;

  if (len == 0 || len > KERN_NFC_MAX_PACKET)
    return;
  if (st->q_count == NTF_QUEUE_DEPTH) {
    /* Drop the oldest notification rather than the newest. */
    st->q_head = (st->q_head + 1) % NTF_QUEUE_DEPTH;
    st->q_count--;
  }
  slot = (st->q_head + st->q_count) % NTF_QUEUE_DEPTH;
  memcpy(st->q_pkt[slot], pkt, len);
  st->q_len[slot] = len;
  st->q_count++;
}

/* 1 if a packet was popped, 0 if empty, -1 if the caller's buffer is small. */
static int nci_queue_pop(nci_state_t *st, uint8_t *pkt, size_t cap,
                         size_t *len) {
  unsigned slot;
  size_t n;

  if (st->q_count == 0)
    return 0;
  slot = st->q_head;
  n = st->q_len[slot];
  if (!pkt || cap < n)
    return -1;
  memcpy(pkt, st->q_pkt[slot], n);
  *len = n;
  st->q_head = (slot + 1) % NTF_QUEUE_DEPTH;
  st->q_count--;
  return 1;
}

/* --- framing/validation helpers (also used by the host test) ----------- */

size_t kern_nfc_nci_build_packet(uint8_t mt_gid, uint8_t oid,
                                 const uint8_t *payload, size_t payload_len,
                                 uint8_t *out, size_t cap) {
  if (!out || payload_len > 0xFF || cap < payload_len + 4)
    return 0;
  if (payload_len != 0 && !payload)
    return 0;

  out[0] = HCIT_NFC;
  out[1] = mt_gid;
  out[2] = oid;
  out[3] = (uint8_t)payload_len;
  if (payload_len != 0)
    memcpy(out + 4, payload, payload_len);
  return payload_len + 4;
}

int kern_nfc_nci_validate_packet(const uint8_t *pkt, size_t len) {
  if (!pkt || len < 4)
    return -1;
  if (pkt[0] != HCIT_NFC)
    return -1;
  /* The driver returns one whole packet per read; the length must agree. */
  if ((size_t)pkt[3] + 4 != len)
    return -1;
  return 0;
}

int kern_nfc_nci_is_tag(const uint8_t *pkt, size_t len) {
  if (!pkt || len < 4)
    return 0;
  if ((pkt[1] & MT_MASK) != MT_NTF)
    return 0;
  /* RF_DISCOVER_NTF: a target was found and expects RF_DISCOVER_SELECT. */
  if (pkt[2] == RF_DISCOVER)
    return pkt[3] >= 2;
  if (pkt[2] == RF_INTF_ACTIVATED_NTF)
    return 1;
  /*
   * This chip's "tag is there" notification is OID 6 with a two-byte
   * payload; a real RF_DEACTIVATE notification carries three bytes.
   */
  return pkt[2] == RF_TAG_NTF && pkt[3] == 2;
}

int kern_nfc_nci_select_params(const uint8_t *ntf, size_t len, uint8_t out[3]) {
  if (!out)
    return -1;
  memcpy(out, SELECT_PARAMS, sizeof(SELECT_PARAMS));
  if (ntf && len >= 6 && ntf[2] == RF_DISCOVER) {
    out[0] = ntf[4]; /* discovery id */
    out[1] = ntf[5]; /* protocol */
    out[2] = (ntf[5] == 0x04) ? 0x02 : 0x01; /* ISO-DEP vs NFC-DEP */
  }
  return 0;
}

int kern_nfc_nci_parse_data(const uint8_t *pkt, size_t len,
                            const uint8_t **data, size_t *data_len,
                            uint8_t *sw1, uint8_t *sw2) {
  size_t body_len;

  if (kern_nfc_nci_validate_packet(pkt, len) != 0)
    return -1;
  if (pkt[1] != MT_DATA)
    return -1;
  body_len = pkt[3];
  if (body_len < 2)
    return -1;

  if (data)
    *data = pkt + 4;
  if (data_len)
    *data_len = body_len - 2;
  if (sw1)
    *sw1 = pkt[4 + body_len - 2];
  if (sw2)
    *sw2 = pkt[4 + body_len - 1];
  return 0;
}

int kern_nfc_nci_response_status(const uint8_t *pkt, size_t len,
                                 uint8_t *status) {
  if (kern_nfc_nci_validate_packet(pkt, len) != 0)
    return -1;
  if (len < 5 || !status)
    return -1;
  *status = pkt[4];
  return 0;
}

int kern_nfc_nci_is_terminal_error(const uint8_t *pkt, size_t len) {
  uint8_t gid;

  if (!pkt || len < 4)
    return 0;
  if ((pkt[1] & MT_MASK) != MT_NTF)
    return 0;
  gid = pkt[1] & 0x0F;
  if (gid == GID_CORE &&
      (pkt[2] == CORE_INTERFACE_ERROR || pkt[2] == CORE_GENERIC_ERROR))
    return 1;
  if (gid == GID_RF && pkt[2] == RF_DEACTIVATE)
    return 1;
  return 0;
}

/* --- packet I/O -------------------------------------------------------- */

/*
 * One whole NCI packet.  Returns 1 with *len set, 0 on timeout or a malformed
 * packet (nfc.py ignores short packets), and -1 on a transport error.
 */
static int nci_read_packet(kern_nfc_t *nfc, uint8_t *pkt, size_t cap,
                           int timeout_ms, size_t *len) {
  int n = kern_nfc_read(nfc, pkt, cap, timeout_ms);

  if (n < 0)
    return -1;
  if (n == 0)
    return 0;
  if (kern_nfc_nci_validate_packet(pkt, (size_t)n) != 0)
    return 0;
  if (len)
    *len = (size_t)n;
  return 1;
}

/* Wait for MT_RSP|gid / oid, queueing every other packet as a notification. */
static int nci_wait_for(kern_nfc_t *nfc, nci_state_t *st, uint8_t want,
                        uint8_t oid, int timeout_ms, uint8_t *out, size_t cap,
                        size_t *out_len) {
  int64_t deadline = nci_now_ms() + timeout_ms;

  for (;;) {
    uint8_t pkt[KERN_NFC_MAX_PACKET];
    size_t len = 0;
    int left = nci_timeout_left(deadline);
    int r;

    if (left == 0)
      return -1;
    r = nci_read_packet(nfc, pkt, sizeof(pkt), left, &len);
    if (r < 0)
      return -1;
    if (r == 0)
      continue;

    if (pkt[1] == want && pkt[2] == oid) {
      if (!out) {
        if (out_len)
          *out_len = len;
        return 0;
      }
      if (cap < len)
        return -1;
      memcpy(out, pkt, len);
      if (out_len)
        *out_len = len;
      return 0;
    }
    nci_queue_push(st, pkt, len);
  }
}

/* Send one command and wait for its response packet. */
static int nci_send(kern_nfc_t *nfc, nci_state_t *st, uint8_t gid, uint8_t oid,
                    const uint8_t *payload, size_t payload_len, uint8_t *rsp,
                    size_t cap, size_t *rsp_len) {
  uint8_t pkt[KERN_NFC_MAX_PACKET];
  size_t len = kern_nfc_nci_build_packet(MT_CMD | gid, oid, payload,
                                         payload_len, pkt, sizeof(pkt));
  int written;
  int r;

  if (len == 0)
    return -1;
  NFC_TRACE("nfc: -> gid=%x oid=%x len=%u\n", gid, oid, (unsigned)len);
  written = kern_nfc_write(nfc, pkt, len);
  if (written != (int)len) {
    NFC_TRACE("nfc: write failed (%d)\n", written);
    return -1;
  }
  r = nci_wait_for(nfc, st, MT_RSP | gid, oid, COMMAND_TIMEOUT_MS, rsp, cap,
                   rsp_len);
  NFC_TRACE("nfc: <- gid=%x oid=%x rc=%d\n", gid, oid, r);
  return r;
}

/* Send a command whose status byte must be NCI_STATUS_OK (0x00). */
static int nci_send_ok(kern_nfc_t *nfc, nci_state_t *st, uint8_t gid,
                       uint8_t oid, const uint8_t *payload,
                       size_t payload_len) {
  uint8_t rsp[KERN_NFC_MAX_PACKET];
  size_t rsp_len = 0;
  uint8_t status;

  if (nci_send(nfc, st, gid, oid, payload, payload_len, rsp, sizeof(rsp),
               &rsp_len) != 0)
    return -1;
  if (kern_nfc_nci_response_status(rsp, rsp_len, &status) != 0)
    return -1;
  return status == 0x00 ? 0 : -1;
}

/* Send a command and ignore its status byte, but require a response. */
static int nci_send_any(kern_nfc_t *nfc, nci_state_t *st, uint8_t gid,
                        uint8_t oid, const uint8_t *payload,
                        size_t payload_len) {
  uint8_t rsp[KERN_NFC_MAX_PACKET];
  size_t rsp_len = 0;

  return nci_send(nfc, st, gid, oid, payload, payload_len, rsp, sizeof(rsp),
                  &rsp_len);
}

/* --- bring-up ---------------------------------------------------------- */

/* Power cycle and replay the vendor's whole command sequence, no polling. */
static int nci_chip_setup(kern_nfc_t *nfc, nci_state_t *st) {
  uint8_t pkt[KERN_NFC_MAX_PACKET];
  size_t len = 0;
  int r;

  /* WAKE 0 -> POWER 0 -> POWER 1 -> WAKE 0, then assert wake. */
  NFC_TRACE("nfc: power cycle\n");
  if (kern_nfc_wake(nfc, 0) != 0)
    return -1;
  if (kern_nfc_power(nfc, 0) != 0)
    return -1;
  nci_sleep_ms(POWER_CYCLE_MS);
  if (kern_nfc_power(nfc, 1) != 0)
    return -1;
  if (kern_nfc_wake(nfc, 0) != 0)
    return -1;
  if (kern_nfc_wake(nfc, WAKE_WHEN_POLLING) != 0)
    return -1;

  st->connected = 0;
  nci_queue_clear(st);

  /* The chip announces itself, then needs a moment for field calibration. */
  r = nci_read_packet(nfc, pkt, sizeof(pkt), ANNOUNCE_TIMEOUT_MS, &len);
  if (r <= 0)
    return -1;
  nci_queue_push(st, pkt, len);
  nci_sleep_ms(SETTLE_MS);

  /* The HAL asks; the answers are version strings we do not use yet. */
  if (nci_send_any(nfc, st, GID_PROP, PROP_GET_BUILD_INFO, NULL, 0) != 0)
    return -1;
  if (nci_send_any(nfc, st, GID_PROP, PROP_GET_PATCH_VERSION, NULL, 0) != 0)
    return -1;

  if (nci_send_ok(nfc, st, GID_CORE, CORE_RESET, (const uint8_t[]){0x01}, 1) !=
      0)
    return -1;
  if (nci_send_ok(nfc, st, GID_CORE, CORE_INIT, NULL, 0) != 0)
    return -1;

  /* LPTD, start-up/antenna, poll: exactly the vendor's order. */
  if (nci_send_ok(nfc, st, GID_CORE, CORE_SET_CONFIG, SET_CONFIG_LPTD,
                  sizeof(SET_CONFIG_LPTD)) != 0)
    return -1;
  if (nci_send_ok(nfc, st, GID_CORE, CORE_SET_CONFIG, SET_CONFIG_START_UP,
                  sizeof(SET_CONFIG_START_UP)) != 0)
    return -1;
  if (nci_send_ok(nfc, st, GID_CORE, CORE_SET_CONFIG, SET_CONFIG_POLL,
                  sizeof(SET_CONFIG_POLL)) != 0)
    return -1;

  if (nci_send_any(nfc, st, GID_PROP, PROP_CMD_06, PROP_06, sizeof(PROP_06)) !=
      0)
    return -1;

  /* The map does not start polling; RF_DISCOVER below does. */
  if (nci_send_ok(nfc, st, GID_RF, RF_DISCOVER_MAP, DISCOVER_MAP_PARAMS,
                  sizeof(DISCOVER_MAP_PARAMS)) != 0)
    return -1;

  if (nci_send_any(nfc, st, GID_CORE, CORE_SET_CONFIG, SET_CONFIG_RF,
                   sizeof(SET_CONFIG_RF)) != 0)
    return -1;
  if (nci_send_any(nfc, st, GID_CORE, CORE_SET_CLK, SET_CLK,
                   sizeof(SET_CLK)) != 0)
    return -1;
  if (nci_send_any(nfc, st, GID_PROP, PROP_CMD_0C, PROP_0C, sizeof(PROP_0C)) !=
      0)
    return -1;

  if (nci_send_any(nfc, st, GID_RF, RF_SET_ROUTING, RF_ROUTING,
                   sizeof(RF_ROUTING)) != 0)
    return -1;
  if (nci_send_any(nfc, st, GID_CORE, CORE_SET_CONFIG, SET_CONFIG_19,
                   sizeof(SET_CONFIG_19)) != 0)
    return -1;
  if (nci_send_any(nfc, st, GID_CORE, CORE_SET_CONFIG, SET_CONFIG_80,
                   sizeof(SET_CONFIG_80)) != 0)
    return -1;
  if (nci_send_any(nfc, st, GID_CORE, CORE_SET_CONFIG, SET_CONFIG_POLL_PARAMS,
                   sizeof(SET_CONFIG_POLL_PARAMS)) != 0)
    return -1;
  if (nci_send_any(nfc, st, GID_CORE, CORE_SET_CONFIG, SET_CONFIG_00,
                   sizeof(SET_CONFIG_00)) != 0)
    return -1;
  if (nci_send_any(nfc, st, GID_RF, RF_DISCOVER, DISCOVER_PARAMS,
                   sizeof(DISCOVER_PARAMS)) != 0)
    return -1;

  /* nfc.py discards whatever arrived during the command sequence. */
  nci_queue_clear(st);
  return 0;
}

/*
 * Wait for a tag, restarting a quiet poll once a second.  Returns 0 and the
 * tag notification, 1 when the poll window expired, -1 on a protocol error.
 */
static int nci_discover(kern_nfc_t *nfc, nci_state_t *st, int timeout_ms,
                        uint8_t *out, size_t cap, size_t *out_len) {
  int64_t deadline = nci_now_ms() + timeout_ms;
  int64_t last_discover = nci_now_ms();

  for (;;) {
    uint8_t pkt[KERN_NFC_MAX_PACKET];
    size_t len = 0;
    int left = nci_timeout_left(deadline);
    int r;

    if (g_cancel)
      return -1;
    if (left == 0)
      return 1;

    /* Notifications buffered while a command was being sent come first. */
    for (;;) {
      r = nci_queue_pop(st, pkt, sizeof(pkt), &len);
      if (r < 0)
        return -1;
      if (r == 0)
        break;
      if (kern_nfc_nci_is_tag(pkt, len)) {
        if (cap < len)
          return -1;
        memcpy(out, pkt, len);
        if (out_len)
          *out_len = len;
        return 0;
      }
    }

    if (nci_now_ms() - last_discover >= DISCOVER_INTERVAL_MS) {
      uint8_t rsp[KERN_NFC_MAX_PACKET];
      size_t rsp_len = 0;
      uint8_t status;

      last_discover = nci_now_ms();

      /*
       * RF_DISCOVER on its own returns 0x06 and does not bring the field
       * back up; the vendor's stack deactivates and re-sends the routing.
       */
      if (nci_send_any(nfc, st, GID_RF, RF_DEACTIVATE,
                       (const uint8_t[]){0x00}, 1) != 0)
        return -1;
      if (nci_send_any(nfc, st, GID_RF, RF_SET_ROUTING, RF_ROUTING,
                       sizeof(RF_ROUTING)) != 0)
        return -1;
      if (nci_send(nfc, st, GID_RF, RF_DISCOVER, DISCOVER_PARAMS,
                   sizeof(DISCOVER_PARAMS), rsp, sizeof(rsp), &rsp_len) != 0)
        return -1;
      if (kern_nfc_nci_response_status(rsp, rsp_len, &status) != 0)
        return -1;
      /* 0x00 ok, 0x06 already active, 0x09 already started, 0x0a activated */
      if (status != 0x00 && status != 0x06 && status != 0x09 &&
          status != 0x0A)
        return -1;
    }

    left = nci_timeout_left(deadline);
    if (left == 0)
      return 1;
    if (left > DISCOVER_READ_MS)
      left = DISCOVER_READ_MS;
    r = nci_read_packet(nfc, pkt, sizeof(pkt), left, &len);
    if (r < 0)
      return -1;
    if (r == 0)
      continue;
    if (kern_nfc_nci_is_tag(pkt, len)) {
      if (cap < len)
        return -1;
      memcpy(out, pkt, len);
      if (out_len)
        *out_len = len;
      return 0;
    }
    /* Anything else is dropped here, as in nfc.py _discover(). */
  }
}

/* Select the tag and wait for RF_INTF_ACTIVATED_NTF. */
static int nci_activate(kern_nfc_t *nfc, nci_state_t *st, const uint8_t *ntf,
                        size_t ntf_len) {
  uint8_t params[3];
  int64_t deadline;

  if (ntf && ntf_len >= 4 && ntf[2] == RF_INTF_ACTIVATED_NTF)
    return 0;

  if (kern_nfc_nci_select_params(ntf, ntf_len, params) != 0)
    return -1;
  if (nci_send_ok(nfc, st, GID_RF, RF_DISCOVER_SELECT, params, sizeof(params)) !=
      0)
    return -1;

  deadline = nci_now_ms() + ACTIVATE_NTF_TIMEOUT_MS;
  for (;;) {
    uint8_t pkt[KERN_NFC_MAX_PACKET];
    size_t len = 0;
    int left = nci_timeout_left(deadline);
    int r;

    for (;;) {
      r = nci_queue_pop(st, pkt, sizeof(pkt), &len);
      if (r < 0)
        return -1;
      if (r == 0)
        break;
      if (pkt[2] == RF_INTF_ACTIVATED_NTF)
        return 0;
    }

    if (left == 0)
      return -1;
    r = nci_read_packet(nfc, pkt, sizeof(pkt), left, &len);
    if (r < 0)
      return -1;
    if (r == 0)
      continue;
    if ((pkt[1] & MT_MASK) == MT_NTF && pkt[2] == RF_INTF_ACTIVATED_NTF)
      return 0;
    nci_queue_push(st, pkt, len);
  }
}

/*
 * Power the chip, replay the vendor bring-up, then poll until a tag is
 * activated.  Opens KERN_NFC_DEV if nfc->fd is not already open.  Blocks for
 * at most TAG_TIMEOUT_MS (30 s).  Returns 0 when a tag is ready for
 * kern_nfc_transceive(), -1 on a protocol error or timeout.
 */
void kern_nfc_cancel(void) { g_cancel = 1; }

int kern_nfc_bring_up(kern_nfc_t *nfc) {
  int64_t deadline;
  nci_state_t *st;

  if (!nfc)
    return -1;
  if (nfc->fd < 0) {
    if (kern_nfc_open(nfc) != 0)
      return -1;
  }

  st = &g_nci;
  nci_bind(st, nfc->fd);
  st->connected = 0;
  g_cancel = 0;
  deadline = nci_now_ms() + TAG_TIMEOUT_MS;

  for (;;) {
    uint8_t ntf[KERN_NFC_MAX_PACKET];
    size_t ntf_len = 0;
    int left = nci_timeout_left(deadline);
    int r;

    if (left == 0 || g_cancel)
      return -1;

    /* A setup failure is a protocol error, not a poll that missed a tag. */
    if (nci_chip_setup(nfc, st) != 0)
      return -1;

    left = nci_timeout_left(deadline);
    if (left == 0 || g_cancel)
      return -1;
    if (left > POLL_WINDOW_MS)
      left = POLL_WINDOW_MS;

    r = nci_discover(nfc, st, left, ntf, sizeof(ntf), &ntf_len);
    if (r < 0)
      return -1;
    if (r > 0) {
      /* No card in this poll window: power down and start a clean cycle. */
      (void)kern_nfc_power(nfc, 0);
      st->connected = 0;
      continue;
    }

    if (nci_activate(nfc, st, ntf, ntf_len) != 0)
      return -1;

    /* The card needs a moment before the first APDU; else it answers B2. */
    nci_sleep_ms(ACTIVATE_SETTLE_MS);
    st->connected = 1;
    return 0;
  }
}

/*
 * Send one ISO-DEP APDU and return (data, SW1, SW2).  *resp_len is in/out:
 * the capacity of resp on entry, the number of data bytes on success.  SW1
 * and SW2 may be NULL if the caller does not want them.  If no tag is
 * active, the full bring-up/poll sequence is run first.  Returns 0 on
 * success, -1 on transport error, timeout, removal, or a response that does
 * not fit in resp.
 */
int kern_nfc_transceive(kern_nfc_t *nfc, const uint8_t *apdu, size_t apdu_len,
                        uint8_t *resp, size_t *resp_len, uint8_t *sw1,
                        uint8_t *sw2) {
  uint8_t pkt[KERN_NFC_MAX_PACKET];
  size_t len;
  int64_t deadline;
  int written;
  nci_state_t *st;

  if (!nfc || !resp_len || (apdu_len != 0 && !apdu))
    return -1;
  if (apdu_len > 0xFF)
    return -1;

  st = &g_nci;
  if (nfc->fd < 0) {
    if (kern_nfc_bring_up(nfc) != 0)
      return -1;
  } else {
    nci_bind(st, nfc->fd);
    if (!st->connected) {
      if (kern_nfc_bring_up(nfc) != 0)
        return -1;
    }
  }

  /* One ISO-DEP APDU is one MT_DATA packet: HCIT, 0x00, 0x00, length. */
  len = kern_nfc_nci_build_packet(MT_DATA, 0x00, apdu, apdu_len, pkt,
                                  sizeof(pkt));
  if (len == 0)
    return -1;
  written = kern_nfc_write(nfc, pkt, len);
  if (written != (int)len)
    return -1;

  deadline = nci_now_ms() + APDU_TIMEOUT_MS;
  for (;;) {
    const uint8_t *data = NULL;
    size_t data_len = 0;
    uint8_t s1 = 0;
    uint8_t s2 = 0;
    int left = nci_timeout_left(deadline);
    int r;

    if (left == 0) {
      st->connected = 0;
      return -1;
    }

    r = nci_read_packet(nfc, pkt, sizeof(pkt), left, &len);
    if (r < 0) {
      st->connected = 0;
      return -1;
    }
    if (r == 0)
      continue;

    if (pkt[1] == MT_DATA) {
      if (kern_nfc_nci_parse_data(pkt, len, &data, &data_len, &s1, &s2) != 0)
        return -1;
      if (data_len > *resp_len || (data_len != 0 && !resp))
        return -1;
      if (data_len != 0)
        memcpy(resp, data, data_len);
      *resp_len = data_len;
      if (sw1)
        *sw1 = s1;
      if (sw2)
        *sw2 = s2;
      return 0;
    }

    if (kern_nfc_nci_is_terminal_error(pkt, len)) {
      st->connected = 0;
      return -1;
    }
    /* CORE_CONN_CREDITS and other notifications: keep waiting. */
  }
}

/* --- byte-for-byte checks against the captured trace ------------------- */

/* The element counts match the NCI length bytes in the captured trace. */
_Static_assert(sizeof(SET_CONFIG_LPTD) == 57,
               "SET_CONFIG_LPTD must match nfc.py / trace length 0x39");
_Static_assert(sizeof(SET_CONFIG_START_UP) == 102,
               "SET_CONFIG_START_UP must match nfc.py / trace length 0x66");
_Static_assert(sizeof(SET_CONFIG_POLL) == 6,
               "SET_CONFIG_POLL must match nfc.py / trace length 0x06");
_Static_assert(sizeof(SET_CONFIG_RF) == 14,
               "SET_CONFIG_RF must match nfc.py");
_Static_assert(sizeof(SET_CONFIG_19) == 4,
               "SET_CONFIG_19 must match nfc.py");
_Static_assert(sizeof(SET_CONFIG_80) == 4,
               "SET_CONFIG_80 must match nfc.py");
_Static_assert(sizeof(SET_CONFIG_POLL_PARAMS) == 16,
               "SET_CONFIG_POLL_PARAMS must match nfc.py");
_Static_assert(sizeof(SET_CONFIG_00) == 5,
               "SET_CONFIG_00 must match nfc.py");
_Static_assert(sizeof(SET_CLK) == 2, "SET_CLK must match nfc.py");
_Static_assert(sizeof(RF_ROUTING) == 22, "RF_ROUTING must match nfc.py");
_Static_assert(sizeof(DISCOVER_MAP_PARAMS) == 7,
               "DISCOVER_MAP_PARAMS must match nfc.py");
_Static_assert(sizeof(DISCOVER_PARAMS) == 15,
               "DISCOVER_PARAMS must match nfc.py");
_Static_assert(sizeof(SELECT_PARAMS) == 3, "SELECT_PARAMS must match nfc.py");
