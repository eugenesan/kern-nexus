# components/xbt

XBT ("blake2b Bitcoin") block-header and proof-of-work support, separate
from the unified signature hash.

The **signature hash** remains the tagged-SHA256 message in `tools/sighash/`
and `docs/bitcoin.md`.  This component only implements the XBT chain's
block/PoW rules.

## Spec finding

The draft URL `doc/blake2b.md` in Bitcoin Knots v29.4.1 does not exist.  The
implementation was derived from the same tag's authoritative sources:

- `src/primitives/block.h` / `block.cpp` — v2 header layout and
  `CBlockHeader::GetHash()`;
- `src/pow.cpp` — `DeriveTarget` / `CheckProofOfWorkImpl`;
- `src/test/data/block_header_v2.json` — five official vectors;
- `test/functional/feature_powchange.py` and
  `test_framework/messages.py` — independent Python reference;
- `doc/unified-sighash.md` — shared activation; it does not define the
  header.

## Layout

The v2 header is **164 bytes** with more fields than the old 80-byte
header: version, previous hash, merkle root, time, bits, nonce, nonce2,
nonce3, extranonce, time offset, tx count, flags, xor-key clear bits,
xor key, height and merge-mining RHS.  The PoW hash is a multi-stage
tagged-SHA256/BLAKE2b construction with four hardware profiles and an
optional XOR mask, not a plain BLAKE2b over the serialized bytes.

## Tests

```sh
cd components/xbt/test
make test
```

Passing:

- RFC 7693 BLAKE2b-512 vectors and streaming/one-shot equivalence;
- all five official v2 block-header vectors, including the intermediate
  hashes, xor key, mask, ASIC profile and final hash;
- parse/serialize, signed height, time offset, malformed input;
- compact-target/PoW boundary cases.

The five vectors are hash vectors, not mined blocks, so they do not pass
PoW at `0x1d00ffff`.  The PoW acceptance path is tested structurally; no
expected hash was invented.

## Not yet wired

- `Blake2bHeight` / `DEPLOYMENT_BLAKE2B` activation constants (not in the
  fetched material);
- network/address selection and PSBT marking for XBT;
- integration into kern's chain selection UI.

These need the XBT network parameters or a chain-config source before
they can be made authoritative.
