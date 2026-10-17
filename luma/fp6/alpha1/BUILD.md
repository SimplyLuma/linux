# Rebuilding the FP6 Alpha 1 kernel

The shipped kernel was cross-built on an x86_64 Fedora 44 host with
clang/LLVM/LLD 22.1.8 (`Fedora 22.1.8-4.fc44`) and pahole 1.30. Any host with
the same toolchain works. The source tree must be on a case-sensitive
filesystem.

```sh
git clone -b milos https://github.com/SimplyLuma/linux.git
cd linux
git checkout luma-fp6-alpha1-7.1.2-luma-fp-ims1.gmu2.selinux1
O=../out-fp6
mkdir "$O"
```

## 1. Configuration

```sh
cp luma/fp6/alpha1/base.config "$O/.config"
scripts/kconfig/merge_config.sh -m -O "$O" "$O/.config" luma/fp6/alpha1/selinux.fragment
make O="$O" ARCH=arm64 LLVM=1 olddefconfig
```

The result should equal `luma/fp6/alpha1/config-7.1.2-luma-fp-ims1.gmu2.selinux1`.
If your toolchain differs, the host-derived symbols (`CONFIG_CC_VERSION_TEXT`,
Rust and `CC_CAN_LINK` probes) will differ; copy the shipped config
straight to `$O/.config` to skip this step.

## 2. Build

Every invocation uses the same variables:

```sh
K="make O=$O ARCH=arm64 LLVM=1 \
  LOCALVERSION=-luma-fp-ims1.gmu2.selinux1 \
  KBUILD_BUILD_USER=luma KBUILD_BUILD_HOST=kernel-cross \
  KBUILD_BUILD_TIMESTAMP='2026-10-15 00:00:00 UTC' KBUILD_BUILD_VERSION=1"

$K -j4 vmlinux_o modules_prepare qcom/milos-fairphone-fp6.dtb
$K -j1 vmlinuz.efi modules qcom/milos-fairphone-fp6.dtb
$K INSTALL_MOD_PATH="$O/full-modules" modules_install
$K INSTALL_MOD_STRIP=1 INSTALL_MOD_PATH="$O/boot-modules" modules_install
```

The kernel that goes into the boot image is `$O/arch/arm64/boot/vmlinuz`.
`tools/prepare-fp6-selinux-kernel.py` holds the same commands as data, and
`tools/run-fp6-selinux-kernel-phase.py` is the controller that ran each phase
in a resource-limited, network-less sandbox.

Module signing is on (`CONFIG_MODULE_SIG_ALL`). Kbuild generates a fresh
`certs/signing_key.pem` on first build. The private key used for the shipped
modules is not published and isn't needed: a rebuilt kernel trusts the key it
was built with. The shipped public certificate has SHA-256
`18a5bfdd06d2f04aca484ffb41a5fb0f37568a4858cb24194440c38da157a23d`.

Because of the signing key (and BTF), a rebuild is functionally equivalent
rather than bit-identical.

## 3. Device tree

The shipped DTB is the kernel-built `milos-fairphone-fp6.dtb` with the
overlays in `dtb/` applied. `dtb/compose-dtb.sh` expects this layout next to
it:

```
inputs/base.dtb                                    # $O/arch/arm64/boot/dts/qcom/milos-fairphone-fp6.dtb
inputs/include/dt-bindings/interrupt-controller/irq.h   # from this tree
inputs/milos-fairphone-fp6-*.dtso                  # from dtb/
```

It needs `cpp`, `dtc`, `fdtoverlay`, `fdtput` (dtc 1.7.2) and `python3`, and
prints the SHA-256 of `dtb-composition/milos-fairphone-fp6-ims-audio.dtb`.
Expected: `4bbb62955e4d6bfad0e7fdcd80235505c53c7c8deb3a90c54841a7dc1ee94d3f`.

## 4. Boot image

On the phone the OS builds `boot` itself on every update (libostree's aboot
backend), from the deployed kernel, DTB and initramfs. Header version 2, page
size 4096, kernel offset `0x8000`, ramdisk offset `0x1000000`, DTB offset
`0x1f00000`, tags offset `0x100`. The kernel command line is in
`devices/fairphone/fp6/` in
[SimplyLuma/luma](https://github.com/SimplyLuma/luma).
