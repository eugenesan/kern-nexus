/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Compact BLAKE2b implementation, written from RFC 7693.
 *
 * The algorithm and the reference code are public domain (CC0).  This file
 * contains no code from Bitcoin Knots or from any other project.
 *
 * Only the unkeyed variants are provided; XBT uses BLAKE2b with a 32-byte
 * digest for proof of work.  The streaming API supports every digest length
 * from 1 to 64 bytes so the implementation can be checked against the RFC
 * 7693 BLAKE2b-512 test vectors as well.
 */

#ifndef XBT_BLAKE2B_H
#define XBT_BLAKE2B_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XBT_BLAKE2B_BLOCK_SIZE 128u
#define XBT_BLAKE2B_MAX_DIGEST 64u

typedef struct xbt_blake2b_ctx {
    uint64_t h[8];
    uint64_t t[2];
    uint8_t buf[XBT_BLAKE2B_BLOCK_SIZE];
    size_t buflen;
    size_t outlen;
} xbt_blake2b_ctx;

/*
 * Initialise a streaming BLAKE2b state for an unkeyed digest of outlen bytes.
 * Returns 0 on success, -1 if outlen is zero, greater than 64, or ctx is NULL.
 */
int xbt_blake2b_init(xbt_blake2b_ctx *ctx, size_t outlen);

/*
 * Absorb data.  Returns 0 on success, -1 on invalid arguments.
 */
int xbt_blake2b_update(xbt_blake2b_ctx *ctx, const void *data, size_t len);

/*
 * Finish the digest.  outlen must equal the value passed to init().
 * Returns 0 on success, -1 on invalid arguments.
 */
int xbt_blake2b_final(xbt_blake2b_ctx *ctx, void *out, size_t outlen);

/*
 * One-shot helper.  Computes outlen bytes of digest over data.
 * Returns 0 on success, -1 on invalid arguments.
 */
int xbt_blake2b(void *out, size_t outlen, const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* XBT_BLAKE2B_H */
