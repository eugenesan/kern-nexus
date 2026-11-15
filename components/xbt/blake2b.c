/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * BLAKE2b, RFC 7693.  Public domain.
 */

#include "blake2b.h"

#include <string.h>

static const uint64_t xbt_blake2b_iv[8] = {
    UINT64_C(0x6a09e667f3bcc908), UINT64_C(0xbb67ae8584caa73b),
    UINT64_C(0x3c6ef372fe94f82b), UINT64_C(0xa54ff53a5f1d36f1),
    UINT64_C(0x510e527fade682d1), UINT64_C(0x9b05688c2b3e6c1f),
    UINT64_C(0x1f83d9abfb41bd6b), UINT64_C(0x5be0cd19137e2179)
};

static const uint8_t xbt_blake2b_sigma[12][16] = {
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
    { 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
    {  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
    {  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
    {  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
    { 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
    { 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
    {  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
    { 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 },
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 }
};

static uint64_t xbt_load64_le(const uint8_t *p)
{
    return (uint64_t)p[0]
         | ((uint64_t)p[1] << 8)
         | ((uint64_t)p[2] << 16)
         | ((uint64_t)p[3] << 24)
         | ((uint64_t)p[4] << 32)
         | ((uint64_t)p[5] << 40)
         | ((uint64_t)p[6] << 48)
         | ((uint64_t)p[7] << 56);
}

static void xbt_store64_le(uint8_t *p, uint64_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    p[4] = (uint8_t)(v >> 32);
    p[5] = (uint8_t)(v >> 40);
    p[6] = (uint8_t)(v >> 48);
    p[7] = (uint8_t)(v >> 56);
}

static uint64_t xbt_rotr64(uint64_t v, unsigned int n)
{
    return (v >> n) | (v << (64u - n));
}

static void xbt_blake2b_increment(xbt_blake2b_ctx *ctx, uint64_t inc)
{
    ctx->t[0] += inc;
    if (ctx->t[0] < inc) {
        ctx->t[1]++;
    }
}

#define XBT_G(a, b, c, d, x, y)                    \
    do {                                           \
        v[a] = v[a] + v[b] + (x);                  \
        v[d] = xbt_rotr64(v[d] ^ v[a], 32);        \
        v[c] = v[c] + v[d];                        \
        v[b] = xbt_rotr64(v[b] ^ v[c], 24);        \
        v[a] = v[a] + v[b] + (y);                  \
        v[d] = xbt_rotr64(v[d] ^ v[a], 16);        \
        v[c] = v[c] + v[d];                        \
        v[b] = xbt_rotr64(v[b] ^ v[c], 63);        \
    } while (0)

static void xbt_blake2b_compress(xbt_blake2b_ctx *ctx,
                                 const uint8_t block[XBT_BLAKE2B_BLOCK_SIZE],
                                 int last)
{
    uint64_t m[16];
    uint64_t v[16];
    unsigned int r;
    unsigned int i;

    for (i = 0; i < 16; i++) {
        m[i] = xbt_load64_le(block + (8u * i));
    }
    for (i = 0; i < 8; i++) {
        v[i] = ctx->h[i];
        v[i + 8] = xbt_blake2b_iv[i];
    }

    v[12] ^= ctx->t[0];
    v[13] ^= ctx->t[1];
    if (last) {
        v[14] = ~v[14];
    }

    for (r = 0; r < 12; r++) {
        const uint8_t *s = xbt_blake2b_sigma[r];

        XBT_G(0, 4,  8, 12, m[s[0]],  m[s[1]]);
        XBT_G(1, 5,  9, 13, m[s[2]],  m[s[3]]);
        XBT_G(2, 6, 10, 14, m[s[4]],  m[s[5]]);
        XBT_G(3, 7, 11, 15, m[s[6]],  m[s[7]]);

        XBT_G(0, 5, 10, 15, m[s[8]],  m[s[9]]);
        XBT_G(1, 6, 11, 12, m[s[10]], m[s[11]]);
        XBT_G(2, 7,  8, 13, m[s[12]], m[s[13]]);
        XBT_G(3, 4,  9, 14, m[s[14]], m[s[15]]);
    }

    for (i = 0; i < 8; i++) {
        ctx->h[i] ^= v[i] ^ v[i + 8];
    }
}

#undef XBT_G

int xbt_blake2b_init(xbt_blake2b_ctx *ctx, size_t outlen)
{
    unsigned int i;

    if (ctx == NULL || outlen == 0 || outlen > XBT_BLAKE2B_MAX_DIGEST) {
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));
    for (i = 0; i < 8; i++) {
        ctx->h[i] = xbt_blake2b_iv[i];
    }

    /* Parameter block: digest length, key length 0, fanout 1, depth 1. */
    ctx->h[0] ^= UINT64_C(0x01010000) ^ (uint64_t)outlen;
    ctx->outlen = outlen;
    return 0;
}

int xbt_blake2b_update(xbt_blake2b_ctx *ctx, const void *data, size_t len)
{
    const uint8_t *in = (const uint8_t *)data;
    size_t left;
    size_t fill;

    if (ctx == NULL || (data == NULL && len != 0)) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    left = ctx->buflen;
    fill = XBT_BLAKE2B_BLOCK_SIZE - left;

    if (len > fill) {
        memcpy(ctx->buf + left, in, fill);
        ctx->buflen = XBT_BLAKE2B_BLOCK_SIZE;
        xbt_blake2b_increment(ctx, XBT_BLAKE2B_BLOCK_SIZE);
        xbt_blake2b_compress(ctx, ctx->buf, 0);
        ctx->buflen = 0;
        in += fill;
        len -= fill;

        while (len > XBT_BLAKE2B_BLOCK_SIZE) {
            xbt_blake2b_increment(ctx, XBT_BLAKE2B_BLOCK_SIZE);
            xbt_blake2b_compress(ctx, in, 0);
            in += XBT_BLAKE2B_BLOCK_SIZE;
            len -= XBT_BLAKE2B_BLOCK_SIZE;
        }
    }

    memcpy(ctx->buf + ctx->buflen, in, len);
    ctx->buflen += len;
    return 0;
}

int xbt_blake2b_final(xbt_blake2b_ctx *ctx, void *out, size_t outlen)
{
    uint8_t digest[XBT_BLAKE2B_MAX_DIGEST];
    unsigned int i;

    if (ctx == NULL || out == NULL || outlen != ctx->outlen) {
        return -1;
    }

    xbt_blake2b_increment(ctx, (uint64_t)ctx->buflen);
    memset(ctx->buf + ctx->buflen, 0,
           XBT_BLAKE2B_BLOCK_SIZE - ctx->buflen);
    xbt_blake2b_compress(ctx, ctx->buf, 1);

    for (i = 0; i < 8; i++) {
        xbt_store64_le(digest + (8u * i), ctx->h[i]);
    }
    memcpy(out, digest, outlen);
    return 0;
}

int xbt_blake2b(void *out, size_t outlen, const void *data, size_t len)
{
    xbt_blake2b_ctx ctx;
    int rc;

    if (out == NULL) {
        return -1;
    }
    rc = xbt_blake2b_init(&ctx, outlen);
    if (rc != 0) {
        return rc;
    }
    rc = xbt_blake2b_update(&ctx, data, len);
    if (rc != 0) {
        return rc;
    }
    return xbt_blake2b_final(&ctx, out, outlen);
}
