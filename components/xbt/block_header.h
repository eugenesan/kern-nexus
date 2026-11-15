/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * XBT ("blake2b Bitcoin") version-2 block header.
 *
 * The layout and the proof-of-work hash are taken from Bitcoin Knots at tag
 * v29.4.1.knots20260508:
 *
 *   src/primitives/block.h
 *   src/primitives/block.cpp   (CBlockHeader::GetHash)
 *   src/pow.cpp                (DeriveTarget / CheckProofOfWorkImpl)
 *   src/test/data/block_header_v2.json
 *
 * There is no doc/blake2b.md in that tree.  The URL named in the task
 * resolves to HTTP 404, so this implementation follows the source of truth
 * above rather than a prose specification.
 *
 * Wire layout (164 bytes, all integers little endian):
 *
 *   offset  size  field
 *        0     4  complete version (high bit 0x80000000 marks header v2)
 *        4    32  previous block hash, internal/wire byte order
 *       36    32  merkle root, internal/wire byte order
 *       68     4  time on wire (see xbt_block_header_v2_time_on_wire)
 *       72     4  nBits (compact target)
 *       76     4  nNonce
 *       80     4  nonce2
 *       84     4  nonce3
 *       88    16  extranonce
 *      104     4  time offset
 *      108     2  transaction count
 *      110     1  flags
 *      111     1  xor-key mask clear bits
 *      112    16  xor key
 *      128     4  height (signed)
 *      132    32  merge-mining right-hand side
 *      ---   164  total
 *
 * Hash byte order: the 32-byte value produced by xbt_block_header_v2_hash()
 * is in display (big-endian) order, i.e. the order printed by
 * getblockheader and stored in the "block_hash" field of the Knots test
 * vectors.  Proof of work compares that value numerically, so it is the
 * big-endian integer represented by those bytes.  The two 32-byte hash
 * fields in the struct are stored in wire order, exactly as they appear on
 * the wire; reversing prev_block gives the order used inside the tagged
 * hash.
 *
 * This component depends only on C11 and libc.  It does not use kern, LVGL,
 * or any host-specific headers.
 */

#ifndef XBT_BLOCK_HEADER_H
#define XBT_BLOCK_HEADER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XBT_BLOCK_HEADER_V2_SIZE 164u
#define XBT_BLOCK_HEADER_V1_SIZE 80u
#define XBT_BLOCK_HEADER_V2_FLAG UINT32_C(0x80000000)
#define XBT_BLOCK_HEADER_FLAG_USE_TIME_OFFSET 0x04u

/* Bitcoin mainnet proof-of-work limit, big-endian. */
extern const uint8_t XBT_POW_LIMIT_MAINNET[32];

typedef struct xbt_block_header_v2 {
    uint32_t version;            /* low 31 bits; v2 flag is implied */
    int32_t height;
    uint8_t prev_block[32];      /* wire order */
    uint8_t merkle_root[32];     /* wire order */
    uint32_t time;               /* full time, offset already applied */
    uint32_t bits;
    uint32_t nonce;
    uint32_t nonce2;
    uint32_t nonce3;
    uint8_t extranonce[16];
    uint32_t time_offset;
    uint16_t tx_count;
    uint8_t flags;
    uint8_t xor_key_mask_clear_bits;
    uint8_t xor_key[16];
    uint8_t mm_rhs[32];
} xbt_block_header_v2;

/*
 * Intermediate values of the v2 proof-of-work hash.  These correspond to the
 * fields of the same meaning in src/test/data/block_header_v2.json, which
 * makes it possible to check each stage against the official vectors.
 */
typedef struct xbt_block_header_v2_hash_components {
    uint8_t xor_key_hash[32];
    uint8_t h1[32];
    uint8_t h2[32];
    uint8_t hash1[32];           /* vector field "blake2b_1" */
    uint8_t hash2[32];           /* vector field "blake2b_2" */
    uint8_t mask[32];
    uint8_t hash[32];            /* final result, display order */
    uint8_t asic_profile;        /* flags & 3 */
} xbt_block_header_v2_hash_components;

/* Complete version word, including the v2 flag. */
uint32_t xbt_block_header_v2_complete_version(const xbt_block_header_v2 *hdr);

/* Time as serialized on the wire (offset applied when flag bit 2 is set). */
uint32_t xbt_block_header_v2_time_on_wire(const xbt_block_header_v2 *hdr);

/*
 * Return 1 if the 4-byte version word at the start of wire has the v2 flag,
 * 0 if not, and -1 on invalid arguments.
 */
int xbt_block_header_is_v2(const uint8_t *wire, size_t len);

/*
 * Parse a 164-byte v2 header from wire.  len must be at least 164; trailing
 * bytes are ignored so this can be used directly on a block stream.  The
 * version word must carry the v2 flag.
 *
 * Returns 0 on success, -1 on invalid arguments, short input, or a header
 * that is not version 2.
 */
int xbt_block_header_v2_parse(xbt_block_header_v2 *hdr,
                              const uint8_t *wire, size_t len);

/*
 * Serialize hdr to exactly XBT_BLOCK_HEADER_V2_SIZE bytes.
 * Returns 0 on success, -1 on invalid arguments.
 */
int xbt_block_header_v2_serialize(const xbt_block_header_v2 *hdr,
                                  uint8_t out[XBT_BLOCK_HEADER_V2_SIZE]);

/*
 * Compute the v2 proof-of-work hash, in display order.
 * Returns 0 on success, -1 on invalid arguments.
 */
int xbt_block_header_v2_hash(const xbt_block_header_v2 *hdr,
                             uint8_t out[32]);

/*
 * As above, but also return every intermediate stage.  Returns 0 on
 * success, -1 on invalid arguments.
 */
int xbt_block_header_v2_hash_ex(const xbt_block_header_v2 *hdr,
                                xbt_block_header_v2_hash_components *out);

/*
 * Expand a compact nBits value into a 256-bit big-endian target.
 *
 * Mirrors arith_uint256::SetCompact followed by the range checks in
 * DeriveTarget: the sign bit must not be set on a non-zero mantissa, the
 * target must not be zero or overflow 256 bits, and it must not exceed
 * pow_limit (32 bytes, big-endian).
 *
 * Returns 0 on success, -1 if nBits is invalid or the target exceeds the
 * limit.
 */
int xbt_pow_target_from_bits(uint32_t bits, const uint8_t pow_limit[32],
                             uint8_t target[32]);

/*
 * Compare a display-order hash against a big-endian target.
 * Returns 1 if hash <= target, 0 if not, -1 on invalid arguments.
 */
int xbt_pow_hash_meets_target(const uint8_t hash[32],
                              const uint8_t target[32]);

/*
 * Convenience helper: hash hdr and check it against hdr->bits.
 * Returns 1 if the proof of work is valid, 0 if it is not, and -1 if the
 * header or nBits is invalid.
 */
int xbt_block_header_v2_check_pow(const xbt_block_header_v2 *hdr,
                                  const uint8_t pow_limit[32]);

#ifdef __cplusplus
}
#endif

#endif /* XBT_BLOCK_HEADER_H */
