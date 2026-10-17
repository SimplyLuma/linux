# Fairphone 6: Luma Mobile Alpha 1 kernel

This branch (`milos`) is the complete corresponding source for the kernel
that Luma Mobile Alpha 1 ships on the Fairphone 6 (Qualcomm SM7635 "milos").

| | |
|---|---|
| Kernel release | `7.1.2-luma-fp-ims1.gmu2.selinux1` |
| `uname -v` | `#1 SMP PREEMPT 2026-10-15 00:00:00 UTC` |
| Toolchain | clang 22.1.8 (Fedora 22.1.8-4.fc44), LLD 22.1.8, pahole 1.30 |
| Upstream base | [milos-mainline/linux](https://github.com/milos-mainline/linux) `dfe0125e73541d1984e4d2d37bd069a036bf0451` |
| Luma patches | 88 commits on top of the base, in the order they were applied |
| Tag | `luma-fp6-alpha1-7.1.2-luma-fp-ims1.gmu2.selinux1` |

## Which binary this is the source for

Public boot image
`https://dl.simplyluma.com/mobile/fp6/alpha1/luma-mobile-alpha1-fp6-boot.bin`
(Android boot image header v2, page size 4096):

| Part | SHA-256 |
|---|---|
| `luma-mobile-alpha1-fp6-boot.bin` | `6b93fa901d85152efb29e9a58874d040e86fcad597c4dae0c9b11c897cf1051b` |
| kernel (`arch/arm64/boot/vmlinuz`, gzip Image, 13166500 bytes) | `b70c803080b46d729bdf116ff60e6b9237f3152cde905558c2c64ce670de4ded` |
| DTB (composed, 89885 bytes) | `4bbb62955e4d6bfad0e7fdcd80235505c53c7c8deb3a90c54841a7dc1ee94d3f` |
| DTB before overlays (`qcom/milos-fairphone-fp6.dtb`) | `cd942c35f96d13c8ea22bcb9bab611abb8cf2a34fec9c95d2e17a56baa617c02` |
| `.config` (also embedded via `CONFIG_IKCONFIG`) | `658c427002de002f218ad178a61f8da55f34c1980e1b22d9d0480b54fccd9817` |
| modules archive (`/usr/lib/modules/7.1.2-luma-fp-ims1.gmu2.selinux1`) | `c3b61181229aa3f9237f27095e143f424a42ccf60c6a6233f8852024608b0c93` |

The `.config` embedded in that kernel (`scripts/extract-ikconfig`) is
byte-identical to `config-7.1.2-luma-fp-ims1.gmu2.selinux1` here.

The kernel source tree at the tag, minus this `luma/` directory, is
file-for-file identical to the tree the kernel was built from (99,929 entries).
The build tree also held stray build outputs (`*.o`, `*.ko`, `.*.cmd`,
`__pycache__`) and an internal provenance manifest; they are not source and
are not included.

OS updates `.21` and `.22` on the FP6 update channel keep the same boot tuple
(kernel, DTB and modules), so this source covers them too.

## Files

- `config-7.1.2-luma-fp-ims1.gmu2.selinux1`: the exact resolved `.config`.
- `base.config` and `selinux.fragment`: how it was produced (`merge_config.sh`
  of the fragment onto the base, then `olddefconfig`).
- `dtb/`: the device-tree overlays and the script that composes the shipped
  DTB from the kernel-built `milos-fairphone-fp6.dtb`.
- `tools/`: the build controller scripts (Apache-2.0) that ran the phases.
- `BUILD.md`: how to rebuild.
- `SHA256SUMS`: hashes of the files in this directory.

## Initramfs

The ramdisk in the same boot image
(`944489269addf33f8cd5e8f305ccc368573396e26df03dc6ea63970242659a82`) is an
unmodified postmarketOS edge initramfs. Its sources are upstream:

- Scripts: `postmarketos-initramfs` 3.12.0-r0,
  [pmaports `41732e213ba0`](https://gitlab.postmarketos.org/postmarketOS/pmaports/-/tree/41732e213ba0/main/postmarketos-initramfs).
  `init`, `init_2nd.sh`, `init_functions.sh` and `init_functions_2nd.sh` are
  byte-identical to that commit (apart from the version string `abuild`
  substitutes into `init`).
- Kernel modules under `lib/modules/7.1.2/` (16 modules plus their index
  files): postmarketOS
  `linux-postmarketos-qcom-milos` 7.1.2
  ([pmaports `5689eb3c5174`](https://gitlab.postmarketos.org/postmarketOS/pmaports/-/tree/5689eb3c5174/device/testing/linux-postmarketos-qcom-milos)),
  built from [milos-mainline/linux `v7.1.2-milos`](https://github.com/milos-mainline/linux/tree/v7.1.2-milos).
- Userspace (busybox, kmod, plymouth, buffybox, e2fsprogs and others): the
  matching Alpine Linux edge and postmarketOS edge packages
  ([aports](https://gitlab.alpinelinux.org/alpine/aports),
  [pmaports](https://gitlab.postmarketos.org/postmarketOS/pmaports)).

## Licence

The kernel is GPL-2.0-only with the per-file exceptions in `COPYING` and
`LICENSES/`. Files under `luma/` carry their own SPDX identifiers
(Apache-2.0 for the overlays and scripts).

## Getting this source another way

If you received a Luma Mobile binary and cannot get this source from GitHub,
open an issue at https://github.com/SimplyLuma/luma/issues and we will
provide it.
