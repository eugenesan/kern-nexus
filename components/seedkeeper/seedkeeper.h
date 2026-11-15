#ifndef KERN_SEEDKEEPER_H
#define KERN_SEEDKEEPER_H

#include <stddef.h>
#include <stdint.h>

#include "../nfc/nfc.h"

/*
 * SeedKeeper / Satochip applet layer.
 *
 * The transport is the NFC component: select the applet, verify the PIN,
 * then import or export secrets.  The plaintext flows (P1=0x01) are the
 * ones the SeedSigner app writes and reads, so cards interoperate.  A card
 * that reports needs_secure_channel wraps every command in the applet's
 * secure channel (INS 0x81/0x82), which this layer does transparently.
 *
 * All functions return 0 on success, a negative SEEDKEEPER_ERR_* on a host
 * side failure, or SEEDKEEPER_ERR_STATUS when the card answered a status
 * word other than 9000.  That status word is kept in sk->last_sw.
 */

#define SEEDKEEPER_CLA 0xB0

#define SEEDKEEPER_INS_SETUP 0x2A
#define SEEDKEEPER_INS_VERIFY_PIN 0x42
#define SEEDKEEPER_INS_GET_STATUS 0x3C
#define SEEDKEEPER_INS_GENERATE_MASTERSEED 0xA0
#define SEEDKEEPER_INS_IMPORT_SECRET 0xA1
#define SEEDKEEPER_INS_EXPORT_SECRET 0xA2
#define SEEDKEEPER_INS_GENERATE_RANDOM_SECRET 0xA3
#define SEEDKEEPER_INS_RESET_SECRET 0xA5
#define SEEDKEEPER_INS_LIST_HEADERS 0xA6
#define SEEDKEEPER_INS_SEEDKEEPER_STATUS 0xA7

#define SEEDKEEPER_TYPE_MASTERSEED 0x10
#define SEEDKEEPER_TYPE_BIP39_MNEMONIC 0x30
#define SEEDKEEPER_TYPE_PASSWORD 0x90
#define SEEDKEEPER_TYPE_DATA 0xC0
#define SEEDKEEPER_TYPE_DESCRIPTOR 0xC1

#define SEEDKEEPER_SUBTYPE_BIP39 0x01
#define SEEDKEEPER_EXPORT_PLAINTEXT_ALLOWED 0x01

#define SEEDKEEPER_PIN_MIN 4
#define SEEDKEEPER_PIN_MAX 16
#define SEEDKEEPER_MAX_LABEL 64
#define SEEDKEEPER_CHUNK_SIZE 128
#define SEEDKEEPER_HEADER_PREFIX 15
#define SEEDKEEPER_FINGERPRINT_SIZE 4
#define SEEDKEEPER_WORDLIST_ENGLISH 0x00

#define SEEDKEEPER_SW_OK 0x9000
#define SEEDKEEPER_SW_APPLET_NOT_FOUND 0x6A82
#define SEEDKEEPER_SW_NO_MORE_SECRETS 0x9C12
#define SEEDKEEPER_SW_SECRET_NOT_FOUND 0x9C08
#define SEEDKEEPER_SW_WRONG_PIN 0x9C02
#define SEEDKEEPER_SW_SECURE_CHANNEL_REQUIRED 0x9C20
#define SEEDKEEPER_SW_EXPORT_NOT_ALLOWED 0x9C31

#define SEEDKEEPER_OK 0
#define SEEDKEEPER_ERR_IO (-1)
#define SEEDKEEPER_ERR_STATUS (-2)
#define SEEDKEEPER_ERR_LENGTH (-3)
#define SEEDKEEPER_ERR_ARGUMENT (-4)
#define SEEDKEEPER_ERR_PARSE (-5)
#define SEEDKEEPER_ERR_FINGERPRINT (-6)
#define SEEDKEEPER_ERR_SPACE (-7)

typedef struct {
  uint16_t id;
  uint8_t type;
  uint8_t origin;
  uint8_t export_rights;
  uint8_t export_nbplain;
  uint8_t export_nbsecure;
  uint8_t export_counter;
  uint8_t fingerprint[SEEDKEEPER_FINGERPRINT_SIZE];
  uint8_t subtype;
  uint8_t label_len;
  char label[SEEDKEEPER_MAX_LABEL + 1];
  int label_valid; /* 0 when the card's label is not valid UTF-8 */
} kern_seedkeeper_header_t;

/* Decode the 15-byte prefix plus label, as list/export return it. */
int seedkeeper_parse_header(const uint8_t *data, size_t len,
                            kern_seedkeeper_header_t *out);

typedef struct {
  uint8_t protocol_major;
  uint8_t protocol_minor;
  uint8_t applet_major;
  uint8_t applet_minor;
  int pin_tries_left; /* -1 when the applet does not report it */
  int setup_done;
  int needs_secure_channel;
} kern_seedkeeper_card_status_t;

typedef struct {
  uint16_t nb_secrets;
  uint16_t total_memory;
  uint16_t free_memory;
  int nb_logs_total; /* -1 when absent */
  int nb_logs_avail;
} kern_seedkeeper_memory_status_t;

typedef struct {
  kern_nfc_transport_t transport;
  void *ctx;
  int selected;
  int pin_verified;
  int needs_secure_channel;
  uint16_t last_sw;
  char pin[SEEDKEEPER_PIN_MAX + 1];
  /* Secure channel state (applet v0.1 with needs_secure_channel=1). */
  int secure_ready;
  uint8_t sc_private[32];
  uint8_t sc_session_key[16];
  uint8_t sc_mac_key[20];
  uint32_t sc_counter;
} kern_seedkeeper_t;

void seedkeeper_init(kern_seedkeeper_t *sk, kern_nfc_transport_t transport,
                     void *ctx);

/* Select the applet.  refresh() retries a lost first command on the
 * ISO-DEP link, as nfc.py does. */
int seedkeeper_select(kern_seedkeeper_t *sk);
int seedkeeper_refresh(kern_seedkeeper_t *sk);

int seedkeeper_card_status(kern_seedkeeper_t *sk,
                           kern_seedkeeper_card_status_t *out);
int seedkeeper_memory_status(kern_seedkeeper_t *sk,
                             kern_seedkeeper_memory_status_t *out);

int seedkeeper_verify_pin(kern_seedkeeper_t *sk, const char *pin);

/* Walk every secret header.  *count is capacity in and count out.  Returns
 * SEEDKEEPER_ERR_SPACE (leaving *count = required) if the array is too
 * small. */
int seedkeeper_list_headers(kern_seedkeeper_t *sk,
                            kern_seedkeeper_header_t *out, size_t *count);

/* Import a plaintext secret.  *id_out and fingerprint_out may be NULL. */
int seedkeeper_import_secret(kern_seedkeeper_t *sk, uint8_t type,
                             uint8_t subtype, const char *label,
                             const uint8_t *secret, size_t secret_len,
                             uint16_t *id_out,
                             uint8_t fingerprint_out[SEEDKEEPER_FINGERPRINT_SIZE]);

/* Export one secret.  *out_len is capacity in and payload length out. */
int seedkeeper_export_secret(kern_seedkeeper_t *sk, uint16_t id, uint8_t *out,
                             size_t *out_len);

/* Generate a random Masterseed on the card (INS A0).  The card keeps it as a
 * plain [size | bytes] secret, so export it to learn the bytes; the label
 * says which secret it is.  seed_size is 16 to 64. */
int seedkeeper_generate_masterseed(kern_seedkeeper_t *sk, uint8_t seed_size,
                                   const char *label, uint8_t export_rights,
                                   uint16_t *id_out,
                                   uint8_t fingerprint_out[SEEDKEEPER_FINGERPRINT_SIZE]);

/* Generate a random secret on the card (INS A3).  secret_size is 16 to 64.
 * external_entropy may be NULL; the card mixes it with its own randomness.
 * save_entropy asks the card to keep a proof that the external entropy was
 * used, which is how the SeedKeeper app proves a seed came from the phone. */
int seedkeeper_generate_random_secret(kern_seedkeeper_t *sk,
                                      uint8_t secret_size, const char *label,
                                      uint8_t export_rights, uint8_t type,
                                      uint8_t subtype,
                                      const uint8_t *external_entropy,
                                      size_t external_entropy_len,
                                      int save_entropy, uint16_t *id_out,
                                      uint8_t fingerprint_out[SEEDKEEPER_FINGERPRINT_SIZE]);

/* The bytes of a plain [size | bytes] secret, the form the card uses for a
 * generated secret. */
int seedkeeper_plain_secret_bytes(const uint8_t *secret, size_t secret_len,
                                   uint8_t *out, size_t cap, size_t *out_len);

/* Free one secret.  Returns 1 when it did not exist. */
int seedkeeper_reset_secret(kern_seedkeeper_t *sk, uint16_t id);

/* BIP-39 Masterseed payload: seed | wordlist | entropy | passphrase, every
 * field one-byte length prefixed except the wordlist. */
int seedkeeper_pack_bip39(const uint8_t *master_seed, size_t master_seed_len,
                          const uint8_t *entropy, size_t entropy_len,
                          const char *passphrase, uint8_t *out, size_t cap,
                          size_t *out_len);
/* First field of a BIP39 Masterseed secret: the stored 64-byte seed. */
int seedkeeper_bip39_master_seed(const uint8_t *secret, size_t secret_len,
                                 uint8_t *out, size_t cap, size_t *out_len);

int seedkeeper_unpack_bip39(const uint8_t *secret, size_t secret_len,
                            uint8_t *entropy, size_t entropy_cap,
                            size_t *entropy_len, char *passphrase,
                            size_t passphrase_cap, size_t *passphrase_len);

/* Human-readable status word, hex included, as seedkeeper.py's
 * status_text().  Returns the number of bytes that would have been
 * written. */
int seedkeeper_describe_status(uint16_t sw, char *buf, size_t cap);

#endif
