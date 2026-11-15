# components/nfc

Low-level BCM2079x transport (`nfc_bcm2079x.c`) and the C API in `nfc.h`.
The chip protocol is **not yet ported**; the reference is
`krux-nexus/krux/src/nfc.py` and `krux-nexus/docs/nfc-transport.md`.

What is here now:

* open/power/wake/read/write/close on `/dev/bcm2079x`;
* the ioctl numbers (0xFA01–0xFA07);
* one whole framed packet per `read()`;
* the `kern_nfc_transport_t` contract for the card layer.

What must be added (next task):

1. NCI framing and the bring-up sequence from `nfc.py`:
   `CORE_RESET`, `CORE_INIT`, the exact `SET_CONFIG` payloads
   (`LPTD`, start-up/antenna, poll), `DISCOVER`, tag notification,
   `RF_DISCOVER_SELECT`.
2. ISO-DEP APDU data exchange and response reassembly.
3. Timeouts: poll window 8 s, activation settle 200 ms, APDU 5 s.
4. `kern_nfc_bring_up()` and `kern_nfc_transceive()`.
5. SeedKeeper/Satochip APDU logic in `components/seedkeeper`.

The NFC chip firmware is already in the chip; no download is needed.  The
`.ncd` blobs in `krux-nexus/nfc/blobs` are for recovery/reference only.

Do not open `/dev/i2c-0` directly: the kernel driver owns address 0x77.
Do not power-cycle the chip while another client has it open.

See [../../docs/hardware.md](../../docs/hardware.md#nfc-bcm2079x) for the
measured framing and bring-up trace.

## Implemented C layer

`nfc_nci.c` now contains the NCI/ISO-DEP implementation ported from
`krux/src/nfc.py`:

- `kern_nfc_bring_up()` — the vendor bring-up sequence, exact SET_CONFIG
  payloads, discover/select;
- `kern_nfc_transceive()` — one APDU, data + SW1/SW2;
- 8 s poll window, 1 s rediscover, 200 ms activation settle, 5 s APDU
  timeout;
- `nfc_nci_internal.h` exposes the framing helpers for the host test.

```sh
cd components/nfc/test
make test
# nfc_nci: 78 checks passed
```

The hardware path is still untested; the next device check is no-card →
tag → SeedKeeper.
