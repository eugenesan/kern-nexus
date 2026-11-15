# Kernel and memory wipe

## 1. The kernel tree

`kern-nexus` uses the same hammerhead 3.4 CAF kernel as `krux-nexus`:

* fork: `eugenesan/android_kernel_lge_hammerhead`
* working branch: `airgap/cam-xz-wipe`
* head in the reference tree: `cc17c59739b`
* base for the patch series: `lineage-23.2`
* source: `linux/` submodule, `tools/kernel/patches/kern-nexus-kernel.patch`,
  profiles in `tools/kernel/profiles/`

The patch series includes:

* **camera/CPP/ISP**: `cf9fd9c` (keep a written buffer two frames),
  `93cca96` (CPP points at the buffer the ISP just wrote), `04893de`
  (return the ISP buffer), `0cd4362` (CPP `-ENODATA` on empty event queue),
  `bc0ddf0` (mask CAMIF IRQ for y-only), `916fb72` (y-only VFE completion);
* **CPP firmware loader**: `95f4b0f` (read the firmware from
  `/lib/firmware` directly);
* **XZ**: `02da9b9` (`CONFIG_KERNEL_XZ`), `4bf26c6`
  (`CONFIG_RD_XZ` initramfs);
* **USB gadget/adb**: `drivers/usb/gadget/android.c` and `f_fs.c` changes
  from the same branch;
* **memory wipe**: `wipe.c`, `sys.c`, `mm/init.c`, `init/main.c`,
  Kconfig/Makefile, `MEMORY_WIPE_ON_POWEROFF`;
* **panic restart**: `fdfae872` (restart on panic rather than power off);
* **defconfig/profile**.

KernelSU commits are **excluded**: kern-nexus runs as root from the
ramdisk and does not need KernelSU.  If a future feature needs it, import it
as a separate opt-in patch set.

## 2. Build configuration

`lineageos_hammerhead_defconfig` plus `tools/kernel/profiles/hammerhead.list`
and `hammerhead-keep.list`:

* `CONFIG_KERNEL_XZ=y` — XZ kernel (~4 MB compressed);
* `CONFIG_RD_XZ=y` — XZ ramdisk on the `fastboot boot` path;
* `CONFIG_MEMORY_WIPE=y`;
* `CONFIG_MEMORY_WIPE_ON_POWEROFF=y`;
* `CONFIG_NFC_BCM2079X=y`;
* camera/CPP/ISP options from `hammerhead-camera.list`;
* `CONFIG_DEVTMPFS=n` — TWRP's ueventd creates `/dev`, including the
  runtime camera nodes.  Do not toggle the tracked profile for a
  diagnostic build; use `KEEPLISTS=` (see `tools/kernel/README.md`).

Use `zImage-dtb`.  `zImage` has no appended DTB and the bootloader refuses
it.

## 3. The power-off wipe

### 3.1 Marker

The LGE persistent-RAM region is 1 MiB at `meminfo.bank[1].start + 1 MiB`;
the ram console used only the first 256 KiB.  The spare tail is preserved
across a warm reset and is RAM only:

```
KRUX/KERN marker physical: 0x100F0000
magic: 0x4b525857 ("KRXW")

struct kern_wipe_marker {
    u32 magic;
    u32 magic_inv;
    u32 seq;
    u32 crc;        /* CRC32 of the fields above */
};
```

`CONFIG_ANDROID_PERSISTENT_RAM=y` must stay even if the ram console is
turned off: it reserves the region.  The ram console itself is optional.

### 3.2 Flow

1. `sys_reboot(LINUX_REBOOT_CMD_POWER_OFF)` is intercepted (`kernel/sys.c`)
   unless this boot is already the wipe boot.
2. `kern_wipe_reboot()` writes the marker and calls
   `kernel_restart("recovery")`.
3. `msm_restart_prepare("recovery")` writes IMEM `0x77665502`; the
   bootloader boots the recovery partition (the same mechanism as
   `reboot recovery`).
4. At `mem_init()` (`arch/arm/mm/init.c` → `wipe_main()`), the marker is
   valid → `WIPE_MODE_ALL`, `wipe_from_marker = 1`.
5. The engine zeros free lowmem pages (bootmem bitmap) and unreserved
   highmem pages (`memblock` + `kmap_atomic_pfn`), then flushes caches.
6. `kernel_init()` calls `wipe_poweroff()` after the PMIC/restart drivers
   initialise; it clears the marker, prints `wipe: powering off`, and powers
   off through the normal PMIC path.  The direct PSHOLD write is only a
   fallback.
7. The marker is cleared only at that final power-off, so a crash during
   the boot re-wipes.

No NAND, no BCB, no userspace, no kexec, no second kernel.

### 3.3 Limits

* The wipe covers **free pages only**.  Reserved memory — framebuffer,
  scanout, ION/camera carveouts — is not wiped.
* The current kernel wipe does not scrub the display/ION carveouts.  Leave
  that as a separate hardening feature; it is not required for the wipe.
* If the recovery boot does not happen, the marker is gone with RAM and the
  wipe is not retried; the secrets are gone too.
* A charger can power the phone back on.  If the boot partition's Android
  is broken (`/system/bin/init` missing), the phone bootloops in Android:
  this is not the wipe.  Enter the bootloader (Volume Down + Power), select
  Recovery, or repair Android.

## 4. `k_run`, `k_next` and build flags

The kernel wipe is self-contained; there is no userspace wipe protocol to
implement.  In this document:

* **`k_run`** is the live boot.  Its only wipe responsibility is the normal
  shutdown path (`reboot(LINUX_REBOOT_CMD_POWER_OFF)`), which the kernel
  intercepts and turns into the marker/recovery boot.
* **`k_next`** is the next boot: `mem_init()` sees the marker, wipes free
  pages, and powers off before userspace.

Use the build flags for development instead of a separate wipe binary:

* `KERN_RELEASE=0` — development logs/features;
* `KERN_SEAL=0` — audit/dev image with adb;
* the delivered image uses `KERN_RELEASE=1`, `KERN_SEAL=1`.

The kernel does not wipe reserved memory (framebuffer, ION/camera
carveouts).  If the threat model requires it, add a display/camera scrub to
the shutdown path as a separate feature; it is not part of the current
kernel wipe.

## 5. Kernel build

```sh
bash scripts/build-kernel.sh
# -> /tmp/kern-nexus/artifacts/build/ko/arch/arm/boot/zImage-dtb
```

The script uses `linux/` when present; otherwise it clones the fork branch
or applies `tools/kernel/patches/kern-nexus-kernel.patch` to a `lineage-23.2`
tree.  `KERNEL_OUT` selects another output directory.  The output must be
on disk, not `/tmp` (tmpfs).

The kernel and ramdisk are packed by `scripts/build-image.sh` using the
TWRP template.  The image is gzip for a flashed recovery image; XZ only
for `fastboot boot`.

## 6. Verification

* `scripts/build-kernel.sh` prints the final config; check
  `CONFIG_MEMORY_WIPE_ON_POWEROFF=y`, `CONFIG_NFC_BCM2079X=y`,
  `CONFIG_KERNEL_XZ=y`, and the radios off.
* Boot an audit image and run `dmesg | grep wipe`; a `wipe=test` image
  wipes ~1488 MB and continues.
* The real test: use kern's Shutdown (or `reboot -p`), watch the serial
  trace for `wipe: marking RAM`, `restart reason = recovery`,
  `wipe: poweroff marker found`, `wipe: N MB wiped`, `wipe: powering off`.
* After the successful wipe the marker is cleared; the next normal boot
  must not re-wipe.
