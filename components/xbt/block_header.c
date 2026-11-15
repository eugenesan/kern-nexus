/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * XBT version-2 block header parsing, serialization, proof-of-work hash and
 * target check.  See block_header.h for the wire layout and provenance.
 */

#include "block_header.h"

#include "blake2b.h"

#include <string.h>

const uint8_t XBT_POW_LIMIT_MAINNET[32] = {
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

/* ------------------------------------------------------------------------- */
/* Minimal SHA-256, needed for BIP340-style tagged hashes.                    */
/* ------------------------------------------------------------------------- */

typedef struct xbt_sha256_ctx {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t data[64];
    size_t datalen;
} xbt_sha256_ctx;

static const uint32_t xbt_sha256_k[64] = {
    UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf),
    UINT32_C(0xe9b5dba5), UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
    UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5), UINT32_C(0xd807aa98),
    UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
    UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7),
    UINT32_C(0xc19bf174), UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
    UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc), UINT32_C(0x2de92c6f),
    UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
    UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8),
    UINT32_C(0xbf597fc7), UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
    UINT32_C(0x06ca6351), UINT32_C(0x14292967), UINT32_C(0x27b70a85),
    UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
    UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e),
    UINT32_C(0x92722c85), UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
    UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3), UINT32_C(0xd192e819),
    UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
    UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c),
    UINT32_C(0x34b0bcb5), UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
    UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3), UINT32_C(0x748f82ee),
    UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
    UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7),
    UINT32_C(0xc67178f2)
};

static uint32_t xbt_rotr32(uint32_t v, unsigned int n)
{
    return (v >> n) | (v << (32u - n));
}

static uint32_t xbt_load32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24)
         | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)
         | (uint32_t)p[3];
}

static void xbt_store32_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t xbt_load32_le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint16_t xbt_load16_le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void xbt_store32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void xbt_store16_le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void xbt_sha256_transform(xbt_sha256_ctx *ctx, const uint8_t block[64])
{
    uint32_t w[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    unsigned int i;

    for (i = 0; i < 16; i++) {
        w[i] = xbt_load32_be(block + (4u * i));
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = xbt_rotr32(w[i - 15], 7)
                    ^ xbt_rotr32(w[i - 15], 18)
                    ^ (w[i - 15] >> 3);
        uint32_t s1 = xbt_rotr32(w[i - 2], 17)
                    ^ xbt_rotr32(w[i - 2], 19)
                    ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t s1 = xbt_rotr32(e, 6) ^ xbt_rotr32(e, 11) ^ xbt_rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + xbt_sha256_k[i] + w[i];
        uint32_t s0 = xbt_rotr32(a, 2) ^ xbt_rotr32(a, 13) ^ xbt_rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void xbt_sha256_init(xbt_sha256_ctx *ctx)
{
    ctx->state[0] = UINT32_C(0x6a09e667);
    ctx->state[1] = UINT32_C(0xbb67ae85);
    ctx->state[2] = UINT32_C(0x3c6ef372);
    ctx->state[3] = UINT32_C(0xa54ff53a);
    ctx->state[4] = UINT32_C(0x510e527f);
    ctx->state[5] = UINT32_C(0x9b05688c);
    ctx->state[6] = UINT32_C(0x1f83d9ab);
    ctx->state[7] = UINT32_C(0x5be0cd19);
    ctx->bitlen = 0;
    ctx->datalen = 0;
}

static void xbt_sha256_update(xbt_sha256_ctx *ctx, const void *data, size_t len)
{
    const uint8_t *in = (const uint8_t *)data;
    size_t i;

    if (len == 0) {
        return;
    }

    ctx->bitlen += (uint64_t)len * 8u;
    i = 0;
    while (i < len) {
        size_t space = 64u - ctx->datalen;
        size_t take = (len - i < space) ? (len - i) : space;

        memcpy(ctx->data + ctx->datalen, in + i, take);
        ctx->datalen += take;
        i += take;

        if (ctx->datalen == 64u) {
            xbt_sha256_transform(ctx, ctx->data);
            ctx->datalen = 0;
        }
    }
}

static void xbt_sha256_final(xbt_sha256_ctx *ctx, uint8_t out[32])
{
    uint64_t bitlen = ctx->bitlen;
    size_t i = ctx->datalen;
    unsigned int j;

    ctx->data[i++] = 0x80u;
    if (i <= 56u) {
        memset(ctx->data + i, 0, 56u - i);
    } else {
        memset(ctx->data + i, 0, 64u - i);
        xbt_sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56u);
    }

    for (j = 0; j < 8; j++) {
        ctx->data[63u - j] = (uint8_t)(bitlen >> (8u * j));
    }
    xbt_sha256_transform(ctx, ctx->data);

    for (j = 0; j < 8; j++) {
        xbt_store32_be(out + (4u * j), ctx->state[j]);
    }
}

static void xbt_sha256(uint8_t out[32], const void *data, size_t len)
{
    xbt_sha256_ctx ctx;

    xbt_sha256_init(&ctx);
    xbt_sha256_update(&ctx, data, len);
    xbt_sha256_final(&ctx, out);
}

/* BIP340 tagged hash: SHA256(SHA256(tag) || SHA256(tag) || msg). */
static void xbt_tagged_sha256(uint8_t out[32], const char *tag,
                              const void *data, size_t len)
{
    xbt_sha256_ctx ctx;
    uint8_t tag_hash[32];
    size_t tag_len = strlen(tag);

    xbt_sha256(tag_hash, tag, tag_len);

    xbt_sha256_init(&ctx);
    xbt_sha256_update(&ctx, tag_hash, sizeof(tag_hash));
    xbt_sha256_update(&ctx, tag_hash, sizeof(tag_hash));
    xbt_sha256_update(&ctx, data, len);
    xbt_sha256_final(&ctx, out);
}

/* ------------------------------------------------------------------------- */
/* Header accessors and (de)serialization.                                    */
/* ------------------------------------------------------------------------- */

uint32_t xbt_block_header_v2_complete_version(const xbt_block_header_v2 *hdr)
{
    if (hdr == NULL) {
        return 0;
    }
    return XBT_BLOCK_HEADER_V2_FLAG
         | (hdr->version & ~XBT_BLOCK_HEADER_V2_FLAG);
}

uint32_t xbt_block_header_v2_time_on_wire(const xbt_block_header_v2 *hdr)
{
    if (hdr == NULL) {
        return 0;
    }
    if ((hdr->flags & XBT_BLOCK_HEADER_FLAG_USE_TIME_OFFSET) != 0) {
        return hdr->time - hdr->time_offset;
    }
    return hdr->time;
}

int xbt_block_header_is_v2(const uint8_t *wire, size_t len)
{
    if (wire == NULL || len < 4u) {
        return -1;
    }
    return (xbt_load32_le(wire) & XBT_BLOCK_HEADER_V2_FLAG) != 0;
}

int xbt_block_header_v2_parse(xbt_block_header_v2 *hdr,
                              const uint8_t *wire, size_t len)
{
    uint32_t complete_version;
    uint32_t height_bits;

    if (hdr == NULL || wire == NULL || len < XBT_BLOCK_HEADER_V2_SIZE) {
        return -1;
    }

    complete_version = xbt_load32_le(wire);
    if ((complete_version & XBT_BLOCK_HEADER_V2_FLAG) == 0) {
        return -1;
    }

    memset(hdr, 0, sizeof(*hdr));
    hdr->version = complete_version & ~XBT_BLOCK_HEADER_V2_FLAG;
    memcpy(hdr->prev_block, wire + 4, sizeof(hdr->prev_block));
    memcpy(hdr->merkle_root, wire + 36, sizeof(hdr->merkle_root));
    hdr->time = xbt_load32_le(wire + 68);
    hdr->bits = xbt_load32_le(wire + 72);
    hdr->nonce = xbt_load32_le(wire + 76);
    hdr->nonce2 = xbt_load32_le(wire + 80);
    hdr->nonce3 = xbt_load32_le(wire + 84);
    memcpy(hdr->extranonce, wire + 88, sizeof(hdr->extranonce));
    hdr->time_offset = xbt_load32_le(wire + 104);
    hdr->tx_count = xbt_load16_le(wire + 108);
    hdr->flags = wire[110];
    hdr->xor_key_mask_clear_bits = wire[111];
    memcpy(hdr->xor_key, wire + 112, sizeof(hdr->xor_key));
    height_bits = xbt_load32_le(wire + 128);
    memcpy(&hdr->height, &height_bits, sizeof(hdr->height));
    memcpy(hdr->mm_rhs, wire + 132, sizeof(hdr->mm_rhs));

    if ((hdr->flags & XBT_BLOCK_HEADER_FLAG_USE_TIME_OFFSET) != 0) {
        hdr->time += hdr->time_offset;
    }
    return 0;
}

int xbt_block_header_v2_serialize(const xbt_block_header_v2 *hdr,
                                  uint8_t out[XBT_BLOCK_HEADER_V2_SIZE])
{
    if (hdr == NULL || out == NULL) {
        return -1;
    }

    xbt_store32_le(out + 0, xbt_block_header_v2_complete_version(hdr));
    memcpy(out + 4, hdr->prev_block, sizeof(hdr->prev_block));
    memcpy(out + 36, hdr->merkle_root, sizeof(hdr->merkle_root));
    xbt_store32_le(out + 68, xbt_block_header_v2_time_on_wire(hdr));
    xbt_store32_le(out + 72, hdr->bits);
    xbt_store32_le(out + 76, hdr->nonce);
    xbt_store32_le(out + 80, hdr->nonce2);
    xbt_store32_le(out + 84, hdr->nonce3);
    memcpy(out + 88, hdr->extranonce, sizeof(hdr->extranonce));
    xbt_store32_le(out + 104, hdr->time_offset);
    xbt_store16_le(out + 108, hdr->tx_count);
    out[110] = hdr->flags;
    out[111] = hdr->xor_key_mask_clear_bits;
    memcpy(out + 112, hdr->xor_key, sizeof(hdr->xor_key));
    xbt_store32_le(out + 128, (uint32_t)hdr->height);
    memcpy(out + 132, hdr->mm_rhs, sizeof(hdr->mm_rhs));
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Proof-of-work hash.                                                        */
/* ------------------------------------------------------------------------- */

int xbt_block_header_v2_hash_ex(const xbt_block_header_v2 *hdr,
                                xbt_block_header_v2_hash_components *out)
{
    uint8_t prev_ordered[32];
    uint8_t prev_hidden[32];
    uint8_t h1_msg[119];
    uint8_t h2_msg[96];
    uint8_t ss[52];
    uint8_t tail[48];
    uint8_t asic_input[160];
    size_t asic_len;
    unsigned int i;
    unsigned int profile;
    int all_zero_key = 1;

    if (hdr == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    /* xor_key_hash = TaggedHash("Bitcoin block hash PoW XOR key", xor_key) */
    xbt_tagged_sha256(out->xor_key_hash, "Bitcoin block hash PoW XOR key",
                      hdr->xor_key, sizeof(hdr->xor_key));

    /* The tagged hash uses the previous hash in display (reversed) order. */
    for (i = 0; i < 32; i++) {
        prev_ordered[i] = hdr->prev_block[31u - i];
    }
    xbt_tagged_sha256(prev_hidden, "Bitcoin prevblock header, hashed",
                      prev_ordered, sizeof(prev_ordered));
    memset(prev_hidden, 0, 6);

    /* h1 commits to the fields that are invisible to the mining hardware. */
    xbt_store32_le(h1_msg + 0, xbt_block_header_v2_complete_version(hdr));
    memcpy(h1_msg + 4, prev_ordered, sizeof(prev_ordered));
    xbt_store32_le(h1_msg + 36, (uint32_t)hdr->height);
    memcpy(h1_msg + 40, hdr->merkle_root, sizeof(hdr->merkle_root));
    xbt_store32_le(h1_msg + 72, xbt_block_header_v2_time_on_wire(hdr));
    h1_msg[76] = 0; /* reserved for an extended 40-bit time */
    xbt_store32_le(h1_msg + 77, hdr->bits);
    xbt_store32_le(h1_msg + 81, (uint32_t)hdr->tx_count);
    h1_msg[85] = hdr->flags;
    h1_msg[86] = hdr->xor_key_mask_clear_bits;
    memcpy(h1_msg + 87, out->xor_key_hash, sizeof(out->xor_key_hash));

    xbt_tagged_sha256(out->h1, "Bitcoin block header 1",
                      h1_msg, sizeof(h1_msg));

    /* h2 is the merge-mining hook. */
    memcpy(h2_msg, out->h1, sizeof(out->h1));
    memset(h2_msg + 32, 0, 32);
    memcpy(h2_msg + 64, hdr->mm_rhs, sizeof(hdr->mm_rhs));
    xbt_tagged_sha256(out->h2, "Merge-mining hook", h2_msg, sizeof(h2_msg));

    /* First BLAKE2b: the stratum-v1-visible part. */
    xbt_store32_le(ss + 0, 0);
    memcpy(ss + 4, out->h2, sizeof(out->h2));
    memcpy(ss + 36, hdr->extranonce, sizeof(hdr->extranonce));
    if (xbt_blake2b(out->hash1, sizeof(out->hash1), ss, sizeof(ss)) != 0) {
        return -1;
    }

    /* The tail shared by the mining-hardware profiles. */
    xbt_store32_le(tail + 0, hdr->nonce);
    xbt_store32_le(tail + 4, hdr->nonce2);
    xbt_store32_le(tail + 8, hdr->time_offset);
    xbt_store32_le(tail + 12, hdr->nonce3);
    memcpy(tail + 16, out->hash1, sizeof(out->hash1));

    profile = hdr->flags & 3u;
    switch (profile) {
    case 0:
        memcpy(asic_input, prev_hidden, sizeof(prev_hidden));
        memcpy(asic_input + 32, tail, sizeof(tail));
        asic_len = 80;
        break;
    case 1:
        xbt_store32_le(asic_input + 0, hdr->nonce);
        xbt_store32_le(asic_input + 4, hdr->nonce2);
        xbt_store32_le(asic_input + 8, hdr->nonce3);
        xbt_store32_le(asic_input + 12, hdr->time_offset);
        memcpy(asic_input + 16, out->hash1, sizeof(out->hash1));
        memcpy(asic_input + 48, out->h2, sizeof(out->h2));
        asic_len = 80;
        break;
    case 2:
        memset(asic_input, 0, 48);
        memcpy(asic_input + 48, out->h2, sizeof(out->h2));
        memcpy(asic_input + 80, tail, sizeof(tail));
        asic_len = 128;
        break;
    default: /* profile 3 */
        memset(asic_input, 0, 80);
        memcpy(asic_input + 80, out->h2, sizeof(out->h2));
        memcpy(asic_input + 112, tail, sizeof(tail));
        asic_len = 160;
        break;
    }

    if (xbt_blake2b(out->hash2, sizeof(out->hash2),
                    asic_input, asic_len) != 0) {
        return -1;
    }

    /* Optional XOR mask, used by pooled mining to hide the xor key. */
    for (i = 0; i < sizeof(hdr->xor_key); i++) {
        if (hdr->xor_key[i] != 0) {
            all_zero_key = 0;
            break;
        }
    }
    if (!all_zero_key) {
        unsigned int clear_bytes = hdr->xor_key_mask_clear_bits / 8u;
        unsigned int remaining_bits = hdr->xor_key_mask_clear_bits % 8u;

        xbt_tagged_sha256(out->mask, "Bitcoin block hash PoW XOR mask",
                          hdr->xor_key, sizeof(hdr->xor_key));
        if (clear_bytes > sizeof(out->mask)) {
            clear_bytes = sizeof(out->mask);
        }
        if (clear_bytes != 0) {
            memset(out->mask, 0, clear_bytes);
        }
        if (clear_bytes < sizeof(out->mask)) {
            out->mask[clear_bytes] &=
                (uint8_t)(0xffu >> remaining_bits);
        }
    }

    for (i = 0; i < sizeof(out->hash); i++) {
        out->hash[i] = (uint8_t)(out->hash2[i] ^ out->mask[i]);
    }
    out->asic_profile = (uint8_t)profile;
    return 0;
}

int xbt_block_header_v2_hash(const xbt_block_header_v2 *hdr, uint8_t out[32])
{
    xbt_block_header_v2_hash_components components;

    if (out == NULL) {
        return -1;
    }
    if (xbt_block_header_v2_hash_ex(hdr, &components) != 0) {
        return -1;
    }
    memcpy(out, components.hash, sizeof(components.hash));
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Compact target and proof-of-work check.                                    */
/* ------------------------------------------------------------------------- */

int xbt_pow_target_from_bits(uint32_t bits, const uint8_t pow_limit[32],
                             uint8_t target[32])
{
    uint32_t word = bits & UINT32_C(0x007fffff);
    unsigned int size = (unsigned int)(bits >> 24);
    int negative = (bits & UINT32_C(0x00800000)) != 0 && word != 0;
    int overflow;
    unsigned int i;

    if (pow_limit == NULL || target == NULL) {
        return -1;
    }
    if (negative || word == 0) {
        return -1;
    }

    overflow = size > 34u
            || (word > 0xffu && size > 33u)
            || (word > 0xffffu && size > 32u);
    if (overflow) {
        return -1;
    }

    memset(target, 0, 32);

    if (size <= 3u) {
        uint32_t value = word >> (8u * (3u - size));

        target[28] = (uint8_t)(value >> 24);
        target[29] = (uint8_t)(value >> 16);
        target[30] = (uint8_t)(value >> 8);
        target[31] = (uint8_t)value;
    } else {
        int lsb_index = 31 - (int)(size - 3u);

        for (i = 0; i < 3u; i++) {
            uint8_t byte = (uint8_t)(word >> (8u * i));

            if (byte != 0) {
                int index = lsb_index - (int)i;

                if (index < 0 || index >= 32) {
                    return -1;
                }
                target[index] = byte;
            }
        }
    }

    /* A non-zero mantissa can still shift away to zero when size <= 3. */
    for (i = 0; i < 32; i++) {
        if (target[i] != 0) {
            break;
        }
    }
    if (i == 32) {
        return -1;
    }

    if (memcmp(target, pow_limit, 32) > 0) {
        return -1;
    }
    return 0;
}

int xbt_pow_hash_meets_target(const uint8_t hash[32],
                              const uint8_t target[32])
{
    if (hash == NULL || target == NULL) {
        return -1;
    }
    return memcmp(hash, target, 32) <= 0;
}

int xbt_block_header_v2_check_pow(const xbt_block_header_v2 *hdr,
                                  const uint8_t pow_limit[32])
{
    uint8_t hash[32];
    uint8_t target[32];

    if (hdr == NULL || pow_limit == NULL) {
        return -1;
    }
    if (xbt_block_header_v2_hash(hdr, hash) != 0) {
        return -1;
    }
    if (xbt_pow_target_from_bits(hdr->bits, pow_limit, target) != 0) {
        return -1;
    }
    return xbt_pow_hash_meets_target(hash, target);
}
