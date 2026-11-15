# components/seedkeeper

The SeedKeeper/Satochip applet layer on top of the NFC transport.  This is
the plaintext protocol (P1=0x01) that SeedSigner writes and reads; the
secure channel (P1=0x02, INS 0x81/0x82) is not implemented, so secrets that
require it are refused by the card.

Ported from `krux-nexus/krux/src/krux/seedkeeper.py`:

* CLA `0xB0`; AID ASCII `"SeedKeeper"`;
* SELECT `00 A4 04 00 0A 53 65 65 64 4B 65 65 70 65 72`;
* INS: `2A` SETUP, `42` VERIFY_PIN, `3C` GET_STATUS, `A0` GENERATE_MASTERSEED,
  `A1` IMPORT_SECRET, `A2` EXPORT_SECRET, `A3` GENERATE_RANDOM_SECRET,
  `A5` RESET_SECRET, `A6` LIST_HEADERS, `A7` SEEDKEEPER_STATUS;
* 15-byte header + label; 128-byte import chunks; `9C12` ends a header walk;
* PIN 4..16; `63Cx` = wrong PIN with tries left; `6A82` = no applet;
* secret types `0x10/0x30/0x90/0xC0/0xC1`; subtype `0x01` = masterseed
  carrying BIP39 entropy, wordlist and passphrase;
* plaintext export `P1=0x01` is SeedSigner-compatible.

`seedkeeper.c` also packs/unpacks the BIP39 Masterseed payload and describes
status words.  Fingerprints use SHA-256 from the pinned mbedTLS.

```sh
cd components/seedkeeper/test
make test
# seedkeeper: checks passed
```

The mock transport tests cover select/PIN/status/list/import/export/reset,
the BIP39 payload, malformed responses and lost-command retry.  No card test
has run yet: the Nexus currently has no tag.
