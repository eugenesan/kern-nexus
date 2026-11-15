#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unified_sighash.h"

struct tvec_spent { uint64_t value; const char *script; };
struct tvec { const char *script_code; const char *rawtx;
  unsigned in_idx, hash_type, script_type; size_t nspent;
  struct tvec_spent spent[4]; const char *expected; };
extern const struct tvec tvecs[];
extern const size_t tvec_count;

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static unsigned char *unhex(const char *s, size_t *len) {
  size_t n = strlen(s) / 2;
  unsigned char *out = malloc(n ? n : 1);
  if (!out) exit(2);
  for (size_t i = 0; i < n; i++)
    out[i] = (unsigned char)((hexval(s[2 * i]) << 4) | hexval(s[2 * i + 1]));
  *len = n;
  return out;
}

static void printhex(const unsigned char *p, size_t n) {
  for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
}

int main(void) {
  int failed = 0;
  for (size_t i = 0; i < tvec_count; i++) {
    const struct tvec *v = &tvecs[i];
    size_t raw_len, sc_len = 0;
    unsigned char *raw = unhex(v->rawtx, &raw_len);
    unsigned char *sc = v->script_code[0] ? unhex(v->script_code, &sc_len) : NULL;
    ush_spent sp[4];
    unsigned char *scripts[4] = {0};
    for (size_t j = 0; j < v->nspent; j++) {
      size_t sl;
      scripts[j] = unhex(v->spent[j].script, &sl);
      sp[j].value = v->spent[j].value;
      sp[j].script = scripts[j];
      sp[j].script_len = sl;
    }
    unsigned char got[32];
    char err[128] = {0};
    int rc = ush_sighash(raw, raw_len, v->in_idx, v->script_type, v->hash_type,
                         sc, sc_len, sp, v->nspent, got, err, sizeof(err));
    int ok = rc == 0;
    if (ok) {
      char hex[65];
      for (int k = 0; k < 32; k++) snprintf(hex + 2 * k, 3, "%02x", got[k]);
      ok = strcmp(hex, v->expected) == 0;
    }
    if (!ok) {
      failed++;
      printf("FAIL vector %zu type=%u hash=%#x idx=%u: %s\n", i, v->script_type,
             v->hash_type, v->in_idx, rc ? err : "digest mismatch");
      if (!rc) {
        printf("  expected %s\n  got      ", v->expected);
        printhex(got, 32);
        printf("\n");
      }
    }
    for (size_t j = 0; j < v->nspent; j++) free(scripts[j]);
    free(sc);
    free(raw);
  }
  printf("%zu vectors, %d failed\n", tvec_count, failed);
  return failed ? 1 : 0;
}
