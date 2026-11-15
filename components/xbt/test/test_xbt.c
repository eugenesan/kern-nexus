/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * Host tests for components/xbt.
 *
 * Vector provenance
 * -----------------
 * BLAKE2b-512("") and BLAKE2b-512("abc") are the two test vectors in
 * RFC 7693, Appendix A.
 *
 * The five block-header vectors below are copied verbatim from Bitcoin Knots
 * at tag v29.4.1.knots20260508, file src/test/data/block_header_v2.json:
 *
 *   https://raw.githubusercontent.com/bitcoinknots/bitcoin/\
 *       v29.4.1.knots20260508/src/test/data/block_header_v2.json
 *
 * They are the same vectors exercised by test/functional/feature_powchange.py
 * (test_header_vectors).  Every published intermediate value is checked:
 * serialized bytes, xor_key_hash, h1, h2, blake2b_1, blake2b_2, mask, and the
 * final block_hash, for all four ASIC profiles.
 *
 * Vector gap
 * ----------
 * These are hash vectors, not mined blocks.  With nBits 0x1d00ffff the target
 * is 0x00000000ffff0000...0000, and all five results start with a non-zero
 * byte, so none of them satisfies proof of work.  The tree at this tag does
 * not publish a mined v2 header, and doc/blake2b.md does not exist (the URL
 * in the task returns HTTP 404).  No expected hash has been invented here:
 * the PoW acceptance path is covered structurally by the target boundary
 * tests (hash == target passes, target + 1 fails), and the vectors cover the
 * rejection path.  A real mined-header vector remains a gap in the public
 * material.
 */

#include "blake2b.h"
#include "block_header.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Tiny check helpers.                                                        */
/* ------------------------------------------------------------------------- */

static int failures;

static void check_true(const char *file, int line, const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what);
        failures++;
    }
}

#define CHECK(cond) check_true(__FILE__, __LINE__, #cond, (cond))

static void check_u32(const char *vec, const char *field,
                      uint32_t got, uint32_t want)
{
    if (got != want) {
        fprintf(stderr, "FAIL [%s] %s: got %u, want %u\n",
                vec, field, (unsigned)got, (unsigned)want);
        failures++;
    }
}

static void check_i32(const char *vec, const char *field,
                      int32_t got, int32_t want)
{
    if (got != want) {
        fprintf(stderr, "FAIL [%s] %s: got %ld, want %ld\n",
                vec, field, (long)got, (long)want);
        failures++;
    }
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int hex_decode(const char *hex, uint8_t *out, size_t outlen)
{
    size_t i;

    if (hex == NULL || out == NULL || strlen(hex) != outlen * 2u) {
        return -1;
    }
    for (i = 0; i < outlen; i++) {
        int hi = hex_nibble(hex[2u * i]);
        int lo = hex_nibble(hex[2u * i + 1u]);

        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static void check_bytes(const char *vec, const char *field,
                        const uint8_t *got, const char *hex, size_t len)
{
    uint8_t want[32];

    if (len > sizeof(want) || hex_decode(hex, want, len) != 0) {
        fprintf(stderr, "FAIL [%s] %s: bad test vector\n", vec, field);
        failures++;
        return;
    }
    if (memcmp(got, want, len) != 0) {
        size_t i;

        fprintf(stderr, "FAIL [%s] %s:\n  got  ", vec, field);
        for (i = 0; i < len; i++) {
            fprintf(stderr, "%02x", got[i]);
        }
        fprintf(stderr, "\n  want %s\n", hex);
        failures++;
    }
}

/* ------------------------------------------------------------------------- */
/* BLAKE2b RFC 7693 vectors.                                                  */
/* ------------------------------------------------------------------------- */

static const uint8_t blake2b_512_empty[64] = {
    0x78, 0x6a, 0x02, 0xf7, 0x42, 0x01, 0x59, 0x03,
    0xc6, 0xc6, 0xfd, 0x85, 0x25, 0x52, 0xd2, 0x72,
    0x91, 0x2f, 0x47, 0x40, 0xe1, 0x58, 0x47, 0x61,
    0x8a, 0x86, 0xe2, 0x17, 0xf7, 0x1f, 0x54, 0x19,
    0xd2, 0x5e, 0x10, 0x31, 0xaf, 0xee, 0x58, 0x53,
    0x13, 0x89, 0x64, 0x44, 0x93, 0x4e, 0xb0, 0x4b,
    0x90, 0x3a, 0x68, 0x5b, 0x14, 0x48, 0xb7, 0x55,
    0xd5, 0x6f, 0x70, 0x1a, 0xfe, 0x9b, 0xe2, 0xce
};

static const uint8_t blake2b_512_abc[64] = {
    0xba, 0x80, 0xa5, 0x3f, 0x98, 0x1c, 0x4d, 0x0d,
    0x6a, 0x27, 0x97, 0xb6, 0x9f, 0x12, 0xf6, 0xe9,
    0x4c, 0x21, 0x2f, 0x14, 0x68, 0x5a, 0xc4, 0xb7,
    0x4b, 0x12, 0xbb, 0x6f, 0xdb, 0xff, 0xa2, 0xd1,
    0x7d, 0x87, 0xc5, 0x39, 0x2a, 0xab, 0x79, 0x2d,
    0xc2, 0x52, 0xd5, 0xde, 0x45, 0x33, 0xcc, 0x95,
    0x18, 0xd3, 0x8a, 0xa8, 0xdb, 0xf1, 0x92, 0x5a,
    0xb9, 0x23, 0x86, 0xed, 0xd4, 0x00, 0x99, 0x23
};

static void test_blake2b(void)
{
    uint8_t out[64];
    uint8_t big[300];
    uint8_t one_shot[32];
    uint8_t chunked[32];
    xbt_blake2b_ctx ctx;
    size_t i;
    size_t off;

    CHECK(xbt_blake2b(out, sizeof(out), "", 0) == 0);
    CHECK(memcmp(out, blake2b_512_empty, sizeof(out)) == 0);

    CHECK(xbt_blake2b(out, sizeof(out), "abc", 3) == 0);
    CHECK(memcmp(out, blake2b_512_abc, sizeof(out)) == 0);

    /* Streaming must match the one-shot result. */
    CHECK(xbt_blake2b_init(&ctx, 64) == 0);
    CHECK(xbt_blake2b_update(&ctx, "a", 1) == 0);
    CHECK(xbt_blake2b_update(&ctx, "bc", 2) == 0);
    CHECK(xbt_blake2b_final(&ctx, out, 64) == 0);
    CHECK(memcmp(out, blake2b_512_abc, sizeof(out)) == 0);

    /* Exercise block-boundary handling with a longer deterministic input. */
    for (i = 0; i < sizeof(big); i++) {
        big[i] = (uint8_t)(i * 37u + 11u);
    }
    CHECK(xbt_blake2b(one_shot, sizeof(one_shot),
                      big, sizeof(big)) == 0);
    CHECK(xbt_blake2b_init(&ctx, sizeof(chunked)) == 0);
    off = 0;
    while (off < sizeof(big)) {
        size_t take = (sizeof(big) - off < 7u) ? sizeof(big) - off : 7u;

        CHECK(xbt_blake2b_update(&ctx, big + off, take) == 0);
        off += take;
    }
    CHECK(xbt_blake2b_final(&ctx, chunked, sizeof(chunked)) == 0);
    CHECK(memcmp(one_shot, chunked, sizeof(one_shot)) == 0);

    /* Argument validation. */
    CHECK(xbt_blake2b_init(NULL, 32) == -1);
    CHECK(xbt_blake2b_init(&ctx, 0) == -1);
    CHECK(xbt_blake2b_init(&ctx, 65) == -1);
    CHECK(xbt_blake2b(NULL, 32, "x", 1) == -1);
    CHECK(xbt_blake2b_init(&ctx, 32) == 0);
    CHECK(xbt_blake2b_final(&ctx, out, 31) == -1);
    CHECK(xbt_blake2b_final(&ctx, out, 32) == 0);
}

/* ------------------------------------------------------------------------- */
/* Official Knots v2 header vectors.                                          */
/* ------------------------------------------------------------------------- */

typedef struct test_vector {
    const char *name;
    const char *serialized;
    const char *xor_key_hash;
    const char *h1;
    const char *h2;
    const char *blake2b_1;
    const char *blake2b_2;
    const char *mask;
    const char *block_hash;
    uint8_t asic_profile;
    uint32_t version;
    int32_t height;
    uint32_t time;
    uint32_t bits;
    uint32_t nonce;
    uint32_t nonce2;
    uint32_t nonce3;
    uint32_t time_offset;
    uint16_t tx_count;
    uint8_t flags;
    uint8_t xor_key_mask_clear_bits;
    uint32_t time_on_wire;
    const char *prev_wire;
    const char *merkle_wire;
    const char *extranonce_wire;
    const char *xor_key_wire;
    const char *mm_rhs_wire;
} test_vector;

static const test_vector vectors[] = {
    {
        "profile_0_time_offset",
        "000000a01f1e1d1c1b1a191817161514131211100f0e0d0c0b0a0908070605040302010000112233445566778899aabbccddeeff00102030405060708090a0b0c0d0e0f0a8913577ffff001d0df0ad0b44332211efcdab89ffeeddccbbaa998877665544332211005802000003001c000000000000000000000000000000000040d10c008967452301efcdab8967452301efcdab8967452301efcdab8967452301efcdab",
        "86e4855b51daf0932719011a6565a5908aef105fc6f8b85a23601de43865f4db",
        "4ff7ec7f24f6935064cb962ec8cc0c947d60621cc22c5ba8516b0b995cd0c01b",
        "ab5becb2336a3701557b0f6e33de39bd333072b8494c7c60952a8e8a636565e3",
        "7e6326906eaa52fe59e03a14f1dfb8dd5d6e78497e56a8a6e4f4fb4d385e43db",
        "4b495dcf05d70a49785b799b22284fbcd9dd1209237c53c87e4674b15587d704",
        "0000000000000000000000000000000000000000000000000000000000000000",
        "4b495dcf05d70a49785b799b22284fbcd9dd1209237c53c87e4674b15587d704",
        0u,
        UINT32_C(536870912), 840000,
        UINT32_C(2000000000), UINT32_C(486604799), UINT32_C(195948557),
        UINT32_C(287454020), UINT32_C(2309737967),
        UINT32_C(600), 3u,
        28u, 0u,
        UINT32_C(1999999400),
        "1f1e1d1c1b1a191817161514131211100f0e0d0c0b0a09080706050403020100",
        "00112233445566778899aabbccddeeff00102030405060708090a0b0c0d0e0f0",
        "ffeeddccbbaa99887766554433221100",
        "00000000000000000000000000000000",
        "8967452301efcdab8967452301efcdab8967452301efcdab8967452301efcdab"
    },
    {
        "profile_1_time_offset_nonzero_key",
        "000000a01f1e1d1c1b1a191817161514131211100f0e0d0c0b0a0908070605040302010000112233445566778899aabbccddeeff00102030405060708090a0b0c0d0e0f0a8913577ffff001d0df0ad0b44332211efcdab89ffeeddccbbaa998877665544332211005802000001001d00efcdab8967452301efcdab896745230141d10c008967452301efcdab8967452301efcdab8967452301efcdab8967452301efcdab",
        "985071be3e4adf9e7983f4d5480ff4f77d14f6dcb6d2d9623226d3f0ff7687c6",
        "b1fd91c55ba79811b18bb0b0d84d30c670c65243624cdddc510a74e0dd5a0123",
        "be70c7fd6151172efb761561f3087bd61d97ccf070a1b05ae4c5d458686523d7",
        "43cfb1efb395515321be80f8ff1961bcec5cb5b9780d552ccc46e878e27250a1",
        "1c0a823c4f532ed5d25d2c5369cf151143f2479d5109e51209a960ad5fe0e958",
        "58b901be52b9b42d058537f41dd321bdcff5ec1cfa01f899625937f7279af7b7",
        "44b383821dea9af8d7d81ba7741c34ac8c07ab81ab081d8b6bf0575a787a1eef",
        1u,
        UINT32_C(536870912), 840001,
        UINT32_C(2000000000), UINT32_C(486604799), UINT32_C(195948557),
        UINT32_C(287454020), UINT32_C(2309737967),
        UINT32_C(600), 1u,
        29u, 0u,
        UINT32_C(1999999400),
        "1f1e1d1c1b1a191817161514131211100f0e0d0c0b0a09080706050403020100",
        "00112233445566778899aabbccddeeff00102030405060708090a0b0c0d0e0f0",
        "ffeeddccbbaa99887766554433221100",
        "efcdab8967452301efcdab8967452301",
        "8967452301efcdab8967452301efcdab8967452301efcdab8967452301efcdab"
    },
    {
        "profile_2_time_offset_selector_7",
        "000000a01f1e1d1c1b1a191817161514131211100f0e0d0c0b0a0908070605040302010000112233445566778899aabbccddeeff00102030405060708090a0b0c0d0e0f0a8913577ffff001d0df0ad0bddccbbaaefcdab89ffeeddccbbaa998877665544332211005802000003001e071032547698badcfe1032547698badcfe40d10c008967452301efcdab8967452301efcdab8967452301efcdab8967452301efcdab",
        "4ead68aa92ceffdb2bc4a97b01c1ad61bcd4db55a8ce5520358024180c71f2f9",
        "c66fb3a62935bf95f948da68c7a6fc7f898ce507c15e493eaafa6d141ba787dd",
        "dbe5fcacfe44b437da5612f21eb86c75e33868b40bfd7f458a9c3ee10a2be540",
        "21e015cef9a6a323f1a5c1cdf791562447a78d2c7693ee0af9b553b80a574d19",
        "0657ae2302c06ff0d557d62fa048791441a1057bf85baeb8ec1bba8c4a3476e3",
        "00aa74c7e80a7f4889d075e84d39086b99989dd8d7ba91ff6c69919317bc5895",
        "06fddae4eaca10b85c87a3c7ed71717fd83998a32fe13f4780722b1f5d882e76",
        2u,
        UINT32_C(536870912), 840000,
        UINT32_C(2000000000), UINT32_C(486604799), UINT32_C(195948557),
        UINT32_C(2864434397), UINT32_C(2309737967),
        UINT32_C(600), 3u,
        30u, 7u,
        UINT32_C(1999999400),
        "1f1e1d1c1b1a191817161514131211100f0e0d0c0b0a09080706050403020100",
        "00112233445566778899aabbccddeeff00102030405060708090a0b0c0d0e0f0",
        "ffeeddccbbaa99887766554433221100",
        "1032547698badcfe1032547698badcfe",
        "8967452301efcdab8967452301efcdab8967452301efcdab8967452301efcdab"
    },
    {
        "profile_3_time_offset_selector_8",
        "000000a01f1e1d1c1b1a191817161514131211100f0e0d0c0b0a0908070605040302010000112233445566778899aabbccddeeff00102030405060708090a0b0c0d0e0f0a8913577ffff001d0df0ad0b44332211040302010000000000000000ffffffffffffffff5802000003001f081032547698badcfe1032547698badcfe40d10c008967452301efcdab8967452301efcdab8967452301efcdab8967452301efcdab",
        "4ead68aa92ceffdb2bc4a97b01c1ad61bcd4db55a8ce5520358024180c71f2f9",
        "151989e9cec8011a56257e54ca73f3b11e904b024597c3fb214c51ab8f9cf5bf",
        "9b10300a354d18ccf225314cae9a628c404edf068656236ecd0978875f9afa2e",
        "b7b7fa4483321ad668a2e9a98489d6d82c3aca5ce5a78db4d824b7cf06e5e217",
        "e69a31e0bb651ed5b3076ef46c9b27b5609e19407b7ff04b7c6292d0641428cd",
        "00aa74c7e80a7f4889d075e84d39086b99989dd8d7ba91ff6c69919317bc5895",
        "e6304527536f619d3ad71b1c21a22fdef9068498acc561b4100b034373a87058",
        3u,
        UINT32_C(536870912), 840000,
        UINT32_C(2000000000), UINT32_C(486604799), UINT32_C(195948557),
        UINT32_C(287454020), UINT32_C(16909060),
        UINT32_C(600), 3u,
        31u, 8u,
        UINT32_C(1999999400),
        "1f1e1d1c1b1a191817161514131211100f0e0d0c0b0a09080706050403020100",
        "00112233445566778899aabbccddeeff00102030405060708090a0b0c0d0e0f0",
        "0000000000000000ffffffffffffffff",
        "1032547698badcfe1032547698badcfe",
        "8967452301efcdab8967452301efcdab8967452301efcdab8967452301efcdab"
    },
    {
        "profile_0_time_offset_disabled_selector_255",
        "000000a01f1e1d1c1b1a191817161514131211100f0e0d0c0b0a0908070605040302010000112233445566778899aabbccddeeff00102030405060708090a0b0c0d0e0f000943577ffff001dffffffff44332211efcdab89ffeeddccbbaa9988776655443322110088776655030018ff2222222222222222111111111111111140d10c000000000000000000000000000000000000000000000000000000000000000000",
        "7bd0c09b8fe9cf1513cc141524b5bf46a4851ce06fe5bce05e04f9434f0a88a2",
        "f06e36d21af2982442bff107cf6546b4440421f841a49ed027d4fa11e3e504e6",
        "eae5d77dab38f5094ad95e848237614bd734f7fbc66f306422eccd2f009dc91e",
        "544a71e01a4c041c727e86ec7cb2c68c62d9dcab0ee9b07cdaf1a59bf2e5d40b",
        "c31b24420d67f86e524f980a24a18e88f36c821046d5288251b5d88998c69f87",
        "0000000000000000000000000000000000000000000000000000000000000001",
        "c31b24420d67f86e524f980a24a18e88f36c821046d5288251b5d88998c69f86",
        0u,
        UINT32_C(536870912), 840000,
        UINT32_C(2000000000), UINT32_C(486604799), UINT32_C(4294967295),
        UINT32_C(287454020), UINT32_C(2309737967),
        UINT32_C(1432778632), 3u,
        24u, 255u,
        UINT32_C(2000000000),
        "1f1e1d1c1b1a191817161514131211100f0e0d0c0b0a09080706050403020100",
        "00112233445566778899aabbccddeeff00102030405060708090a0b0c0d0e0f0",
        "ffeeddccbbaa99887766554433221100",
        "22222222222222221111111111111111",
        "0000000000000000000000000000000000000000000000000000000000000000"
    }
};

static void test_header_vectors(void)
{
    size_t n;

    for (n = 0; n < sizeof(vectors) / sizeof(vectors[0]); n++) {
        const test_vector *v = &vectors[n];
        uint8_t wire[XBT_BLOCK_HEADER_V2_SIZE];
        uint8_t reserialized[XBT_BLOCK_HEADER_V2_SIZE];
        uint8_t hash[32];
        xbt_block_header_v2 hdr;
        xbt_block_header_v2_hash_components c;

        CHECK(hex_decode(v->serialized, wire, sizeof(wire)) == 0);
        CHECK(xbt_block_header_is_v2(wire, sizeof(wire)) == 1);
        CHECK(xbt_block_header_v2_parse(&hdr, wire, sizeof(wire)) == 0);

        check_u32(v->name, "version", hdr.version, v->version);
        check_u32(v->name, "complete_version",
                  xbt_block_header_v2_complete_version(&hdr),
                  XBT_BLOCK_HEADER_V2_FLAG | v->version);
        check_i32(v->name, "height", hdr.height, v->height);
        check_u32(v->name, "time", hdr.time, v->time);
        check_u32(v->name, "time_on_wire",
                  xbt_block_header_v2_time_on_wire(&hdr), v->time_on_wire);
        check_u32(v->name, "bits", hdr.bits, v->bits);
        check_u32(v->name, "nonce", hdr.nonce, v->nonce);
        check_u32(v->name, "nonce2", hdr.nonce2, v->nonce2);
        check_u32(v->name, "nonce3", hdr.nonce3, v->nonce3);
        check_u32(v->name, "time_offset", hdr.time_offset, v->time_offset);
        check_u32(v->name, "tx_count", hdr.tx_count, v->tx_count);
        check_u32(v->name, "flags", hdr.flags, v->flags);
        check_u32(v->name, "xor_key_mask_clear_bits",
                  hdr.xor_key_mask_clear_bits, v->xor_key_mask_clear_bits);

        check_bytes(v->name, "prev_block", hdr.prev_block,
                    v->prev_wire, sizeof(hdr.prev_block));
        check_bytes(v->name, "merkle_root", hdr.merkle_root,
                    v->merkle_wire, sizeof(hdr.merkle_root));
        check_bytes(v->name, "extranonce", hdr.extranonce,
                    v->extranonce_wire, sizeof(hdr.extranonce));
        check_bytes(v->name, "xor_key", hdr.xor_key,
                    v->xor_key_wire, sizeof(hdr.xor_key));
        check_bytes(v->name, "mm_rhs", hdr.mm_rhs,
                    v->mm_rhs_wire, sizeof(hdr.mm_rhs));

        CHECK(xbt_block_header_v2_serialize(&hdr, reserialized) == 0);
        CHECK(memcmp(wire, reserialized, sizeof(wire)) == 0);

        CHECK(xbt_block_header_v2_hash_ex(&hdr, &c) == 0);
        CHECK(c.asic_profile == v->asic_profile);
        check_bytes(v->name, "xor_key_hash", c.xor_key_hash,
                    v->xor_key_hash, sizeof(c.xor_key_hash));
        check_bytes(v->name, "h1", c.h1, v->h1, sizeof(c.h1));
        check_bytes(v->name, "h2", c.h2, v->h2, sizeof(c.h2));
        check_bytes(v->name, "hash1", c.hash1,
                    v->blake2b_1, sizeof(c.hash1));
        check_bytes(v->name, "hash2", c.hash2,
                    v->blake2b_2, sizeof(c.hash2));
        check_bytes(v->name, "mask", c.mask, v->mask, sizeof(c.mask));
        check_bytes(v->name, "hash", c.hash,
                    v->block_hash, sizeof(c.hash));

        CHECK(xbt_block_header_v2_hash(&hdr, hash) == 0);
        CHECK(memcmp(hash, c.hash, sizeof(hash)) == 0);

        /* Published vectors are not mined; all fail nBits 0x1d00ffff. */
        CHECK(xbt_block_header_v2_check_pow(&hdr,
                                            XBT_POW_LIMIT_MAINNET) == 0);

        if (failures == 0) {
            printf("ok: header vector %s\n", v->name);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Structural tests.                                                          */
/* ------------------------------------------------------------------------- */

static void test_structure(void)
{
    uint8_t wire[XBT_BLOCK_HEADER_V2_SIZE];
    uint8_t longer[XBT_BLOCK_HEADER_V2_SIZE + 16];
    xbt_block_header_v2 hdr;
    xbt_block_header_v2 back;
    size_t i;

    memset(wire, 0, sizeof(wire));
    CHECK(xbt_block_header_is_v2(wire, sizeof(wire)) == 0);
    CHECK(xbt_block_header_is_v2(wire, 3) == -1);
    CHECK(xbt_block_header_is_v2(NULL, sizeof(wire)) == -1);
    CHECK(xbt_block_header_v2_parse(&hdr, wire, sizeof(wire)) == -1);

    wire[3] = 0x80; /* complete version 0x80000000: v2, low version 0 */
    CHECK(xbt_block_header_is_v2(wire, sizeof(wire)) == 1);
    CHECK(xbt_block_header_v2_parse(&hdr, wire, sizeof(wire)) == 0);
    CHECK(hdr.version == 0);
    CHECK(xbt_block_header_v2_parse(&hdr, wire, sizeof(wire) - 1) == -1);

    memset(longer, 0, sizeof(longer));
    longer[3] = 0x80;
    CHECK(xbt_block_header_v2_parse(&hdr, longer, sizeof(longer)) == 0);

    CHECK(xbt_block_header_v2_parse(NULL, wire, sizeof(wire)) == -1);
    CHECK(xbt_block_header_v2_parse(&hdr, NULL, sizeof(wire)) == -1);
    CHECK(xbt_block_header_v2_serialize(NULL, wire) == -1);
    CHECK(xbt_block_header_v2_serialize(&hdr, NULL) == -1);
    CHECK(xbt_block_header_v2_hash(NULL, wire) == -1);
    CHECK(xbt_block_header_v2_hash(&hdr, NULL) == -1);

    /* Full round-trip with every field exercised. */
    memset(&hdr, 0, sizeof(hdr));
    hdr.version = UINT32_C(0x01234567);
    hdr.height = -12345;
    for (i = 0; i < sizeof(hdr.prev_block); i++) {
        hdr.prev_block[i] = (uint8_t)(0x10u + i);
        hdr.merkle_root[i] = (uint8_t)(0x80u + i);
        hdr.mm_rhs[i] = (uint8_t)(0xf0u - i);
    }
    for (i = 0; i < sizeof(hdr.extranonce); i++) {
        hdr.extranonce[i] = (uint8_t)(0x30u + i);
        hdr.xor_key[i] = (uint8_t)(0x60u + i);
    }
    hdr.time = UINT32_C(1700000000);
    hdr.bits = UINT32_C(0x1d00ffff);
    hdr.nonce = UINT32_C(0xdeadbeef);
    hdr.nonce2 = UINT32_C(0x11223344);
    hdr.nonce3 = UINT32_C(0x55667788);
    hdr.time_offset = UINT32_C(600);
    hdr.tx_count = UINT16_C(4242);
    hdr.flags = XBT_BLOCK_HEADER_FLAG_USE_TIME_OFFSET | 2u;
    hdr.xor_key_mask_clear_bits = 13;

    check_u32("roundtrip", "complete_version",
              xbt_block_header_v2_complete_version(&hdr),
              UINT32_C(0x81234567));
    check_u32("roundtrip", "time_on_wire",
              xbt_block_header_v2_time_on_wire(&hdr),
              UINT32_C(1700000000 - 600));

    CHECK(xbt_block_header_v2_serialize(&hdr, wire) == 0);
    CHECK(xbt_block_header_v2_parse(&back, wire, sizeof(wire)) == 0);
    CHECK(back.version == hdr.version);
    CHECK(back.height == hdr.height);
    CHECK(back.time == hdr.time);
    CHECK(back.bits == hdr.bits);
    CHECK(back.nonce == hdr.nonce);
    CHECK(back.nonce2 == hdr.nonce2);
    CHECK(back.nonce3 == hdr.nonce3);
    CHECK(back.time_offset == hdr.time_offset);
    CHECK(back.tx_count == hdr.tx_count);
    CHECK(back.flags == hdr.flags);
    CHECK(back.xor_key_mask_clear_bits == hdr.xor_key_mask_clear_bits);
    CHECK(memcmp(back.prev_block, hdr.prev_block, 32) == 0);
    CHECK(memcmp(back.merkle_root, hdr.merkle_root, 32) == 0);
    CHECK(memcmp(back.extranonce, hdr.extranonce, 16) == 0);
    CHECK(memcmp(back.xor_key, hdr.xor_key, 16) == 0);
    CHECK(memcmp(back.mm_rhs, hdr.mm_rhs, 32) == 0);

    /* With the time-offset flag clear, the wire time is the full time. */
    hdr.flags = 2u;
    check_u32("roundtrip", "time_on_wire_no_flag",
              xbt_block_header_v2_time_on_wire(&hdr), hdr.time);
}

/* ------------------------------------------------------------------------- */
/* Compact target and PoW checks.                                             */
/* ------------------------------------------------------------------------- */

static void test_pow(void)
{
    uint8_t target[32];
    uint8_t expected[32];
    uint8_t regtest_limit[32];
    uint8_t above[32];
    uint8_t zeros[32];
    xbt_block_header_v2 hdr;

    /* 0x1d00ffff expands to 0x00000000ffff0000...0000. */
    memset(expected, 0, sizeof(expected));
    expected[4] = 0xff;
    expected[5] = 0xff;
    CHECK(xbt_pow_target_from_bits(UINT32_C(0x1d00ffff),
                                   XBT_POW_LIMIT_MAINNET, target) == 0);
    CHECK(memcmp(target, expected, sizeof(target)) == 0);

    /* Regtest limit 0x7fff... and nBits 0x207fffff. */
    memset(regtest_limit, 0xff, sizeof(regtest_limit));
    regtest_limit[0] = 0x7f;
    memset(expected, 0, sizeof(expected));
    expected[0] = 0x7f;
    expected[1] = 0xff;
    expected[2] = 0xff;
    CHECK(xbt_pow_target_from_bits(UINT32_C(0x207fffff),
                                   regtest_limit, target) == 0);
    CHECK(memcmp(target, expected, sizeof(target)) == 0);

    /* Invalid compact encodings. */
    CHECK(xbt_pow_target_from_bits(UINT32_C(0x1d80ffff),
                                   XBT_POW_LIMIT_MAINNET, target) == -1);
    CHECK(xbt_pow_target_from_bits(UINT32_C(0x23000001),
                                   XBT_POW_LIMIT_MAINNET, target) == -1);
    CHECK(xbt_pow_target_from_bits(UINT32_C(0x2100ffff),
                                   XBT_POW_LIMIT_MAINNET, target) == -1);
    CHECK(xbt_pow_target_from_bits(UINT32_C(0x1d000000),
                                   XBT_POW_LIMIT_MAINNET, target) == -1);
    /* Non-zero mantissa that shifts away to a zero target. */
    CHECK(xbt_pow_target_from_bits(UINT32_C(0x0069c699),
                                   XBT_POW_LIMIT_MAINNET, target) == -1);
    CHECK(xbt_pow_target_from_bits(UINT32_C(0x020000ff),
                                   XBT_POW_LIMIT_MAINNET, target) == -1);
    CHECK(xbt_pow_target_from_bits(0, XBT_POW_LIMIT_MAINNET, target) == -1);
    CHECK(xbt_pow_target_from_bits(UINT32_C(0x1d00ffff),
                                   NULL, target) == -1);
    CHECK(xbt_pow_target_from_bits(UINT32_C(0x1d00ffff),
                                   XBT_POW_LIMIT_MAINNET, NULL) == -1);

    /* Boundary: hash == target passes, target + 1 fails. */
    CHECK(xbt_pow_target_from_bits(UINT32_C(0x1d00ffff),
                                   XBT_POW_LIMIT_MAINNET, target) == 0);
    CHECK(xbt_pow_hash_meets_target(target, target) == 1);
    memcpy(above, target, sizeof(above));
    above[31]++;
    CHECK(xbt_pow_hash_meets_target(above, target) == 0);
    memset(zeros, 0, sizeof(zeros));
    CHECK(xbt_pow_hash_meets_target(zeros, target) == 1);
    CHECK(xbt_pow_hash_meets_target(NULL, target) == -1);
    CHECK(xbt_pow_hash_meets_target(target, NULL) == -1);

    /* A header carrying invalid nBits cannot produce a PoW verdict. */
    memset(&hdr, 0, sizeof(hdr));
    hdr.bits = UINT32_C(0x1d80ffff);
    CHECK(xbt_block_header_v2_check_pow(&hdr,
                                        XBT_POW_LIMIT_MAINNET) == -1);
    hdr.bits = 0;
    CHECK(xbt_block_header_v2_check_pow(&hdr,
                                        XBT_POW_LIMIT_MAINNET) == -1);
    CHECK(xbt_block_header_v2_check_pow(NULL,
                                        XBT_POW_LIMIT_MAINNET) == -1);
    CHECK(xbt_block_header_v2_check_pow(&hdr, NULL) == -1);
}

int main(void)
{
    test_blake2b();
    printf("ok: BLAKE2b-512 RFC 7693 vectors and streaming\n");
    test_header_vectors();
    test_structure();
    printf("ok: v2 parse/serialize structural tests\n");
    test_pow();
    printf("ok: compact target and PoW boundary tests\n");

    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all xbt tests passed\n");
    return 0;
}
