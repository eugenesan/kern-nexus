# Safe points and rollback (2026-11-17)

This records the known-good state **before** the in-process camd
integration.  Nothing in this document is flashed; all device images are
`fastboot boot`ed from `/tmp`.

## 1. Kernel — in-tree branch (no build-time patches)

| item | value |
|---|---|
| `linux/` submodule branch | `airgap/cam-xz-wipe-kern` |
| submodule commit | `3e3f2ace562` ("msm_cpp: guard frame timeout work against a missing frame") |
| parent | `airgap/cam-xz-wipe` (`cc17c59739b`, the camera/wipe branch) |
| outer commit pinning it | `1292199` |

The branch already contains the full `tools/kernel/patches/kern-nexus-kernel.patch`
series (inherited from `airgap/cam-xz-wipe`) **and** the CPP timeout NULL
guard from `tools/kernel/patches/cpp-timeout-null-guard.patch`, committed
in-tree.  The patch files are retained for reference/bootstrapping only;
`scripts/build-kernel.sh` applies any patch that is not yet present and
skips the rest, so building from this branch is idempotent.

To return to this kernel state:

```sh
cd /home/user/Develop/src/kern-nexus/linux
git checkout airgap/cam-xz-wipe-kern
git reset --hard 3e3f2ace562
cd ..
# the outer tree at 1292199 already pins this exact commit
```

## 2. Outer tree snapshot before integration

| item | value |
|---|---|
| branch | `stable-pre-camd-integration` |
| tag | `pre-camd-integration-2026-11-17` |
| commit | `1292199` |
| parent commits | `3fcbd64` (Stage 0 seam), `b2e2795` (CPP patch), `a3bee97` (camd idle CPU) |

Everything up to and including the verified teardown/restart work is in
this snapshot.  To roll back the whole outer tree:

```sh
cd /home/user/Develop/src/kern-nexus
git checkout stable-pre-camd-integration      # or: git checkout pre-camd-integration-2026-11-17
git submodule update --init linux            # pins 3e3f2ace562
bash scripts/test.sh
bash scripts/build-all.sh
```

## 3. What was verified at the safe point

* Host suites: `scripts/test.sh` passes (sighash, SeedKeeper, cUR, QR, 12/12 k_quirc).
* Kernel: CPP guard patch applied, no `[REI]` instrumentation in the
  pinned branch (the instrumented kernel lives only on the separate
  worktree `camera-re-instr` at `/tmp/opencode/linux-instr`).
* Teardown/restart (experimental branch, not the shipped image):
  5/5 cycles served fresh frames; AF sweep moved focus; 10-min idle with
  camd stopped gave 40→32 C and ~3.6 mA, then a fresh frame on restart;
  no watchdog, write-master, ping-pong or panic events.
* The device was restored to the project audit image with the committed
  camd `md5 8900181b7c6198d146829c5daa380ca5` and is healthy.

## 4. Experimental branches (not safe points)

* `camera-stream-exp` — camd teardown/restart + bridge respawn experiments.
* `camera-re-instr` (in the `linux/` submodule) — kernel with `[REI]` printks
  used for the RE; do not build firmware from it.

## 5. Rules that still apply

* Never flash `boot`; development is `fastboot boot` only.
* Delivered image goes to `recovery` (`mmcblk0p20`).
* Build products stay under `/tmp/kern-nexus` or
  `/tmp/opencode/camera-stream-exp-build`, never in the checkout.
* Any in-process camd work is behind `KERN_CAMERA_IN_PROCESS` (default OFF)
  until it passes the full device verification.
