# tools/sighash

Standalone reference implementation and vector test for the unified
opt-in sighash (Bitcoin Knots / tagged SHA256).  166 embit vectors pass.

```sh
make -C tools/sighash
# 166 vectors, 0 failed
```

Files:

| file | purpose |
|---|---|
| `sha256.c/h` | small SHA256 + tagged/double-SHA256 helpers |
| `unified_sighash.c/h` | transaction parser and unified digest |
| `gen_vectors.py` | generates `vectors.c` from `unified_sighash.json` |
| `vectors.c` | generated vector table |
| `test_unified.c` | runner |

This is the **bootstrapping reference**, not the final libwally code.
Next steps:

1. Port the same logic into `libwally-core` as
   `wally_tx_get_unified_signature_hash()` and the PSBT wrapper.
2. Add a BLAKE2b backend for XBT; keep the SHA256 path unchanged.
3. Validate the libwally test against these 166 vectors before signing on
   hardware.

The vectors are byte-identical in the SeedSigner reference and the Krux
vendored embit:

```
/home/user/Develop/src/krux-mobile/seedsigner  (branch privkeio/unified-sighash,
  tests/unified/data/unified_sighash.json)
/home/user/Develop/src/krux-nexus/krux/vendor/embit/tests/tests/data/unified_sighash.json
```

`gen_vectors.py` takes the path as an argument; regenerate after an embit
update.
