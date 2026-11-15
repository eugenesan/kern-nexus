# linux/

The kernel is the hammerhead 3.4 CAF tree from
`eugenesan/android_kernel_lge_hammerhead`, branch `airgap/cam-xz-wipe`.
`kern-nexus` keeps only the changes as a patch so the new project does not
carry a 1.5 GB tree.

## Layout

```
patches/kern-nexus-kernel.patch   changes against lineage-23.2, KernelSU excluded
config/lineageos_hammerhead_defconfig
                                  the modified defconfig, KernelSU lines removed
```

The patch contains the camera/CPP/ISP fixes, XZ kernel/ramdisk support, the
USB gadget/adb fixes and the kernel-only power-off RAM wipe.  KernelSU is
deliberately excluded: this project runs kern as root from the initramfs.

## Using it

```sh
git clone -b lineage-23.2 --depth 1 \
    https://github.com/eugenesan/android_kernel_lge_hammerhead linux
git -C linux apply ../patches/kern-nexus-kernel.patch
cp config/lineageos_hammerhead_defconfig \
   linux/arch/arm/configs/kern_nexus_hammerhead_defconfig
```

`scripts/build-kernel.sh` does this automatically when `linux/` is not a
kernel tree, or uses an existing tree.  It builds with the host
`arm-linux-gnueabihf-gcc` and writes:

```
artifacts/built/ko/arch/arm/boot/zImage-dtb
artifacts/built/ko/.config
artifacts/built/ko.build.log
```

## Regenerating the patch

```sh
cd ../krux-nexus
git -C linux diff --binary lineage-23.2..airgap/cam-xz-wipe -- \
    . ':(exclude)drivers/kernelsu' \
      ':(exclude)drivers/Kconfig' \
      ':(exclude)drivers/Makefile' \
      ':(exclude)arch/arm/configs/lineageos_hammerhead_defconfig' \
      ':(exclude)arch/arm/configs/wiper_hammerhead_defconfig' \
    > ../kern-nexus/linux/patches/kern-nexus-kernel.patch
```

The defconfig is copied separately and the KernelSU block removed; the
patch does not reference `drivers/kernelsu`, so the tree configures without
it.

## Build options that matter

* `CONFIG_KERNEL_XZ=y`, `CONFIG_RD_XZ=y` — smaller kernel/ramdisk;
* `CONFIG_MEMORY_WIPE=y`, `CONFIG_MEMORY_WIPE_ON_POWEROFF=y`;
* `CONFIG_NFC_BCM2079X=y`;
* `CONFIG_DEVTMPFS=n` — TWRP ueventd creates `/dev`; the VFE diagnostic
  build can override via `KEEPLISTS=`, never by editing the tracked profile;
* `zImage-dtb`, not `zImage`.

See [../docs/kernel-and-wipe.md](../docs/kernel-and-wipe.md) for the wipe
design and the limits.
