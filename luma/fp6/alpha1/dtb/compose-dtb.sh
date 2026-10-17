#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Offline DT-only extraction of the canonical IMS/audio union owner.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")" && pwd)
base_dtb=$root/inputs/base.dtb
work=$root/dtb-composition
mkdir "$work"
merged_dtb=$work/milos-fairphone-fp6-ims-audio.dtb
o=$root/inputs
fingerprint_overlay=$o/milos-fairphone-fp6-fingerprint-base-0382-lab.dtso
fingerprint_enable_overlay=$o/milos-fairphone-fp6-fingerprint-lab-enable.dtso
qsee_pool_overlay=$o/milos-fairphone-fp6-qsee-pool.dtso
qsee_heaps_overlay=$o/milos-fairphone-fp6-qsee-dedicated-heaps.dtso
qsee_log_overlay=$o/milos-fairphone-fp6-qsee-log.dtso
appsbl_overlay=$o/milos-fairphone-fp6-qsee-appsbl-contract.dtso
compile_overlay() {
 cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp -I "$root/inputs/include" "$1" | dtc -@ -I dts -O dtb -o "$2"
}
apply_overlay() { fdtoverlay -i "$1" -o "$3" "$2"; }
[[ $(sha256sum "$base_dtb" | cut -d' ' -f1) == cd942c35f96d13c8ea22bcb9bab611abb8cf2a34fec9c95d2e17a56baa617c02 ]]
compile_overlay "$fingerprint_overlay" "$work/fingerprint.dtbo"
compile_overlay "$fingerprint_enable_overlay" "$work/fingerprint-enable.dtbo"
compile_overlay "$qsee_pool_overlay" "$work/qsee-pool.dtbo"
compile_overlay "$qsee_heaps_overlay" "$work/qsee-heaps.dtbo"
compile_overlay "$qsee_log_overlay" "$work/qsee-log.dtbo"
compile_overlay "$appsbl_overlay" "$work/appsbl.dtbo"

apply_overlay "$base_dtb" "$work/fingerprint.dtbo" "$work/merge-1.dtb"
apply_overlay "$work/merge-1.dtb" "$work/fingerprint-enable.dtbo" "$work/merge-2.dtb"
apply_overlay "$work/merge-2.dtb" "$work/qsee-pool.dtbo" "$work/merge-3.dtb"
fdtput -d "$work/merge-3.dtb" /firmware/scm memory-region
apply_overlay "$work/merge-3.dtb" "$work/qsee-heaps.dtbo" "$work/merge-4.dtb"
apply_overlay "$work/merge-4.dtb" "$work/qsee-log.dtbo" "$work/merge-5.dtb"
apply_overlay "$work/merge-5.dtb" "$work/appsbl.dtbo" "$work/merge-6.dtb"

# Overlays cannot rename nodes. Reproduce the accepted stock-visible QSEE node
# names, then restore the accepted matched-CMA reusable pool semantics.
dtc -I dtb -O dts -o "$work/pre-rename.dts" "$work/merge-6.dtb" 2>"$work/pre-rename.warn"
python3 - "$work/pre-rename.dts" "$work/renamed.dts" <<'PY'
from pathlib import Path
import sys

source, output = map(Path, sys.argv[1:])
text = source.read_text()
for old, new in {
    "qseecom-apps-pool": "qseecom_region",
    "qseecom-ta-pool": "qseecom_ta_region",
}.items():
    count = text.count(old)
    if count != 3:
        raise SystemExit(f"unexpected {old!r} occurrence count: {count}")
    text = text.replace(old, new)
output.write_text(text)
PY
dtc -@ -I dts -O dtb -o "$merged_dtb" "$work/renamed.dts" 2>"$work/renamed.warn"
for pool in /reserved-memory/qseecom_region /reserved-memory/qseecom_ta_region; do
  fdtput -d "$merged_dtb" "$pool" no-map
  fdtput "$merged_dtb" "$pool" reusable
done

sha256sum "$merged_dtb"
