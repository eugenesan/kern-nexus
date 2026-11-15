# kern-nexus documentation

Start with the blueprint; it is the overview and the coding-agent brief.

| document | read it for |
|---|---|
| [kern-nexus-blueprint.md](kern-nexus-blueprint.md) | **start here**: target, architecture, boot modes, port layout, ordered work |
| [build-and-deploy.md](build-and-deploy.md) | toolchain, kernel/Kern builds, LVGL/mbedTLS, ramdisk/image, TWRP modes, flash/deploy |
| [hardware.md](hardware.md) | measured nodes and wiring: display/input/power, camera, NFC, UART, air gap |
| [bitcoin.md](bitcoin.md) | unified opt-in sighash (tagged SHA256; XBT BLAKE2b block/PoW), libwally port, SeedKeeper, parity |
| [kernel-and-wipe.md](kernel-and-wipe.md) | kernel patch set, power-off RAM wipe, `k_run`/`k_next` |
| [bringup-plan.md](bringup-plan.md) | phases, exit criteria, test matrix, open decisions |

Reference material lives in the sibling `../krux-nexus` tree:

* `../krux-nexus/README.md` — the working Linux/TWRP/Krux port;
* `../krux-nexus/docs/build-guide.md` — the build detail this project
  inherits;
* `../krux-nexus/docs/memory-wipe-state.md` — the verified wipe trace;
* `../krux-nexus/docs/cam-state.md` — the camera path and its pitfalls;
* `../krux-nexus/docs/nfc-transport.md` — the BCM2079x contract;
* `../krux-nexus/krux/src/nfc.py`, `seedkeeper.py`, `seedkeeper_sc.py` —
  the protocol references;
* `../krux-nexus/krux/vendor/embit` — the unified-sighash reference and
  vectors.

## Verification assets

* `tools/sighash/` — standalone unified-sighash implementation and the
  166 embit vectors (`make -C tools/sighash`).
* `tools/bringup/` — static placeholder and the boot-chain image recipe.
* `tools/check-airgap.py` — kernel/ramdisk wireless gate.
* `tools/serial-cmd.py` — host UART helper.
