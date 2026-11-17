# Preface prompt: first session — camd stop/restart stream experiment

Paste the block below as the **first message** of a new session in
`/home/user/Develop/src/kern-nexus`.  It assumes the agent has the same
tools as this session (shell, file tools, git, adb/fastboot, serial) and
that the device is available.  The plain text after it is context for the
human, not part of the prompt.

---

## Prompt

You are joining the `kern-nexus` project: a C Bitcoin signer (`kern`)
running as one static ARM binary on an LG Nexus 5 (`hammerhead`) under a
TWRP recovery ramdisk.  The project is **operational**: QR scanning,
camera entropy, SeedKeeper NFC and PSBT signing work on hardware.  Your
first assignment is a **side experiment** on the camera daemon, not a
change to the shipping firmware.

### 0. Read before touching anything

In this order:

1. `docs/agent-bootstrap.md` — rules, layout, device loop.
2. `docs/status.md` — what works and what is still open; especially §2, §3, §5, §7.
3. `docs/kern-nexus-blueprint.md` and `docs/hardware.md` §5 (camera).
4. `components/camera/re/README.md` — the **git-ignored local camera RE archive**.
   Then `STREAM-RESTART.md`, `FINDINGS.md`, `HISTORY.md`, `MEASUREMENTS.md`
   and `exports/MANIFEST.md` in that directory.
5. The exported Krux reference in `components/camera/re/exports/krux-reference/docs/`,
   especially `cam-state.md` and `cam-good-state.md`.

The archive documents four previous attempts to stop/restart the frame
stream and why each failed.  Do not repeat them without a new idea, and
do not start coding before you can state which constraint you are
satisfying and how you will measure success.

### 1. Project rules that still apply

* Never flash `boot`.  Development uses `fastboot boot`; the delivered
  image goes to `recovery` (`mmcblk0p20`).  Do not change the shipped
  images or the current `/opt/kern` binaries.
* No network stack, Wi-Fi, Bluetooth or cellular code.  NFC is the only
  wireless interface.  No Python/MicroPython on the target.
* Standard Bitcoin keeps SHA256; XBT is signer-scope only and its
  activation constants must not be invented.
* Build products go under `/tmp/kern-nexus` (`KERN_NEXUS_BUILD`), never
  into the checkout.  Run `scripts/test.sh` before and after changes.

### 2. Device access you have

You have **full access to the Nexus 5** (device id `0643efdcf0eca63a`):

* **adb** over USB when the current recovery/dev image is running;
  `adb reboot bootloader` → `fastboot devices` → `fastboot boot <image>`
  boots any kernel/ramdisk temporarily.
* **UART**: device side `ttyHSL0` at 115200 8N1 on the headset jack, host
  side FT232 on `/dev/ttyUSB0` (`stty -F /dev/ttyUSB0 115200 cs8 -cstopb
  -parenb raw`, or `scripts/serial-cmd.py`).  Kernel and `start-serial`
  share the line; use it to catch watchdog/teardown traces that adb loses.
* The device can also boot the **referenced Android** with root via
  **KernelSU** (`su` from a shell).  KernelSU patches live in branches of
  the included kernel tree `linux/` — see
  `airgap/cam-ksu`, `airgap/cam-ksu-dump`, `airgap/cam-debug` and the
  other `airgap/*` branches (`git -C linux branch -a`).  Build one of
  those with `scripts/build-kernel.sh` / `KERNEL_CFG=…` and `fastboot boot`
  it for diagnostics.  With `su` you may inspect sysfs, dmesg, IOMMU and
  camera nodes, run probes, and capture traces from Android.
* You may run **any** command, capture or program on the device, boot any
  kernel, and start/stop services, subject only to the safety rule below.

**ABSOLUTE DEVICE SAFETY RULE — never corrupt pre-existing data on any
device partition.** Concretely:

* No `fastboot flash`, `fastboot erase`, `fastboot format`, `dd` to block
  devices, `mkfs`, `wipe`, or writes to `/system`, `/data`, `/boot`,
  `/recovery`, `/cache`, `/firmware`, `/modem`, `/persist`, etc.
* Only `fastboot boot` is permitted for new kernels/images (RAM-only).
* Write new files only to fresh, uniquely named paths under
  `/data/local/tmp`, `/data/kern` (new names) or tmpfs.  Never overwrite
  an existing file without first making a verified backup; never use
  `mv`/`cp` onto a source path (the CPP firmware self-copy with `O_TRUNC`
  once blanked it and produced `CPP FW 0xdeadbeef`).
* Leave the operational `/data/kern` settings, PIN and `/opt/kern`
  binaries intact, and leave the current standard/audit images untouched.
  The device must still boot into the working firmware afterwards.
* If an operation might destroy or modify existing partition data, stop
  and ask the operator instead of doing it.

### 3. Experiment isolation (the shipping tree stays operational)

* Create a **separate git worktree** for this work (for example
  `/tmp/opencode/camera-stream-exp`, via `git worktree add`), or at least
  a dedicated branch.  Do not commit to the operational branch, do not
  touch the deployed binaries/images, and use a separate
  `KERN_NEXUS_BUILD` path (e.g. `/tmp/opencode/camera-stream-exp-build`)
  so `scripts/build-*` cannot overwrite the working artifacts.
* You may read and append findings in the git-ignored
  `components/camera/re/` archive (it is local scratch; add a
  `EXPERIMENT-*.md` there with dates, commands and results).  Do not make
  the archive part of the build.
* Any feature you add to camd must be **default-off / feature-gated**:
  the current continuous-stream behaviour must remain the default and the
  operational build must be unaffected.  A persistent knob (like the
  existing `idle_vts`/`idle_after` state values) or a build flag is fine.

### 4. First task

Improve camd so the frame stream can be stopped and restarted (reduced
idle heat and battery) **without** breaking the resume: no watchdog,
no stitched frames, no AE pumping, correct first frame.

Work from `components/camera/re/STREAM-RESTART.md` §4.  Start with the
lowest-risk direction (A: make idle cheap without stopping the pipeline;
then B: very low idle frame rate with ISP geometry changed together).
Do **not** attempt V4L2 STREAMOFF/STREAMON, daemon exit/restart, or a long
`0x0100` sensor pause unless you first find a concrete reason the kernel
constraints from `FINDINGS.md` no longer apply.

For every experiment:

1. Write a short plan first (hypothesis, exact change, measurements,
   rollback point) into the `re/` archive.
2. Record the known-good hashes/md5 and keep the last working camd binary
   on the device (a new `/data/local/tmp` name) for instant rollback.
3. Use the stop conditions in `STREAM-RESTART.md` §5 — `No free write
   masters`, `Invalid close`, ping-pong mismatch, tasklet overflow,
   `wdog_bark`, drifting overlap — and abandon an approach immediately
   if any appear.
4. Verify a resume with a QR decode of a known code, entropy bits/pixel
   and deviation, an AF sweep at a fixed distance, then a 10-minute idle
   soak and 100 page cycles.  Compare thermal and CPU against the
   documented baseline.
5. Finish with a written conclusion in `re/EXPERIMENT-*.md`: what worked,
   what failed, the evidence, and a recommendation (adopt as an opt-in,
   keep experimenting, or abandon).  Do not merge into the operational
   tree.

### 5. Practical notes

* Camera control socket: `/data/kern/camd-ctl.sock` (0600), commands
  `E` exposure+gain, `G` luma, `V` VMAX, `F` focus step, `A` auto focus,
  `R`/`r` refocus, `q` snapshot.  Frame socket:
  `/data/kern/camd.sock` with `F`, `A`, `R`/`r`, `D` — **not `Q`**: this
  tree has no supervisor to restart a daemon that exits.
* Framebuffer capture: `bash /tmp/opencode/fbcap.sh <name>.png` (exported
  copy in `components/camera/re/exports/code/`) or see `MEASUREMENTS.md`.
* Touch injection over adb is unreliable; verify UI flows with the
  operator, and use the control sockets for deterministic measurements.
* The device is battery-sensitive.  Recharge with USB if it drops off;
  do not leave the ISP streaming unattended for long periods.
* If the device comes up in fastboot, `fastboot boot` the audit image
  (`boot-kern-standard-release-audit.img`), wait ~20 s, then
  `adb shell /sbin/start-kern` (standard image boots TWRP by default).

Start by summarizing your understanding of the constraints and stating
which direction you will try first, then proceed.

---

## Notes for the human (not part of the prompt)

This file is a hand-off prompt, not maintained firmware documentation.
It intentionally repeats the safety rules because a fresh agent will not
have the earlier conversation.  Add it to a new session together with the
git-ignored `components/camera/re/` archive; the agent can append its
experiment log there without touching the operational project.
