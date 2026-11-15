#include "unified_sighash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sha256.h"

typedef struct {
  const uint8_t *txid; /* wire order (as serialized) */
  uint32_t vout;
  uint32_t sequence;
} ush_in;

typedef struct {
  uint64_t value;
  const uint8_t *script;
  size_t script_len;
} ush_out;

typedef struct {
  uint32_t version;
  uint32_t locktime;
  ush_in *in;
  size_t nin;
  ush_out *out;
  size_t nout;
} ush_tx;

static int fail(char *err, size_t errlen, const char *msg) {
  if (err && errlen)
    snprintf(err, errlen, "%s", msg);
  return -1;
}

static int read_u32(const uint8_t **p, const uint8_t *end, uint32_t *v) {
  if ((size_t)(end - *p) < 4)
    return -1;
  *v = (uint32_t)(*p)[0] | (uint32_t)(*p)[1] << 8 | (uint32_t)(*p)[2] << 16 |
       (uint32_t)(*p)[3] << 24;
  *p += 4;
  return 0;
}

static int read_u64(const uint8_t **p, const uint8_t *end, uint64_t *v) {
  uint32_t lo, hi;
  if (read_u32(p, end, &lo) || read_u32(p, end, &hi))
    return -1;
  *v = (uint64_t)lo | (uint64_t)hi << 32;
  return 0;
}

static int read_varint(const uint8_t **p, const uint8_t *end, uint64_t *v) {
  if (*p >= end)
    return -1;
  uint8_t n = *(*p)++;
  if (n < 0xfd) {
    *v = n;
    return 0;
  }
  if (n == 0xfd) {
    if (end - *p < 2)
      return -1;
    *v = (uint64_t)(*p)[0] | (uint64_t)(*p)[1] << 8;
    *p += 2;
    return 0;
  }
  if (n == 0xfe) {
    uint32_t x;
    if (read_u32(p, end, &x))
      return -1;
    *v = x;
    return 0;
  }
  if (end - *p < 8)
    return -1;
  *v = 0;
  for (int i = 0; i < 8; i++)
    *v |= (uint64_t)(*p)[i] << (8 * i);
  *p += 8;
  return 0;
}

static int read_bytes(const uint8_t **p, const uint8_t *end, const uint8_t **out,
                      size_t *len) {
  uint64_t n;
  if (read_varint(p, end, &n) || n > (uint64_t)(end - *p))
    return -1;
  *out = *p;
  *len = (size_t)n;
  *p += n;
  return 0;
}

static int parse_tx(const uint8_t *raw, size_t len, ush_tx *tx) {
  const uint8_t *p = raw, *end = raw + len;
  uint64_t n;
  memset(tx, 0, sizeof(*tx));
  if (read_u32(&p, end, &tx->version))
    return -1;
  if (read_varint(&p, end, &n))
    return -1;
  int segwit = 0;
  if (n == 0) {
    if (p >= end || *p++ != 1)
      return -1;
    segwit = 1;
    if (read_varint(&p, end, &n))
      return -1;
  }
  if (n > 100000)
    return -1;
  tx->nin = (size_t)n;
  tx->in = calloc(tx->nin ? tx->nin : 1, sizeof(*tx->in));
  if (!tx->in)
    return -1;
  for (size_t i = 0; i < tx->nin; i++) {
    if (end - p < 32)
      return -1;
    tx->in[i].txid = p;
    p += 32;
    if (read_u32(&p, end, &tx->in[i].vout))
      return -1;
    const uint8_t *script;
    size_t script_len;
    if (read_bytes(&p, end, &script, &script_len))
      return -1;
    if (read_u32(&p, end, &tx->in[i].sequence))
      return -1;
  }
  if (segwit) {
    for (size_t i = 0; i < tx->nin; i++) {
      uint64_t items;
      if (read_varint(&p, end, &items) || items > 1000)
        return -1;
      for (uint64_t j = 0; j < items; j++) {
        const uint8_t *item;
        size_t item_len;
        if (read_bytes(&p, end, &item, &item_len))
          return -1;
      }
    }
  }
  if (read_varint(&p, end, &n) || n > 100000)
    return -1;
  tx->nout = (size_t)n;
  tx->out = calloc(tx->nout ? tx->nout : 1, sizeof(*tx->out));
  if (!tx->out)
    return -1;
  for (size_t i = 0; i < tx->nout; i++) {
    const uint8_t *script;
    size_t script_len;
    if (read_u64(&p, end, &tx->out[i].value) ||
        read_bytes(&p, end, &script, &script_len))
      return -1;
    tx->out[i].script = script;
    tx->out[i].script_len = script_len;
  }
  if (read_u32(&p, end, &tx->locktime))
    return -1;
  return 0;
}

static void free_tx(ush_tx *tx) {
  free(tx->in);
  free(tx->out);
  tx->in = NULL;
  tx->out = NULL;
}

static void put_u32(sha256_ctx *h, uint32_t v) {
  uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                  (uint8_t)(v >> 24)};
  sha256_update(h, b, 4);
}

static void put_u64(sha256_ctx *h, uint64_t v) {
  for (int i = 0; i < 8; i++)
    sha256_update(h, &(uint8_t){(uint8_t)(v >> (8 * i))}, 1);
}

static void put_varint(sha256_ctx *h, uint64_t v) {
  uint8_t b[9];
  size_t n;
  if (v < 0xfd) {
    b[0] = (uint8_t)v;
    n = 1;
  } else if (v <= 0xffff) {
    b[0] = 0xfd;
    b[1] = (uint8_t)v;
    b[2] = (uint8_t)(v >> 8);
    n = 3;
  } else if (v <= 0xffffffffu) {
    b[0] = 0xfe;
    for (int i = 0; i < 4; i++)
      b[1 + i] = (uint8_t)(v >> (8 * i));
    n = 5;
  } else {
    b[0] = 0xff;
    for (int i = 0; i < 8; i++)
      b[1 + i] = (uint8_t)(v >> (8 * i));
    n = 9;
  }
  sha256_update(h, b, n);
}

static void put_script(sha256_ctx *h, const uint8_t *script, size_t len) {
  put_varint(h, len);
  if (len)
    sha256_update(h, script, len);
}

static void hash_prevouts(const ush_tx *tx, uint8_t out[32]) {
  sha256_ctx h;
  sha256_init(&h);
  for (size_t i = 0; i < tx->nin; i++) {
    sha256_update(&h, tx->in[i].txid, 32);
    put_u32(&h, tx->in[i].vout);
  }
  sha256_final(&h, out);
}

static void hash_amounts(const ush_spent *spent, size_t n, uint8_t out[32]) {
  sha256_ctx h;
  sha256_init(&h);
  for (size_t i = 0; i < n; i++)
    put_u64(&h, spent[i].value);
  sha256_final(&h, out);
}

static void hash_script_pubkeys(const ush_spent *spent, size_t n,
                                uint8_t out[32]) {
  sha256_ctx h;
  sha256_init(&h);
  for (size_t i = 0; i < n; i++)
    put_script(&h, spent[i].script, spent[i].script_len);
  sha256_final(&h, out);
}

static void hash_sequence(const ush_tx *tx, uint8_t out[32]) {
  sha256_ctx h;
  sha256_init(&h);
  for (size_t i = 0; i < tx->nin; i++)
    put_u32(&h, tx->in[i].sequence);
  sha256_final(&h, out);
}

static void hash_outputs(const ush_tx *tx, uint8_t out[32]) {
  sha256_ctx h;
  sha256_init(&h);
  for (size_t i = 0; i < tx->nout; i++) {
    put_u64(&h, tx->out[i].value);
    put_script(&h, tx->out[i].script, tx->out[i].script_len);
  }
  sha256_final(&h, out);
}

static void hash_output(const ush_out *out, uint8_t h[32]) {
  sha256_ctx ctx;
  sha256_init(&ctx);
  put_u64(&ctx, out->value);
  put_script(&ctx, out->script, out->script_len);
  sha256_final(&ctx, h);
}

static void tapleaf_hash(const uint8_t *script, size_t len, uint8_t out[32]) {
  sha256_ctx h;
  sha256_tagged_init(&h, "TapLeaf");
  uint8_t version = 0xC0;
  sha256_update(&h, &version, 1);
  put_script(&h, script, len);
  sha256_final(&h, out);
}

int ush_sighash(const uint8_t *rawtx, size_t rawtx_len, unsigned in_idx,
                unsigned script_type, unsigned hash_type,
                const uint8_t *script_code, size_t script_code_len,
                const ush_spent *spent, size_t nspent, uint8_t out[32],
                char *err, size_t errlen) {
  ush_tx tx;
  sha256_ctx h;
  uint8_t tmp[32];
  unsigned sh = hash_type & 0x1f;
  int acp = (hash_type & 0x80) != 0;
  int rc = -1;

  if (parse_tx(rawtx, rawtx_len, &tx) != 0)
    return fail(err, errlen, "bad transaction");
  if (in_idx >= tx.nin) {
    fail(err, errlen, "input index out of range");
    goto done;
  }
  if (nspent != tx.nin) {
    fail(err, errlen, "spent output count does not match input count");
    goto done;
  }
  if ((script_type == 0 || script_type == 1) && !script_code) {
    fail(err, errlen, "script_code required");
    goto done;
  }
  if (script_type > 3) {
    fail(err, errlen, "bad script type");
    goto done;
  }

  sha256_tagged_init(&h, "UnifiedSighash");
  sha256_update(&h, (const uint8_t *)"\x00", 1);
  {
    uint8_t ht = (uint8_t)hash_type;
    sha256_update(&h, &ht, 1);
  }
  put_u32(&h, tx.version);
  put_u32(&h, tx.locktime);
  sha256_update(&h, (const uint8_t *)"\x00", 1);

  if (!acp) {
    hash_prevouts(&tx, tmp);
    sha256_update(&h, tmp, 32);
    hash_amounts(spent, nspent, tmp);
    sha256_update(&h, tmp, 32);
    hash_script_pubkeys(spent, nspent, tmp);
    sha256_update(&h, tmp, 32);
    hash_sequence(&tx, tmp);
    sha256_update(&h, tmp, 32);
  }
  if (sh != 2 && sh != 3) {
    hash_outputs(&tx, tmp);
    sha256_update(&h, tmp, 32);
  }
  {
    uint8_t st = (uint8_t)script_type;
    sha256_update(&h, &st, 1);
  }
  if (acp) {
    sha256_update(&h, tx.in[in_idx].txid, 32);
    put_u32(&h, tx.in[in_idx].vout);
    put_u64(&h, spent[in_idx].value);
    put_script(&h, spent[in_idx].script, spent[in_idx].script_len);
    put_u32(&h, tx.in[in_idx].sequence);
  } else {
    put_u32(&h, in_idx);
  }
  if (script_type == 0 || script_type == 1) {
    put_script(&h, script_code, script_code_len);
  } else {
    sha256_update(&h, (const uint8_t *)"\x00", 1);
  }
  /* The SINGLE output hash goes here, before the tapscript fields. */
  if (sh == 3) {
    if (in_idx >= tx.nout) {
      fail(err, errlen, "SIGHASH_SINGLE with no matching output");
      goto done;
    }
    hash_output(&tx.out[in_idx], tmp);
    sha256_update(&h, tmp, 32);
  }
  if (script_type == 3) {
    if (!script_code) {
      fail(err, errlen, "tapscript leaf required");
      goto done;
    }
    tapleaf_hash(script_code, script_code_len, tmp);
    sha256_update(&h, tmp, 32);
    sha256_update(&h, (const uint8_t *)"\x00", 1);
    static const uint8_t ff[4] = {0xff, 0xff, 0xff, 0xff};
    sha256_update(&h, ff, 4);
  }
  sha256_final(&h, out);
  rc = 0;

done:
  free_tx(&tx);
  return rc;
}
