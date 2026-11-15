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

### 1.4 Where the implementation lives

The digest is implemented in kern, not in libwally:

* `kern/main/core/unified_sighash.c` — transaction parser and digest;
* `kern/main/core/unified_sha256.c` — SHA256/tagged-SHA256;
* `kern/main/core/psbt.c` — per-input signing glue.

libwally stays unmodified.  Its `wally_psbt_sign_bip32()` dispatch, key
matching and taproot branches assume BIP143/BIP341; adding a fourth type
there would mean maintaining a fork of a pinned submodule.  libwally is
still used for EC signing, DER encoding, BIP341 tweaks and the PSBT maps.
The digest is computed once per input from the transaction and all spent
outputs, then:

* ECDSA (legacy, P2WPKH, P2WSH, P2SH): sign, DER-encode, append `0x21`,
  add a partial signature keyed by the compressed pubkey;
* taproot key path: tweak the private key with the PSBT's merkle root,
  Schnorr-sign, append `0x21`;
* taproot script path: sign each leaf whose hash is listed for our xonly
  key, keyed by `xonly || leaf_hash`.

`psbt_sighash_is_supported()` accepts `0x21` (`UNIFIED|ALL`); the review
screen names it `UNIFIED|ALL`.  Mixed or partial requests are still
refused, and every input must carry an amount because the digest commits
to all of them.

### 1.5 Tests

* `make -C tools/sighash` compiles the firmware files and passes all 166
  vectors.
* `kern/main/core/test/test_psbt_classify.c` signs a P2WPKH and a BIP86
  PSBT with `0x21` and verifies each signature under the expected unified
  digest; the suite is at **146 passed**.

### 1.6 XBT / BLAKE2b block rules

XBT changes the **block header / proof of work** to BLAKE2b.  It does not
change the transaction or PSBT format, and it does not change the unified
signature hash, which remains tagged SHA256.

A signer does not validate blocks.  Kern's signing path therefore only
needs to recognise the opt-in sighash byte; chain selection, block headers
and PoW are coordinator/node concerns.  `components/xbt` contains a
standalone BLAKE2b/v2-header implementation with the five official Knots
vectors, but nothing links it into kern and it is not a prerequisite for
signing.

The missing XBT facts are the activation height and the network/address
parameters.  Until those are supplied, do not add a chain selector or
invent constants.  A PSBT that carries `0x21` is signed under the unified
digest and labelled `UNIFIED|ALL`; a PSBT that asks for any other
non-`ALL` type is refused, exactly as Krux does in `sighash.py`.

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

### 2.2 What kern does with the card

`kern/main/pages/seedkeeper/` is the card's user interface.  All of it
goes through `seedkeeper_page.c`, which asks for the PIN before any card
command is sent, runs the blocking APDUs on the worker task and keeps the
session open while the user reads a menu.  Errors are drawn inside the
page, never in a modal: a modal outlives the page and then calls back
into deleted LVGL objects.

| Where | Flow | What it does |
|---|---|---|
| Load Mnemonic → From SeedKeeper | load | exports a BIP39 Masterseed, checks it against the seed the card stored, then asks for a passphrase only when the secret did not carry one |
| New Mnemonic → From SeedKeeper | new seed | the card generates 32 random bytes (`INS 0xA0`), the device turns them into a 24-word mnemonic and the card keeps the BIP39 Masterseed of it |
| Back Up → To SeedKeeper | backup | stores the loaded wallet on the card as a BIP39 Masterseed, under a label the user gives |
| Home → SeedKeeper | list | every secret on the card: id, label and type |
| Home → SeedKeeper | delete | picks a secret and frees it (`INS 0xA5`) after a confirmation |
| Home → SeedKeeper | card info | applet and protocol version, secure channel, setup state, PIN tries left |
| Home → SeedKeeper | free space | free, used and total card memory |

Two rules the flows keep:

* A seed **generated on the card carries no passphrase**.  The card has no
  passphrase of its own, so the BIP39 Masterseed it stores is derived from
  the entropy alone and the device loads exactly that.  A wallet that
  needs a passphrase is created elsewhere and copied to the card with
  "Back Up → To SeedKeeper".
* The backup flow **checks the passphrase against the loaded wallet** by
  comparing the master key fingerprint of the derived seed with the loaded
  key's.  A wrong passphrase fails instead of quietly writing a different
  wallet to the card.

The applet's own rules that the port follows: a generated secret is kept
as a plain `[size | bytes]` blob, an imported secret is padded by the card
to a multiple of 16 bytes, the fingerprint is the first four bytes of the
SHA-256 of the secret, and the export strips the padding and appends the
card's ECDSA signature to the last chunk.

### 2.3 Secure channel port

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

### 2.4 Test plan

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
* Generation round-trip: `INS 0xA0`, export the plain secret, check the
  entropy, and check that the BIP39 Masterseed the card keeps reloads to
  the same mnemonic.
* Backup round-trip: store the loaded wallet, then load it again from the
  card and compare the master key fingerprint.
* Delete: free a secret and check that the card's secret count and free
  memory follow.

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
