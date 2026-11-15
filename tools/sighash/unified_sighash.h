#ifndef KERN_UNIFIED_SIGHASH_H
#define KERN_UNIFIED_SIGHASH_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint64_t value;
  const uint8_t *script;
  size_t script_len;
} ush_spent;

/*
 * Unified opt-in sighash (Bitcoin Knots layout, tagged SHA256 for the
 * reference vectors).  script_code is required for types 0/1; for type 3
 * it is the tapscript leaf script.  No annex/codeseparator in the vectors.
 *
 * Returns 0 on success, -1 on a parse/argument error (message in err).
 */
int ush_sighash(const uint8_t *rawtx, size_t rawtx_len, unsigned in_idx,
                unsigned script_type, unsigned hash_type,
                const uint8_t *script_code, size_t script_code_len,
                const ush_spent *spent, size_t nspent, uint8_t out[32],
                char *err, size_t errlen);

#endif
