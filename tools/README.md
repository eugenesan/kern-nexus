# tools/

| path | purpose |
|---|---|
| `kernel/build-kernel.sh` | the mature air-gap kernel builder (copied from krux-nexus) |
| `kernel/apply-defconfig.py` | strips/enables Kconfig symbols and verifies them |
| `kernel/profiles/` | hammerhead strip/keep/camera profiles |
| `check-airgap.py` | fails if the kernel config or ramdisk has wireless comms other than NFC |
| `serial-cmd.py` | host helper for the UART shell (`start-serial`) |

`kernel/build-kernel.sh` is driven by `scripts/build-kernel.sh`; use the
wrapper.  `KEEPLISTS=` overrides the keep-lists (space-separated paths) —
do not edit a tracked profile for a one-off diagnostic build.

`check-airgap.py` is called by `scripts/build-image.sh` for the standard
image:

```sh
python3 tools/check-airgap.py \
    --config /tmp/kern-nexus/artifacts/build/ko/.config \
    --ramdisk /tmp/kern-nexus/artifacts/build/ramdisk-kern-bundled
```

`serial-cmd.py`:

```sh
python3 tools/serial-cmd.py "id" "ps -A -o PID,NAME"
```
