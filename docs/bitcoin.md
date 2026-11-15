# Bitcoin features: unified sighash, PSBT, SeedKeeper, parity

## 1. Unified opt-in sighash (Bitcoin Knots 0x20)

The unified opt-in is an **additional** signing mechanism.  It does not
replace standard Bitcoin:

* standard Bitcoin keeps SHA256d / BIP143 / BIP341 exactly as today;
* the unified signature hash itself is **TaggedHash("UnifiedSighash", ...)**
  with SHA256, exactly as the Knots spec says;
* XBT ("blake2b Bitcoin") adds a **BLAKE2b block-header/proof-of-work**
  change and activates the unified sighash at the same buried deployment
  (`Blake2bHeight` / `DEPLOYMENT_BLAKE2B`);
* the two block structures and the two signing paths coexist; the signer
  selects the chain rules, not a UI toggle.

This is why the earlier "BLAKE2b sighash" wording was wrong: BLAKE2b is the
XBT **block/PoW** change, not the unified sighash digest.  The seedsigner
tests name the activation with `-testactivationheight=blake2b@...`; the
signatures they produce still use the tagged-SHA256 message.

The reference implementation is the **SeedSigner fork**:

* `/home/user/Develop/src/krux-mobile/seedsigner`, branch
  `privkeio/unified-sighash` (also `privkeyio/unified-sighash`);
* pinned `privkeyio/embit` commit `5f700aa3222da65c27331df50b9a899236cb5461`
  (`unified-sighash-v0.8.0`), with the same code later rebased to v0.8.1 /
  v0.8.7 branches;
* `tests/unified/data/unified_sighash.json`, `test_vectors.py`,
  `knots_interop.py`, `cve_2020_14199.py`, and the full UI flow test;
* `unified-sighash.patch` is the complete app-side patch.

The `krux/vendor/embit` branch `de0e363` in `krux-nexus` carries the same
tagged-SHA256 digest and the 166 vectors, but the SeedSigner tree is the
cleaner source for a new port because it includes the Knots interop, CVE
and end-to-end tests.  Re-port from there for kern-nexus rather than
carrying the Krux patch forward.

References:

* specification: Bitcoin Knots `doc/unified-sighash.md`
  (v29.4.1.knots20260508);
* implementation: `krux/vendor/embit` branch `unified-sighash-v0.8.1`,
  commits `fff7ffa..de0e363`;
* UI policy: `krux/src/krux/sighash.py`;
* vectors/tests: `krux/vendor/embit/tests/tests/data/unified_sighash.json`,
  `test_unified_sighash.py`, `test_unified_sighash_negotiation.py`.

### 1.1 Opt-in and script types

```
SIGHASH.UNIFIED = 0x20
SIGHASH.ALL     = 0x01
SIGHASH.NONE    = 0x02
SIGHASH.SINGLE  = 0x03
SIGHASH.ANYONECANPAY = 0x80

UNIFIED_SCRIPT_TYPE:
  0 BARE       bare / P2SH
  1 WITNESS_V0 P2WPKH / P2WSH
  2 TAPROOT    key path
  3 TAPSCRIPT  script path
```

The unified algorithm is selected only when the hash type byte has `0x20`.
Standard BIP143/BIP341 paths remain untouched otherwise.

Validation (`SIGHASH.check_unified`) must be replicated exactly:

* hash type is 0..0xff (consensus reads only the last signature byte);
* `0x20` must be set;
* `ANYONECANPAY` is `0x80`;
* `sh = type & 0x1f`;
* for taproot/tapscript (`script_type` 2/3): reject bits outside
  `0x1f | 0x20 | 0x80`, and accept only `ALL`, `NONE`, `SINGLE`;
* for bare/witness-v0 the legacy reading applies (anything not `NONE` or
  `SINGLE` means `ALL`).

### 1.2 Digest construction

For input `i`, script type `st`, all spent `values[]` and
`script_pubkeys[]`:

```
h = tagged_hash_init("UnifiedSighash", "")
h.update(0x00)                       # epoch
h.update(sighash)                    # one byte, as the signature carries it
h.update(version.to_le32())
h.update(locktime.to_le32())
h.update(0x00)                       # fifth locktime byte (future extension)

if not anyonecanpay:
    h.update(hash_prevouts())        # sha256d of outpoints
    h.update(hash_amounts(values))   # sha256d of u64 amounts
    h.update(hash_script_pubkeys(script_pubkeys))
    h.update(hash_sequence())

if sh not in (NONE, SINGLE):
    h.update(hash_outputs())

h.update(bytes([st]))                # script type, where BIP341 has spend_type

if anyonecanpay:
    h.update(reversed(txid_i))
    h.update(vout_i.to_le32())
    h.update(values[i].to_le64())
    h.update(script_pubkeys[i].serialize())
    h.update(sequence_i.to_le32())
else:
    h.update(i.to_le32())

if st in (BARE, WITNESS_V0):
    h.update(script_code.serialize())
else:
    h.update(0x01 if annex else 0x00)
    if annex:
        h.update(sha256(compact_size(len(annex)) + annex))

if sh == SINGLE:
    if i >= len(outputs): error
    h.update(sha256(outputs[i].serialize()))

if st == TAPSCRIPT:
    h.update(tapleaf_hash)
    h.update(0x00)                   # key version
    h.update(0xffffffff if no codeseparator else codeseparator.to_le32())

return h.digest()
```

Notes from the reference implementation:

* `hash_prevouts`, `hash_sequence`, `hash_outputs` are the BIP341
  `sha256d`-style helpers; `hash_amounts`/`hash_script_pubkeys` hash the
  concatenation with `sha256d`.
* `script_code` is required for BARE/WITNESS_V0: scriptPubKey for bare,
  redeemScript for P2SH, witnessScript for P2WSH, implied P2PKH for P2WPKH.
* `tapleaf_hash` is required for TAPSCRIPT.
* The digest size is 32 bytes; signing uses the normal ECDSA/Schnorr path
  for the script type.
* The memoised amounts/script_pubkeys caches must be keyed by **the list
  they were computed over**, not just "computed once".  A caller reusing a
  transaction with different spent outputs must not get the first call's
  digest.

### 1.3 Selection and UI policy (Krux `sighash.py`)

Krux allows only:

```
None, DEFAULT (taproot), ALL, UNIFIED|ALL
```

and refuses a PSBT when:

* one of our inputs would be skipped (`REFUSED_PARTIAL`), or
* our inputs would be signed with no single honest label (`REFUSED_MIXED`).

A label is always shown: `"Unified 0x21"` or `"Standard 0x01"`.  A host
that downgrades a request to standard must be visible as standard, not as
"nothing requested".  Replicate this in kern's PSBT review; do not sign an
unlabelled type.

The signed signatures are read back from the PSBT and compared before and
after signing; a pre-populated/planted signature slot is rejected.  Keep
that behaviour.

### 1.4 Porting to libwally-core

Add to the libwally fork (not to kern):

```c
int wally_tx_get_unified_signature_hash(
    const struct wally_tx *tx,
    size_t input_index,
    uint32_t script_type,             /* 0..3 */
    const uint64_t *values,           /* one per input */
    size_t values_len,
    const struct wally_tx_output **script_pubkeys, /* or script bytes */
    size_t script_pubkeys_len,
    const unsigned char *script_code, size_t script_code_len,
    const unsigned char *tapleaf_hash, size_t tapleaf_hash_len,
    const unsigned char *annex, size_t annex_len,
    uint32_t codeseparator_pos,
    uint32_t sighash_type,
    unsigned char *bytes_out);
```

and a PSBT wrapper that:

* selects the unified path iff the input's `sighash_type` has `0x20`;
* derives the script type from the **script** (scriptPubKey, redeemScript,
  witnessScript, tapleaf), never from which PSBT fields are populated;
* validates per input, fails one input at a time with a status code;
* keeps the caller's values out of any shared sibling cache and keeps
  memory overhead constant;
* exposes the opt-in flag to kern's signer.

Implementation can use `wally_sha256`, `wally_sha256d` and the existing
tagged-hash helper; there is no new cryptographic primitive.

### 1.5 Tests

Before any hardware:

1. Generate/port a C vector runner using
   `unified_sighash.json` (every vector: script type, amounts,
   scriptPubKeys, annex, codeseparator, hash type, expected digest).
2. Port the negotiation tests: mixed types, partial signing, downgrade,
   `SIGHASH_SINGLE` without an output, invalid bits, out-of-range type.
3. Compare against `embit` byte-for-byte for all vectors.
4. Add a libwally PSBT test that signs a PSBT with `0x21` and verifies the
   signature under the unified digest.
5. Run the existing libwally PSBT tests to prove standard paths unchanged.

### 1.6 XBT / BLAKE2b block rules

XBT is a separate chain/block format, not a replacement for Bitcoin.  The
fork changes the **block header / proof of work** to BLAKE2b and activates
the unified sighash at the same buried deployment (`Blake2bHeight`).  The
signature hash remains the tagged-SHA256 message above; the signature
verification path is the same ECDSA/Schnorr path for the script type.

Implement the chain selection as a build/runtime rule, not a UI toggle:

```c
typedef enum { KERN_CHAIN_BITCOIN, KERN_CHAIN_XBT } kern_chain_id;
```

The XBT side needs:

* BLAKE2b block-header hashing and the v2 block header (height + tx count)
  that the deployment requires;
* the activation height / deployment name (`blake2b`);
* XBT network/address parameters and how a PSBT is marked for XBT;
* the Knots functional tests (`knots_interop.py`, `cve_2020_14199.py`,
  `seedsigner_knots_e2e.py`) and the 166 vectors.

The current C vector test in `tools/sighash/` covers the SHA256/Knots
signature-hash side and passes all 166 vectors (`make -C tools/sighash`).
Add an XBT block-header test separately; do not change the signature-hash
vectors to fit it.

## 2. SeedKeeper / Satochip / TapSigner

Reference: `krux/src/krux/seedkeeper.py` (841 lines, plaintext flows) and
`seedkeeper_sc.py` (240 lines, secure channel).  Transport is any
`transmit(apdu) -> (data, sw1, sw2)`; in kern it is the NFC component.

### 2.1 Card basics

* CLA `0xB0`; AID ASCII `"SeedKeeper"` = `53 65 65 64 4b 65 65 70 65 72`.
* SELECT APDU `00 A4 04 00 <len> <AID>`; `6A82` means the applet is not
  provisioned.
* PIN: 4..16 bytes; `INS_VERIFY_PIN 0x42`; wrong PIN returns `63Cx` with
  the remaining tries in the low nibble.
* Status words: `9000` success; `9C01` full, `9C02` wrong PIN, `9C04`
  setup missing, `9C06` PIN required, `9C08` secret not found, `9C0C`
  identity blocked, `9C20` secure channel required, `9C31` export not
  allowed, and the rest in `STATUS_WORDS`.

Commands used by the plaintext flows:

| INS | name |
|---|---|
| `0x2A` | SETUP |
| `0x42` | VERIFY_PIN |
| `0x3C` | GET_STATUS |
| `0xA0` | GENERATE_MASTERSEED |
| `0xA1` | IMPORT_SECRET |
| `0xA2` | EXPORT_SECRET |
| `0xA3` | GENERATE_RANDOM_SECRET |
| `0xA5` | RESET_SECRET |
| `0xA6` | LIST_HEADERS |
| `0xA7` | SEEDKEEPER_STATUS |
| `0x81` | INIT_SECURE_CHANNEL |
| `0x82` | PROCESS_SECURE_CHANNEL |

Secret types: `0x10` masterseed, `0x30` BIP39 mnemonic, `0x90` password,
`0xC0` data, `0xC1` descriptor.  Subtype `0x01` on a masterseed means it
carries the BIP39 mnemonic, wordlist and passphrase.  Header: 15-byte
prefix (id, type, origin, export rights, export counters, fingerprint,
subtype, reserved, label length) + UTF-8 label (≤64 bytes).  Import is in
`CHUNK_SIZE=128` chunks.

Only **plaintext export** (`P1=0x01`, `EXPORT_PLAINTEXT_ALLOWED`) is in the
portable implementation; this is SeedSigner-compatible.  Secure-channel
secrets (`P1=0x02`) need the card ECDSA key and per-secret AES key and are
not portable without the handshake.

### 2.2 Secure channel port

If kern must read secure-channel secrets:

* ephemeral secp256k1 key pair;
* ECDH shared X;
* HMAC-SHA1 key derivation: `sc_key` → 16-byte AES key, `sc_mac` → MAC key;
* AES-128-CBC, PKCS#7;
* IV = 12 random bytes + odd 32-bit counter (card uses even counters);
* card returns ephemeral X + self-signature over `00 20 || X`; recover Y
  and verify;
* envelope `PROCESS_SECURE_CHANNEL`.

libwally-core has secp256k1 ECDH/recover; mbedTLS has AES-CBC/HMAC-SHA1.
Keep the same byte layout as `seedkeeper_sc.py`.

### 2.3 Test plan

* A physical SeedKeeper/Satochip card; no card is needed for transport
  bring-up (the chip answers without a card).
* Status/PIN tests: wrong PIN, blocked card, no applet.
* Plaintext import/export round-trip with a known mnemonic; compare with
  SeedSigner.
* Descriptor secret (`0xC1`) and masterseed + BIP39 subtype `0x01`.
* Chunk boundaries: 0, 1, 127, 128, 129, multiple chunks.
* Remove/reinsert card mid-operation; verify reconnect/resume behaviour
  (only `GET_STATUS` is safe to repeat; stateful walks restart).
* Secure channel, if enabled, against a card provisioned for it.

## 3. Krux ↔ kern parity and port scope

kern already implements most of Krux's Bitcoin feature set.  The port
should not reimplement what libwally/kern already does; it should wire the
Nexus 5 hardware and the unified sighash.

| area | Krux-nexus | kern | kern-nexus action |
|---|---|---|---|
| BIP39/SeedQR/KEF/BIP85 | yes | yes | keep |
| BIP49/84/86, multisig | yes | yes | keep |
| Miniscript `wsh`/`tr` | yes | yes | keep |
| PSBT signing | yes | yes | wire unified opt-in |
| BIP137 / BIP322 | yes | yes | keep |
| UR/BBQr/Specter pMofN | yes | yes | keep (cUR, bbqr, k_quirc) |
| NFC SeedKeeper | yes | no | port |
| SD/file/printer | yes/no | SD | hide/disable |
| Camera QR/entropy | yes | yes (ESP camera) | port camera thread |
| Display/touch/keys | fbdev/evdev | ESP BSP | port |
| Persistent settings/PIN | flash | NVS/eFuse | `/data/kern` as Krux |
| Firmware update | no | ESP OTA | disable |

Feature decisions to keep explicit:

* SeedXOR/Binary Grids are not in kern; do not add them silently.
* kern is ahead on Taproot Miniscript and BIP-138 encrypted descriptor
  backups; keep those.
* A single static binary, no Python, no dynamic libraries.

## 4. Security invariants

* `explicit_bzero()` every mnemonic, passphrase, PIN, PSBT, APDU and
  decrypted seed buffer; also zero framebuffer/scanout and camera/NFC
  buffers before power-off.
* Never write seeds or plaintext secrets to `/data` or eMMC.  Settings and
  the PIN verifier live under `/data/kern`; secret buffers are never
  persisted.  The Nexus 5 has no eFuse HMAC: replace the simulator's fixed
  HMAC key with a per-device random salt stored under `/data/kern`.
* The only external I/O is NFC, camera, display/touch/keys and optional
  adb/UART.  No network stack, no USB network, no SD menu in the delivered
  image.
