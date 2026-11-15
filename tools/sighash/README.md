# tools/sighash

The unified opt-in sighash reference and vector test (Bitcoin Knots /
tagged SHA256).  166 embit vectors pass.

The firmware source is no longer here: it lives in
`kern/main/core/unified_sighash.{c,h}` and
`kern/main/core/unified_sha256.{c,h}`.  This test compiles those files, so
it validates exactly the code that is linked into kern.

```sh
make -C tools/sighash
# 166 vectors, 0 failed
```

| file | purpose |
|---|---|
| `vectors.c` | generated vector table |
| `test_unified.c` | runner |
| `gen_vectors.py` | regenerates `vectors.c` |

## Where it is wired

* `kern/main/core/psbt.c` computes the digest and signs it with libwally's
  EC primitives; there is **no libwally patch**.
* `psbt_sighash_is_supported()` accepts `UNIFIED|ALL` (`0x21`) and the
  review screen labels it `UNIFIED|ALL`.
* ECDSA (legacy, P2WPKH, P2WSH, P2SH) and taproot key-path and script-path
  inputs sign with the unified digest.
* The signing test in `kern/main/core/test/test_psbt_classify.c` signs a
  P2WPKH and a BIP86 PSBT and verifies each signature under the expected
  digest (`146 passed`).

Why not libwally: its signature-hash dispatch, key matching and taproot
branches all assume BIP143/BIP341.  Adding a fourth type there would mean
carrying a fork of a pinned submodule.  Kern already has the per-input
classification and PSBT map access, so the digest and the small signing
glue stay there; libwally remains the primitive library.

The vectors are byte-identical in the SeedSigner reference and the Krux
vendored embit:

```
/home/user/Develop/src/krux-mobile/seedsigner  (branch privkeio/unified-sighash,
  tests/unified/data/unified_sighash.json)
/home/user/Develop/src/krux-nexus/krux/vendor/embit/tests/tests/data/unified_sighash.json
```

`gen_vectors.py` takes the path as an argument; regenerate after an embit
update.
