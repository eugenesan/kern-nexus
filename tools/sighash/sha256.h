#ifndef KERN_SHA256_H
#define KERN_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t data[64];
  size_t datalen;
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len);
void sha256_final(sha256_ctx *ctx, uint8_t out[32]);
void sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/* BIP340-style tagged hash: SHA256(SHA256(tag) || SHA256(tag) || data). */
void sha256_tagged_init(sha256_ctx *ctx, const char *tag);
void sha256_tagged(const char *tag, const uint8_t *data, size_t len,
                   uint8_t out[32]);

/* double SHA256 */
void sha256d(const uint8_t *data, size_t len, uint8_t out[32]);

#endif
