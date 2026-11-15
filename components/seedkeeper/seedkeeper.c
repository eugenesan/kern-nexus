/*
 * SeedKeeper / Satochip applet layer, plaintext flows.
 *
 * Ported from krux/src/krux/seedkeeper.py, which follows the SeedSigner fork
 * by 3rdIteration and pysatochip.  The wire bytes here are the reference:
 *
 *   SELECT      00 A4 04 00 0A "SeedKeeper"
 *   VERIFY_PIN  B0 42 00 00 <len> <pin>
 *   GET_STATUS  B0 3C 00 00
 *   LIST        B0 A6 00 01, then B0 A6 00 02 until 9C12
 *   IMPORT      B0 A1 01 01 <header|size>, B0 A1 01 02 chunks,
 *               B0 A1 01 03 <last chunk>
 *   EXPORT      B0 A2 01 01 02 <id>, then B0 A2 01 02 02 <id> chunks
 *   RESET       B0 A5 00 00 02 <id>
 *
 * The secure channel (P1=0x02) is not implemented; see seedkeeper.h.
 */

#include "seedkeeper.h"

#include <stdio.h>
#include <string.h>

#include <mbedtls/sha256.h>

#ifdef KERN_NEXUS
#include <fcntl.h>
#include <unistd.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <wally_crypto.h>
#include <secp256k1.h>
#include <secp256k1_ecdh.h>
#endif

static const uint8_t seedkeeper_aid[10] = {0x53, 0x65, 0x65, 0x64,
                                           0x4B, 0x65, 0x65, 0x70,
                                           0x65, 0x72};

void seedkeeper_init(kern_seedkeeper_t *sk, kern_nfc_transport_t transport,
                     void *ctx) {
  if (!sk)
    return;
  memset(sk, 0, sizeof(*sk));
  sk->transport = transport;
  sk->ctx = ctx;
  sk->pin[0] = '\0';
}

/* One APDU, no status interpretation.  Returns 0 even for a non-9000 card
 * answer; the caller decides whether that is an error. */
static int seedkeeper_exchange(kern_seedkeeper_t *sk, const uint8_t *apdu,
                               size_t apdu_len, uint8_t *resp,
                               size_t *resp_len, uint16_t *sw) {
  uint8_t scratch[KERN_NFC_MAX_PACKET];
  uint8_t sw1 = 0;
  uint8_t sw2 = 0;
  size_t len;
  int rc;

  if (!sk || !sk->transport.transmit || !apdu)
    return SEEDKEEPER_ERR_ARGUMENT;
  if (apdu_len > 255 + 5)
    return SEEDKEEPER_ERR_LENGTH;
  if (!resp) {
    resp = scratch;
    len = sizeof(scratch);
  } else {
    len = resp_len ? *resp_len : KERN_NFC_MAX_PACKET;
  }

  rc = sk->transport.transmit(sk->ctx, apdu, apdu_len, resp, &len, &sw1,
                              &sw2);
  if (rc != 0) {
    sk->last_sw = 0;
    return SEEDKEEPER_ERR_IO;
  }
  sk->last_sw = (uint16_t)(((uint16_t)sw1 << 8) | sw2);
  if (resp_len)
    *resp_len = len;
  if (sw)
    *sw = sk->last_sw;
  return SEEDKEEPER_OK;
}

#ifdef KERN_NEXUS
/* --- applet v0.1 secure channel --------------------------------------- */

#define SK_INS_INIT_SC 0x81
#define SK_INS_PROCESS_SC 0x82

static void sk_trace(const char *what, int value) {
  int fd = open("/dev/kmsg", O_WRONLY);
  if (fd >= 0) {
    dprintf(fd, "kern-nexus sc: %s %d\n", what, value);
    close(fd);
  }
}

static int sk_random(uint8_t *out, size_t len) {
  int fd = open("/dev/urandom", O_RDONLY);
  ssize_t got;
  if (fd < 0)
    return -1;
  got = read(fd, out, len);
  close(fd);
  return got == (ssize_t)len ? 0 : -1;
}

static int sk_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *data,
                        size_t data_len, uint8_t out[20]) {
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
  if (!md)
    return -1;
  return mbedtls_md_hmac(md, key, key_len, data, data_len, out) == 0 ? 0 : -1;
}

static int sk_aes_cbc(const uint8_t key[16], uint8_t iv[16],
                      const uint8_t *in, size_t len, uint8_t *out,
                      int decrypt) {
  mbedtls_aes_context ctx;
  int rc;
  mbedtls_aes_init(&ctx);
  rc = decrypt ? mbedtls_aes_setkey_dec(&ctx, key, 128)
               : mbedtls_aes_setkey_enc(&ctx, key, 128);
  if (rc == 0)
    rc = mbedtls_aes_crypt_cbc(&ctx,
                               decrypt ? MBEDTLS_AES_DECRYPT : MBEDTLS_AES_ENCRYPT,
                               len, iv, in, out);
  mbedtls_aes_free(&ctx);
  return rc == 0 ? 0 : -1;
}


/* Raw ECDH x coordinate, as the applet uses.  wally_ecdh() hashes the
 * compressed point with SHA256, which is not what the secure channel's
 * HMAC-SHA1 key derivation expects. */
static int sk_ecdh_hash_x(unsigned char *output, const unsigned char *x32,
                         const unsigned char *y32, void *data) {
  (void)y32;
  (void)data;
  memcpy(output, x32, 32);
  return 1;
}

static int sk_ecdh_x(const uint8_t peer33[33], const uint8_t priv[32],
                     uint8_t out[32]) {
  static secp256k1_context *ctx;
  secp256k1_pubkey pub;
  if (!ctx)
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN |
                                   SECP256K1_CONTEXT_VERIFY);
  if (!ctx)
    return -1;
  if (!secp256k1_ec_pubkey_parse(ctx, &pub, peer33, 33))
    return -1;
  return secp256k1_ecdh(ctx, out, &pub, priv, sk_ecdh_hash_x, NULL) ? 0 : -1;
}

/* Keepalive: the BCM2079x drops an idle ISO-DEP link after ~0.3 s. */
static int sk_keepalive(kern_seedkeeper_t *sk) {
  static const uint8_t apdu[] = {0xB0, 0x3C, 0x00, 0x00};
  uint8_t resp[32];
  size_t resp_len = sizeof(resp);
  uint16_t sw = 0;
  if (seedkeeper_exchange(sk, apdu, sizeof(apdu), resp, &resp_len, &sw) != 0)
    return -1;
  return sw == SEEDKEEPER_SW_OK ? 0 : -1;
}

static int sk_recover_peer(kern_seedkeeper_t *sk, const uint8_t *resp,
                           size_t resp_len, uint8_t peer[65]) {
  size_t x_len, sig_len;
  const uint8_t *x, *der;
  uint8_t hash[32];
  uint8_t compact[64];
  uint8_t pub33[33];

  if (resp_len < 6)
    return -1;
  x_len = ((size_t)resp[0] << 8) | resp[1];
  if (x_len != 32 || resp_len < 2 + x_len + 2)
    return -1;
  x = resp + 2;
  sig_len = ((size_t)resp[2 + x_len] << 8) | resp[3 + x_len];
  if (!sig_len || resp_len < 4 + x_len + sig_len)
    return -1;
  der = resp + 4 + x_len;

  /* The card signs 00 20 || X.  We only need the message it signed. */
  {
    uint8_t msg[34];
    msg[0] = 0;
    msg[1] = 0x20;
    memcpy(msg + 2, x, 32);
    mbedtls_sha256(msg, sizeof(msg), hash, 0);
  }
  if (sig_len == 64)
    memcpy(compact, der, 64);
  else if (wally_ec_sig_from_der(der, sig_len, compact, sizeof(compact)) !=
           WALLY_OK)
    return -1;

  for (int recid = 0; recid < 4; recid++) {
    uint8_t recsig[65];
    recsig[0] = (uint8_t)(27 + recid);
    memcpy(recsig + 1, compact, sizeof(compact));
    if (wally_ec_sig_to_public_key(hash, sizeof(hash), recsig, sizeof(recsig),
                                   pub33, sizeof(pub33)) != WALLY_OK)
      continue;
    if (memcmp(pub33 + 1, x, 32) != 0)
      continue;
    if (wally_ec_public_key_decompress(pub33, sizeof(pub33), peer, 65) !=
        WALLY_OK)
      return -1;
    return 0;
  }
  return -1;
}

static int sk_sc_init(kern_seedkeeper_t *sk) {
  uint8_t apdu[5 + 65];
  uint8_t resp[256];
  uint8_t peer[65];
  uint8_t pub33[33];
  uint8_t shared[32];
  uint8_t peer33[33];
  uint8_t key_material[20];
  size_t resp_len = sizeof(resp);
  uint16_t sw = 0;

  for (int tries = 0; tries < 16; tries++) {
    if (sk_random(sk->sc_private, sizeof(sk->sc_private)) != 0)
      return -1;
    if (wally_ec_private_key_verify(sk->sc_private, sizeof(sk->sc_private)) ==
        WALLY_OK)
      break;
    if (tries == 15)
      return -1;
  }
  if (wally_ec_public_key_from_private_key(sk->sc_private,
                                           sizeof(sk->sc_private), pub33,
                                           sizeof(pub33)) != WALLY_OK)
    return -1;

  apdu[0] = SEEDKEEPER_CLA;
  apdu[1] = SK_INS_INIT_SC;
  apdu[2] = 0;
  apdu[3] = 0;
  apdu[4] = 65;
  if (wally_ec_public_key_decompress(pub33, sizeof(pub33), apdu + 5, 65) !=
      WALLY_OK)
    return -1;

  /* The first APDU after an RF re-activation is sometimes lost.  Retry the
   * 0x81 command; a GET_STATUS between attempts also keeps the link alive. */
  int init_rc = -1;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt) {
      /* A lost 0x81 may still have initialized the applet, and selecting
       * the AID alone does not clear that card-side state.  Power-cycle the
       * RF link and re-activate the card before trying 0x81 again; otherwise
       * the applet answers 6E00 (class not supported). */
      (void)kern_nfc_bring_up((kern_nfc_t *)sk->ctx);
      (void)seedkeeper_select(sk);
      usleep(150000);
    }
    resp_len = sizeof(resp);
    init_rc = seedkeeper_exchange(sk, apdu, sizeof(apdu), resp, &resp_len, &sw);
    sk_trace("init try", init_rc);
    if (init_rc == 0 && sw == SEEDKEEPER_SW_OK)
      break;
    if (init_rc == 0 && sw != SEEDKEEPER_SW_OK)
      init_rc = -1;
    (void)sk_keepalive(sk);
    usleep(50000);
  }
  if (init_rc != 0) {
    sk_trace("init io", 0);
    return -1;
  }
  sk_trace("init sw", sw);
  if (sw != SEEDKEEPER_SW_OK)
    return -1;
  if (sk_keepalive(sk) != 0) {
    sk_trace("keepalive1", 0);
    return -1;
  }
  if (sk_recover_peer(sk, resp, resp_len, peer) != 0) {
    sk_trace("recover", 0);
    return -1;
  }
  if (sk_keepalive(sk) != 0)
    return -1;
  /* libwally's ECDH takes a compressed public key. */
  if (wally_ec_public_key_compress(peer, sizeof(peer), peer33, sizeof(peer33)) !=
      WALLY_OK)
    return -1;
  if (sk_ecdh_x(peer33, sk->sc_private, shared) != 0) {
    sk_trace("ecdh", 0);
    return -1;
  }
  if (sk_keepalive(sk) != 0)
    return -1;

  if (sk_hmac_sha1(shared, sizeof(shared), (const uint8_t *)"sc_key", 6,
                   key_material) != 0)
    return -1;
  memcpy(sk->sc_session_key, key_material, sizeof(sk->sc_session_key));
  if (sk_hmac_sha1(shared, sizeof(shared), (const uint8_t *)"sc_mac", 6,
                   sk->sc_mac_key) != 0)
    return -1;
  sk->sc_counter = 1;
  sk->secure_ready = 1;
  memset(shared, 0, sizeof(shared));
  memset(key_material, 0, sizeof(key_material));
  return 0;
}

static int sk_sc_wrap(kern_seedkeeper_t *sk, const uint8_t *apdu,
                      size_t apdu_len, uint8_t *out, size_t cap,
                      size_t *out_len) {
  uint8_t iv[16];
  uint8_t iv_sent[16];
  uint8_t padded[256];
  uint8_t cipher[256];
  uint8_t mac[20];
  uint8_t mac_input[2 + 256];
  size_t padded_len, mac_len = sizeof(mac);

  if (apdu_len > 200 || cap < 5 + 16 + 2 + 208 + 2 + 20)
    return -1;
  if (sk_random(iv, 12) != 0)
    return -1;
  iv[12] = (uint8_t)(sk->sc_counter >> 24);
  iv[13] = (uint8_t)(sk->sc_counter >> 16);
  iv[14] = (uint8_t)(sk->sc_counter >> 8);
  iv[15] = (uint8_t)sk->sc_counter;
  sk->sc_counter += 2;

  padded_len = apdu_len + 16 - (apdu_len % 16);
  memcpy(padded, apdu, apdu_len);
  memset(padded + apdu_len, (int)(padded_len - apdu_len),
         padded_len - apdu_len);
  /* mbedtls updates the IV buffer to the last ciphertext block; the card
   * must receive the original random+counter IV and the MAC covers it. */
  memcpy(iv_sent, iv, sizeof(iv_sent));
  if (sk_aes_cbc(sk->sc_session_key, iv, padded, padded_len, cipher, 0) != 0)
    return -1;

  mac_input[0] = (uint8_t)(padded_len >> 8);
  mac_input[1] = (uint8_t)padded_len;
  memcpy(mac_input + 2, cipher, padded_len);
  {
    uint8_t mac_data[16 + 2 + 256];
    memcpy(mac_data, iv_sent, 16);
    memcpy(mac_data + 16, mac_input, 2 + padded_len);
    if (sk_hmac_sha1(sk->sc_mac_key, sizeof(sk->sc_mac_key), mac_data,
                     18 + padded_len, mac) != 0)
      return -1;
  }

  out[0] = SEEDKEEPER_CLA;
  out[1] = SK_INS_PROCESS_SC;
  out[2] = 0;
  out[3] = 0;
  {
    size_t data_len = 16 + 2 + padded_len + 2 + mac_len;
    size_t p = 5;
    if (data_len > 255 || cap < p + data_len)
      return -1;
    out[4] = (uint8_t)data_len;
    memcpy(out + p, iv_sent, 16);
    p += 16;
    out[p++] = (uint8_t)(padded_len >> 8);
    out[p++] = (uint8_t)padded_len;
    memcpy(out + p, cipher, padded_len);
    p += padded_len;
    out[p++] = (uint8_t)(mac_len >> 8);
    out[p++] = (uint8_t)mac_len;
    memcpy(out + p, mac, mac_len);
    p += mac_len;
    *out_len = p;
  }
  return 0;
}

static int sk_sc_unwrap(kern_seedkeeper_t *sk, const uint8_t *data,
                        size_t data_len, uint8_t *out, size_t *out_len) {
  uint8_t iv[16];
  uint8_t plain[256];
  size_t size;
  size_t pad;

  if (data_len == 0) {
    /* A successful VERIFY_PIN has no response payload. */
    *out_len = 0;
    return 0;
  }
  if (data_len < 18) {
    sk_trace("short reply", (int)data_len);
    return -1;
  }
  memcpy(iv, data, 16);
  size = ((size_t)data[16] << 8) | data[17];
  sk_trace("reply len", (int)data_len);
  sk_trace("cipher size", (int)size);
  if (!size || size > sizeof(plain) || data_len < 18 + size || size % 16)
    return -1;
  if (sk_aes_cbc(sk->sc_session_key, iv, data + 18, size, plain, 1) != 0)
    return -1;
  pad = plain[size - 1];
  sk_trace("pad", (int)pad);
  if (!pad || pad > 16 || pad > size)
    return -1;
  for (size_t i = 0; i < pad; i++)
    if (plain[size - 1 - i] != pad)
      return -1;
  size -= pad;
  if (size > *out_len)
    return -1;
  memcpy(out, plain, size);
  *out_len = size;
  memset(plain, 0, sizeof(plain));
  return 0;
}
#endif /* KERN_NEXUS */

static int seedkeeper_command(kern_seedkeeper_t *sk, uint8_t ins, uint8_t p1,
                              uint8_t p2, const uint8_t *data,
                              size_t data_len, uint8_t *resp,
                              size_t *resp_len, uint16_t *sw) {
  uint8_t apdu[5 + 255];

  if (data_len > 255)
    return SEEDKEEPER_ERR_LENGTH;
  apdu[0] = SEEDKEEPER_CLA;
  apdu[1] = ins;
  apdu[2] = p1;
  apdu[3] = p2;
  apdu[4] = (uint8_t)data_len;
  if (data_len)
    memcpy(apdu + 5, data, data_len);

#ifdef KERN_NEXUS
  /* Applet v0.1 refuses everything except SELECT, GET_STATUS, 0x81 and 0x82
   * once needs_secure_channel is set.  Wrap the command and keep the outer
   * status word as the command's status. */
  if (sk->needs_secure_channel && ins != 0xA4 && ins != SK_INS_INIT_SC &&
      ins != SK_INS_PROCESS_SC && ins != 0xFF && ins != 0x3C) {
    uint8_t wrapped[5 + 300];
    uint8_t reply[300];
    uint8_t plain[256];
    size_t wrapped_len = 0;
    size_t reply_len = sizeof(reply);
    size_t plain_len = sizeof(plain);
    uint16_t outer_sw = 0;
    int rc;

    if (!sk->secure_ready && sk_sc_init(sk) != 0)
      return SEEDKEEPER_ERR_IO;
    if (sk_sc_wrap(sk, apdu, 5 + data_len, wrapped, sizeof(wrapped),
                   &wrapped_len) != 0)
      return SEEDKEEPER_ERR_IO;
    rc = seedkeeper_exchange(sk, wrapped, wrapped_len, reply, &reply_len,
                             &outer_sw);
    if (rc != SEEDKEEPER_OK)
      return rc;
    if (outer_sw != SEEDKEEPER_SW_OK) {
      sk->last_sw = outer_sw;
      if (sw)
        *sw = outer_sw;
      return SEEDKEEPER_ERR_STATUS;
    }
    if (sk_sc_unwrap(sk, reply, reply_len, plain, &plain_len) != 0) {
      sk_trace("unwrap fail", 0);
      return SEEDKEEPER_ERR_IO;
    }
    if (resp && resp_len) {
      if (*resp_len < plain_len)
        return SEEDKEEPER_ERR_LENGTH;
      memcpy(resp, plain, plain_len);
      *resp_len = plain_len;
    }
    if (sw)
      *sw = SEEDKEEPER_SW_OK;
    sk->last_sw = SEEDKEEPER_SW_OK;
    return SEEDKEEPER_OK;
  }
#endif

  return seedkeeper_exchange(sk, apdu, 5 + data_len, resp, resp_len, sw);
}

static int seedkeeper_command_ok(kern_seedkeeper_t *sk, uint8_t ins, uint8_t p1,
                                 uint8_t p2, const uint8_t *data,
                                 size_t data_len, uint8_t *resp,
                                 size_t *resp_len) {
  uint16_t sw = 0;
  int rc = seedkeeper_command(sk, ins, p1, p2, data, data_len, resp, resp_len,
                              &sw);
  if (rc != SEEDKEEPER_OK)
    return rc;
  if (sw != SEEDKEEPER_SW_OK)
    return SEEDKEEPER_ERR_STATUS;
  return SEEDKEEPER_OK;
}

/* --- small helpers ------------------------------------------------------ */

int seedkeeper_describe_status(uint16_t sw, char *buf, size_t cap) {
  static const struct {
    uint16_t sw;
    const char *text;
  } table[] = {
      {0x6300, "State unchanged"},
      {0x6400, "State change expected"},
      {0x6700, "Wrong length"},
      {0x6982, "Security status not satisfied"},
      {0x6985, "Conditions of use not satisfied"},
      {0x6A80, "Incorrect parameters in the data field"},
      {0x6A82, "File not found"},
      {0x6A86, "Incorrect parameters P1 P2"},
      {0x6B00, "Wrong parameters P1 P2"},
      {0x6D00, "Not supported by this applet"},
      {0x6E00, "Class not supported"},
      {0x6F00, "Unknown error"},
      {0x9000, "Success"},
      {0x9C01, "Card is full"},
      {0x9C02, "Wrong PIN"},
      {0x9C03, "Operation not allowed"},
      {0x9C04, "Card setup not done"},
      {0x9C05, "Feature unsupported"},
      {0x9C06, "PIN required"},
      {0x9C08, "Secret not found"},
      {0x9C0C, "Identity blocked"},
      {0x9C0F, "Invalid parameter"},
      {0x9C10, "Incorrect P1"},
      {0x9C11, "Incorrect P2"},
      {0x9C12, "No more secrets"},
      {0x9C20, "Secure channel required"},
      {0x9C21, "Secure channel not initialized"},
      {0x9C22, "Secure channel IV rejected"},
      {0x9C23, "Secure channel MAC rejected"},
      {0x9C26, "Instruction deprecated"},
      {0x9C30, "Card operation lock was not released"},
      {0x9C31, "Export not allowed"},
      {0x9C38, "Wrong secret type"},
      {0x9CFF, "Card internal error"},
  };
  size_t i;

  if (!buf || cap == 0)
    return 0;

  if ((sw & 0xFF00) == 0x6300 && (sw & 0x00F0) == 0x00C0) {
    unsigned tries = sw & 0x000F;
    return snprintf(buf, cap, "PIN is incorrect, %u %s left", tries,
                    tries == 1 ? "try" : "tries");
  }
  for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (table[i].sw == sw)
      return snprintf(buf, cap, "%s (%#06x)", table[i].text, sw);
  }
  return snprintf(buf, cap, "Unknown error (%#06x)", sw);
}

static unsigned seedkeeper_padded_size(size_t secret_len) {
  return (unsigned)(secret_len + 16 - (secret_len % 16));
}

static int seedkeeper_sha256(const uint8_t *data, size_t len,
                             uint8_t digest[32]) {
  return mbedtls_sha256(data, len, digest, 0) == 0 ? 0 : -1;
}

/* Strict UTF-8 check, matching what Python's decode("utf-8") accepts for the
 * label bytes.  Overlongs, surrogates and values above U+10FFFF are invalid. */
static int seedkeeper_utf8_valid(const uint8_t *s, size_t len) {
  size_t i = 0;
  while (i < len) {
    uint8_t c = s[i++];
    uint32_t cp;
    size_t need;
    if (c < 0x80)
      continue;
    if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      need = 1;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      need = 2;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      need = 3;
    } else {
      return 0;
    }
    if (i + need > len)
      return 0;
    while (need--) {
      uint8_t t = s[i++];
      if ((t & 0xC0) != 0x80)
        return 0;
      cp = (cp << 6) | (t & 0x3F);
    }
    if ((c & 0xE0) == 0xC0 && cp < 0x80)
      return 0;
    if ((c & 0xF0) == 0xE0 && cp < 0x800)
      return 0;
    if ((c & 0xF8) == 0xF0 && cp < 0x10000)
      return 0;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
      return 0;
  }
  return 1;
}

int seedkeeper_parse_header(const uint8_t *data, size_t len,
                            kern_seedkeeper_header_t *out) {
  uint8_t label_len;

  if (!data || !out)
    return SEEDKEEPER_ERR_ARGUMENT;
  if (len < SEEDKEEPER_HEADER_PREFIX)
    return SEEDKEEPER_ERR_PARSE;
  label_len = data[14];
  if (len < (size_t)SEEDKEEPER_HEADER_PREFIX + label_len)
    return SEEDKEEPER_ERR_PARSE;

  memset(out, 0, sizeof(*out));
  out->id = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
  out->type = data[2];
  out->origin = data[3];
  out->export_rights = data[4];
  out->export_nbplain = data[5];
  out->export_nbsecure = data[6];
  out->export_counter = data[7];
  memcpy(out->fingerprint, data + 8, SEEDKEEPER_FINGERPRINT_SIZE);
  out->subtype = data[12];
  out->label_len = label_len;
  out->label_valid =
      seedkeeper_utf8_valid(data + SEEDKEEPER_HEADER_PREFIX, label_len);
  if (label_len > SEEDKEEPER_MAX_LABEL) {
    out->label_valid = 0;
    label_len = SEEDKEEPER_MAX_LABEL;
  }
  memcpy(out->label, data + SEEDKEEPER_HEADER_PREFIX, label_len);
  out->label[label_len] = '\0';
  return SEEDKEEPER_OK;
}

/* --- applet selection and status --------------------------------------- */

int seedkeeper_select(kern_seedkeeper_t *sk) {
  uint8_t apdu[5 + sizeof(seedkeeper_aid)];
  /* SELECT returns the applet's FCI (13 bytes on a SeedKeeper).  A 2-byte
   * buffer makes the transport fail before it can hand back the status
   * word, which reads as a dead card. */
  uint8_t resp[256];
  size_t resp_len = sizeof(resp);
  uint16_t sw = 0;
  int rc;

  apdu[0] = 0x00;
  apdu[1] = 0xA4;
  apdu[2] = 0x04;
  apdu[3] = 0x00;
  apdu[4] = (uint8_t)sizeof(seedkeeper_aid);
  memcpy(apdu + 5, seedkeeper_aid, sizeof(seedkeeper_aid));

  sk->selected = 0;
  sk->pin_verified = 0;
  sk->secure_ready = 0;
  sk->pin[0] = '\0';
  rc = seedkeeper_exchange(sk, apdu, sizeof(apdu), resp, &resp_len, &sw);
  if (rc != SEEDKEEPER_OK)
    return rc;
  if (sw != SEEDKEEPER_SW_OK)
    return SEEDKEEPER_ERR_STATUS;
  sk->selected = 1;
  return SEEDKEEPER_OK;
}

int seedkeeper_refresh(kern_seedkeeper_t *sk) {
  int rc = SEEDKEEPER_ERR_IO;
  int attempt;

  /* Selecting logs the card out, so this is only for commands that log in
   * again (PIN verification, setup).  Retry only a transport error; a card
   * status word is final. */
  for (attempt = 0; attempt < 3; attempt++) {
    rc = seedkeeper_select(sk);
    if (rc == SEEDKEEPER_OK)
      return SEEDKEEPER_OK;
    if (rc != SEEDKEEPER_ERR_IO)
      return rc;
  }
  return rc;
}

int seedkeeper_card_status(kern_seedkeeper_t *sk,
                           kern_seedkeeper_card_status_t *out) {
  uint8_t data[32];
  size_t len = sizeof(data);
  int rc;

  if (!out)
    return SEEDKEEPER_ERR_ARGUMENT;
  rc = seedkeeper_command_ok(sk, SEEDKEEPER_INS_GET_STATUS, 0x00, 0x00, NULL,
                             0, data, &len);
  if (rc != SEEDKEEPER_OK)
    return rc;
  if (len < 4)
    return SEEDKEEPER_ERR_PARSE;

  memset(out, 0, sizeof(*out));
  out->protocol_major = data[0];
  out->protocol_minor = data[1];
  out->applet_major = data[2];
  out->applet_minor = data[3];
  out->pin_tries_left = len > 4 ? data[4] : -1;
  /* A card that predates the field could only be here already set up. */
  out->setup_done = len > 10 ? data[10] != 0 : 1;
  out->needs_secure_channel = len > 11 ? data[11] != 0 : 0;
  sk->needs_secure_channel = out->needs_secure_channel;
  return SEEDKEEPER_OK;
}

int seedkeeper_memory_status(kern_seedkeeper_t *sk,
                             kern_seedkeeper_memory_status_t *out) {
  uint8_t data[32];
  size_t len = sizeof(data);
  int rc;

  if (!out)
    return SEEDKEEPER_ERR_ARGUMENT;
  rc = seedkeeper_command_ok(sk, SEEDKEEPER_INS_SEEDKEEPER_STATUS, 0x00, 0x00,
                             NULL, 0, data, &len);
  if (rc != SEEDKEEPER_OK)
    return rc;
  if (len < 6)
    return SEEDKEEPER_ERR_PARSE;

  out->nb_secrets = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
  out->total_memory = (uint16_t)(((uint16_t)data[2] << 8) | data[3]);
  out->free_memory = (uint16_t)(((uint16_t)data[4] << 8) | data[5]);
  out->nb_logs_total =
      len > 7 ? ((int)data[6] << 8) | data[7] : -1;
  out->nb_logs_avail =
      len > 9 ? ((int)data[8] << 8) | data[9] : -1;
  return SEEDKEEPER_OK;
}

int seedkeeper_verify_pin(kern_seedkeeper_t *sk, const char *pin) {
  size_t len;
  int rc;

  if (!sk || !pin)
    return SEEDKEEPER_ERR_ARGUMENT;
  len = strlen(pin);
  if (len < SEEDKEEPER_PIN_MIN || len > SEEDKEEPER_PIN_MAX)
    return SEEDKEEPER_ERR_ARGUMENT;

  sk->pin_verified = 0;
  rc = seedkeeper_command_ok(sk, SEEDKEEPER_INS_VERIFY_PIN, 0x00, 0x00,
                             (const uint8_t *)pin, len, NULL, NULL);
  if (rc != SEEDKEEPER_OK)
    return rc;
  sk->pin_verified = 1;
  memcpy(sk->pin, pin, len + 1);
  return SEEDKEEPER_OK;
}

/* --- secret headers ---------------------------------------------------- */

int seedkeeper_list_headers(kern_seedkeeper_t *sk,
                            kern_seedkeeper_header_t *out, size_t *count) {
  size_t cap;
  size_t used = 0;
  uint8_t p2 = 0x01;

  if (!sk || !count || (!out && *count != 0))
    return SEEDKEEPER_ERR_ARGUMENT;
  cap = *count;

  for (;;) {
    uint8_t data[128];
    size_t len = sizeof(data);
    uint16_t sw = 0;
    int rc = seedkeeper_command(sk, SEEDKEEPER_INS_LIST_HEADERS, 0x00, p2, NULL,
                                0, data, &len, &sw);
    /* 9C12 ends the walk and is reported as an error status by the command
     * layer; it is the normal exit, not a failure. */
    if (sw == SEEDKEEPER_SW_NO_MORE_SECRETS)
      break;
    if (rc != SEEDKEEPER_OK)
      return rc;
    if (sw != SEEDKEEPER_SW_OK)
      return SEEDKEEPER_ERR_STATUS;
    if (used == cap) {
      *count = used + 1;
      return SEEDKEEPER_ERR_SPACE;
    }
    rc = seedkeeper_parse_header(data, len, &out[used]);
    if (rc != SEEDKEEPER_OK)
      return rc;
    used++;
    p2 = 0x02;
  }
  *count = used;
  return SEEDKEEPER_OK;
}

/* --- import / export --------------------------------------------------- */

int seedkeeper_import_secret(kern_seedkeeper_t *sk, uint8_t type,
                             uint8_t subtype, const char *label,
                             const uint8_t *secret, size_t secret_len,
                             uint16_t *id_out,
                             uint8_t fingerprint_out[SEEDKEEPER_FINGERPRINT_SIZE]) {
  uint8_t payload[SEEDKEEPER_HEADER_PREFIX + SEEDKEEPER_MAX_LABEL + 2];
  uint8_t data[2 + SEEDKEEPER_CHUNK_SIZE];
  uint8_t resp[16];
  uint8_t digest[32];
  size_t label_len;
  size_t offset = 0;
  size_t remaining;
  unsigned padded;
  size_t payload_len;
  size_t len;
  int rc;

  if (!sk || !secret || secret_len == 0)
    return SEEDKEEPER_ERR_ARGUMENT;
  label = label ? label : "";
  label_len = strlen(label);
  if (label_len > SEEDKEEPER_MAX_LABEL)
    return SEEDKEEPER_ERR_ARGUMENT;
  padded = seedkeeper_padded_size(secret_len);
  if (padded > 0xFFFF)
    return SEEDKEEPER_ERR_LENGTH;

  /* header[2:]: type, origin, export rights, three export counters, four
   * fingerprint bytes, subtype, reserved, label length, label.  The id is
   * assigned by the card and the origin import byte is ignored. */
  payload[0] = type;
  payload[1] = 0x00;
  payload[2] = SEEDKEEPER_EXPORT_PLAINTEXT_ALLOWED;
  payload[3] = 0x00;
  payload[4] = 0x00;
  payload[5] = 0x00;
  payload[6] = 0x00;
  payload[7] = 0x00;
  payload[8] = 0x00;
  payload[9] = 0x00;
  payload[10] = subtype;
  payload[11] = 0x00;
  payload[12] = (uint8_t)label_len;
  memcpy(payload + 13, label, label_len);
  payload[13 + label_len] = (uint8_t)(padded >> 8);
  payload[14 + label_len] = (uint8_t)(padded & 0xFF);
  payload_len = 15 + label_len;

  len = sizeof(resp);
  rc = seedkeeper_command_ok(sk, SEEDKEEPER_INS_IMPORT_SECRET, 0x01, 0x01,
                             payload, payload_len, resp, &len);
  if (rc != SEEDKEEPER_OK)
    return rc;

  remaining = secret_len;
  while (remaining > SEEDKEEPER_CHUNK_SIZE) {
    data[0] = 0x00;
    data[1] = SEEDKEEPER_CHUNK_SIZE;
    memcpy(data + 2, secret + offset, SEEDKEEPER_CHUNK_SIZE);
    rc = seedkeeper_command_ok(sk, SEEDKEEPER_INS_IMPORT_SECRET, 0x01, 0x02,
                               data, 2 + SEEDKEEPER_CHUNK_SIZE, NULL, NULL);
    if (rc != SEEDKEEPER_OK)
      return rc;
    offset += SEEDKEEPER_CHUNK_SIZE;
    remaining -= SEEDKEEPER_CHUNK_SIZE;
  }

  data[0] = (uint8_t)(remaining >> 8);
  data[1] = (uint8_t)(remaining & 0xFF);
  memcpy(data + 2, secret + offset, remaining);
  len = sizeof(resp);
  rc = seedkeeper_command_ok(sk, SEEDKEEPER_INS_IMPORT_SECRET, 0x01, 0x03,
                             data, 2 + remaining, resp, &len);
  if (rc != SEEDKEEPER_OK)
    return rc;
  if (len < 6)
    return SEEDKEEPER_ERR_PARSE;

  if (id_out)
    *id_out = (uint16_t)(((uint16_t)resp[0] << 8) | resp[1]);

  if (seedkeeper_sha256(secret, secret_len, digest) != 0)
    return SEEDKEEPER_ERR_FINGERPRINT;
  if (fingerprint_out)
    memcpy(fingerprint_out, digest, SEEDKEEPER_FINGERPRINT_SIZE);
  if (memcmp(resp + 2, digest, SEEDKEEPER_FINGERPRINT_SIZE) != 0)
    return SEEDKEEPER_ERR_FINGERPRINT;
  return SEEDKEEPER_OK;
}

int seedkeeper_export_secret(kern_seedkeeper_t *sk, uint16_t id, uint8_t *out,
                             size_t *out_len) {
  uint8_t id_data[2];
  uint8_t data[512];
  kern_seedkeeper_header_t header;
  uint8_t *dst;
  size_t cap;
  size_t written = 0;
  size_t len;
  int rc;

  if (!sk || !out || !out_len)
    return SEEDKEEPER_ERR_ARGUMENT;
  cap = *out_len;
  dst = out;

  id_data[0] = (uint8_t)(id >> 8);
  id_data[1] = (uint8_t)(id & 0xFF);
  len = sizeof(data);
  rc = seedkeeper_command_ok(sk, SEEDKEEPER_INS_EXPORT_SECRET, 0x01, 0x01,
                             id_data, sizeof(id_data), data, &len);
  if (rc != SEEDKEEPER_OK)
    return rc;
  rc = seedkeeper_parse_header(data, len, &header);
  if (rc != SEEDKEEPER_OK)
    return rc;

  for (;;) {
    size_t chunk_len;
    len = sizeof(data);
    rc = seedkeeper_command_ok(sk, SEEDKEEPER_INS_EXPORT_SECRET, 0x01, 0x02,
                               id_data, sizeof(id_data), data, &len);
    if (rc != SEEDKEEPER_OK)
      return rc;
    if (len < 2)
      return SEEDKEEPER_ERR_PARSE;
    chunk_len = ((size_t)data[0] << 8) | data[1];
    if (chunk_len > len - 2)
      return SEEDKEEPER_ERR_PARSE;
    if (written + chunk_len > cap)
      return SEEDKEEPER_ERR_SPACE;
    memcpy(dst + written, data + 2, chunk_len);
    written += chunk_len;
    /* The applet appends its ECDSA signature to the last chunk only, which
     * is how the walk knows it is done.  The signature is not verified: the
     * plaintext transport has already given the caller the bytes, and the
     * card's key is not part of this protocol. */
    if (len > 2 + chunk_len)
      break;
  }
  *out_len = written;
  return SEEDKEEPER_OK;
}

int seedkeeper_reset_secret(kern_seedkeeper_t *sk, uint16_t id) {
  uint8_t id_data[2];
  uint16_t sw = 0;
  int rc;

  if (!sk)
    return SEEDKEEPER_ERR_ARGUMENT;
  id_data[0] = (uint8_t)(id >> 8);
  id_data[1] = (uint8_t)(id & 0xFF);
  rc = seedkeeper_command(sk, SEEDKEEPER_INS_RESET_SECRET, 0x00, 0x00, id_data,
                          sizeof(id_data), NULL, NULL, &sw);
  if (rc != SEEDKEEPER_OK)
    return rc;
  if (sw == SEEDKEEPER_SW_OK)
    return 0;
  if (sw == SEEDKEEPER_SW_SECRET_NOT_FOUND)
    return 1;
  return SEEDKEEPER_ERR_STATUS;
}

/* --- BIP-39 Masterseed payload ----------------------------------------- */

int seedkeeper_pack_bip39(const uint8_t *master_seed, size_t master_seed_len,
                          const uint8_t *entropy, size_t entropy_len,
                          const char *passphrase, uint8_t *out, size_t cap,
                          size_t *out_len) {
  size_t pass_len;
  size_t total;
  size_t p = 0;

  if (!master_seed || !entropy || !out || !out_len)
    return SEEDKEEPER_ERR_ARGUMENT;
  passphrase = passphrase ? passphrase : "";
  pass_len = strlen(passphrase);
  if (master_seed_len > 255 || entropy_len > 255 || pass_len > 255)
    return SEEDKEEPER_ERR_LENGTH;
  total = 1 + master_seed_len + 1 + 1 + entropy_len + 1 + pass_len;
  if (total > cap)
    return SEEDKEEPER_ERR_SPACE;

  out[p++] = (uint8_t)master_seed_len;
  memcpy(out + p, master_seed, master_seed_len);
  p += master_seed_len;
  out[p++] = SEEDKEEPER_WORDLIST_ENGLISH;
  out[p++] = (uint8_t)entropy_len;
  memcpy(out + p, entropy, entropy_len);
  p += entropy_len;
  out[p++] = (uint8_t)pass_len;
  memcpy(out + p, passphrase, pass_len);
  p += pass_len;

  *out_len = p;
  return SEEDKEEPER_OK;
}

static int seedkeeper_take(const uint8_t *secret, size_t secret_len,
                           size_t *offset, uint8_t **chunk, size_t *chunk_len) {
  size_t n;
  if (*offset >= secret_len)
    return SEEDKEEPER_ERR_PARSE;
  n = secret[*offset];
  (*offset)++;
  if (n > secret_len - *offset)
    return SEEDKEEPER_ERR_PARSE;
  *chunk = (uint8_t *)(secret + *offset);
  *chunk_len = n;
  *offset += n;
  return SEEDKEEPER_OK;
}

int seedkeeper_unpack_bip39(const uint8_t *secret, size_t secret_len,
                            uint8_t *entropy, size_t entropy_cap,
                            size_t *entropy_len, char *passphrase,
                            size_t passphrase_cap, size_t *passphrase_len) {
  uint8_t *master_seed;
  uint8_t *ent;
  uint8_t *pass;
  size_t master_seed_len;
  size_t ent_len;
  size_t pass_len;
  size_t offset = 0;
  uint8_t wordlist;
  int rc;

  if (!secret || !entropy || !entropy_len || !passphrase || !passphrase_cap ||
      !passphrase_len)
    return SEEDKEEPER_ERR_ARGUMENT;

  rc = seedkeeper_take(secret, secret_len, &offset, &master_seed,
                       &master_seed_len);
  if (rc != SEEDKEEPER_OK)
    return rc;
  if (offset >= secret_len)
    return SEEDKEEPER_ERR_PARSE;
  wordlist = secret[offset++];
  if (wordlist != SEEDKEEPER_WORDLIST_ENGLISH)
    return SEEDKEEPER_ERR_PARSE;
  rc = seedkeeper_take(secret, secret_len, &offset, &ent, &ent_len);
  if (rc != SEEDKEEPER_OK)
    return rc;
  rc = seedkeeper_take(secret, secret_len, &offset, &pass, &pass_len);
  if (rc != SEEDKEEPER_OK)
    return rc;
  if (master_seed_len == 0 || ent_len == 0)
    return SEEDKEEPER_ERR_PARSE;
  if (ent_len > entropy_cap || pass_len >= passphrase_cap)
    return SEEDKEEPER_ERR_SPACE;

  memcpy(entropy, ent, ent_len);
  memcpy(passphrase, pass, pass_len);
  passphrase[pass_len] = '\0';
  *entropy_len = ent_len;
  *passphrase_len = pass_len;
  return SEEDKEEPER_OK;
}
