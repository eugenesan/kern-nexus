# components/seedkeeper

The SeedKeeper/Satochip applet layer.  **Not implemented yet**; the
reference is `krux-nexus/krux/src/krux/seedkeeper.py` (plaintext flows) and
`seedkeeper_sc.py` (secure channel).

Wire facts to preserve:

* CLA `0xB0`; AID ASCII `"SeedKeeper"`;
* SELECT `00 A4 04 00 0A 53 65 65 64 4B 65 65 70 65 72`;
* INS: `2A` SETUP, `42` VERIFY_PIN, `3C` GET_STATUS, `A0` GENERATE_MASTERSEED,
  `A1` IMPORT_SECRET, `A2` EXPORT_SECRET, `A3` GENERATE_RANDOM_SECRET,
  `A5` RESET_SECRET, `A6` LIST_HEADERS, `A7` SEEDKEEPER_STATUS;
* secure channel: `81` INIT, `82` PROCESS;
* PIN 4..16; `63Cx` = wrong PIN with tries left; `6A82` = no applet;
* secret types `0x10/0x30/0x90/0xC0/0xC1`; subtype `0x01` = masterseed
  carrying BIP39 mnemonic/wordlist/passphrase;
* 15-byte header + label; chunks of 128 bytes;
* plaintext export `P1=0x01` is SeedSigner-compatible; `P1=0x02` needs the
  secure channel.

The secure channel needs secp256k1 ECDH, HMAC-SHA1, AES-128-CBC and the
card's self-signature/ECDSA recovery.  libwally-core + mbedTLS provide the
primitives; do not pull in a new crypto library.

Tests: see [../../docs/bitcoin.md](../../docs/bitcoin.md#2-seedkeeper--satochip--tapsigner).
