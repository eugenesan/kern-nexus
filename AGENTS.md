# Agent notes

Read `docs/kern-nexus-blueprint.md` first, then the detail document for the
area you touch.

Hard rules:

* Target is the LG Nexus 5: ARMv7-A, hard-float, Linux 3.4, static binary.
* Never flash `boot`.  Dev images are `fastboot boot`; the delivered image
  is written to `recovery`.
* No network stack, Wi-Fi, Bluetooth or cellular code.  NFC is the only
  wireless interface.
* No Python or MicroPython on the target.  Camera/NFC are C
  components/threads.
* Settings/PIN live in `/data/kern` as in Krux; never persist seeds or
  plaintext secrets.  `explicit_bzero()` secret buffers.
* Standard Bitcoin uses SHA256; XBT uses BLAKE2b block/PoW with the same
  tagged-SHA256 unified sighash.  Validate the 166 vectors first.
* The standard sealed image hides `adbd`; use `start-adb`/`start-serial`.
* Copy only necessary components from `../krux-nexus`; record the source
  commit and deviations in `components/<name>/README.md`.

Build:

```sh
bash scripts/build-kernel.sh
bash scripts/build-kern.sh
bash scripts/build-image.sh
```

The current build dependencies and open decisions are in
`docs/build-and-deploy.md` and `docs/bringup-plan.md`.
