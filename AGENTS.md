# Agent notes

Read `docs/status.md` first (what works, what is next), then
`docs/agent-bootstrap.md` (the session prompt), then
`docs/kern-nexus-blueprint.md` and the detail document for the area you
touch.

Hard rules:

* Target is the LG Nexus 5: ARMv7-A, hard-float, Linux 3.4, static binary.
* Never flash `boot`.  Dev images are `fastboot boot`; the delivered image
  is written to `recovery`.
* No network stack, Wi-Fi, Bluetooth or cellular code.  NFC is the only
  wireless interface.
* No Python or MicroPython on the target.  Camera/NFC are C
  components/threads.
* Settings/PIN live in `/data/kern` as in Krux; never persist seeds or
  plaintext secrets.  Scrub secret buffers (§7 of `status.md` for what is
  still missing).
* Standard Bitcoin uses SHA256; XBT uses BLAKE2b block/PoW with the same
  tagged-SHA256 unified sighash.  Validate the 166 vectors first, and do
  not invent XBT constants.
* The standard sealed image hides `adbd`; use `start-adb`/`start-serial`.
* Copy only necessary components from `../krux-nexus`; record the source
  commit and deviations in `components/<name>/README.md`.
* Build products go under `/tmp/kern-nexus` (`KERN_NEXUS_BUILD`), never
  into the checkout.  `kern/components/cUR` takes `BUILD=` for the same
  reason.

Build and test:

```sh
bash scripts/build-all.sh            # kernel, mbedTLS, zlib, kern, camd, images
bash scripts/test.sh                 # host suites (sighash, SeedKeeper, cUR, QR, core)
bash scripts/build-cur-tests.sh      # cUR alone (built from a copy in /tmp)
```

Device loop, camera/NFC control sockets, and known UI-verification
pitfalls are in `docs/agent-bootstrap.md` and `docs/status.md`.
