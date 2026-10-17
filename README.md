# Luma Linux kernels

Kernel source for [Luma Mobile](https://simplyluma.com). Device support lives
in [SimplyLuma/luma](https://github.com/SimplyLuma/luma) under `devices/`; this
repository holds only kernels.

## One branch per SoC family

Each SoC family gets one long-lived branch. Every device on that SoC builds
from it, so a new phone on a supported chip needs a device manifest, not a new
kernel.

| Branch | SoC | Upstream base | Devices |
|---|---|---|---|
| [`milos`](https://github.com/SimplyLuma/linux/tree/milos) | Qualcomm SM7635 (milos) | [milos-mainline/linux](https://github.com/milos-mainline/linux) | Fairphone 6 |

Rules for a branch:

- It starts from a public upstream tree for that SoC (mainline Linux or the
  SoC's mainlining community tree) and keeps upstream history.
- Luma changes are ordinary commits on top, one change per commit, with the
  original author kept.
- Per-device build inputs (exact `.config`, device-tree overlays, build steps)
  go in `luma/<device>/<release>/` on that branch, not in the kernel tree.
- Moving to a new upstream base means a rebase onto the new base, with the old
  tip kept as a tag.
- No firmware blobs, keys or other secrets, ever. Firmware is listed by name
  and hash in the device manifest in SimplyLuma/luma and stays on the device.

## Tags: one per shipped kernel

Every kernel Luma distributes gets an annotated tag:

```
luma-<device>-<release>-<kernel release>
```

The tag message names the published file it corresponds to and the SHA-256 of
the boot image, kernel, DTB, `.config` and modules. That makes the tag the
complete corresponding source for that binary.

| Tag | Binary |
|---|---|
| [`luma-fp6-alpha1-7.1.2-luma-fp-ims1.gmu2.selinux1`](https://github.com/SimplyLuma/linux/tree/luma-fp6-alpha1-7.1.2-luma-fp-ims1.gmu2.selinux1) | Fairphone 6, Luma Mobile Alpha 1, `luma-mobile-alpha1-fp6-boot.bin` (`6b93fa90…`) and OS updates .21, .22 |

## Licence

Linux is GPL-2.0-only with the exceptions described in `COPYING` and
`LICENSES/` on each kernel branch.

If you received a Luma binary and cannot get its source here, open an issue at
https://github.com/SimplyLuma/luma/issues.
