#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify normal installed kernel modules and create a runtime-only board archive.

Never mutates either modules_install tree; never exports the private signing key.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import tarfile
import tempfile

ABI = '7.1.2-luma-fp-ims1.gmu2.selinux1'
MARKER = b'~Module signature appended~\n'
REQUIRED = ['imx896', 'ov13b10', 's5kkd1sp', 'v4l2_cci', 'dw9714', 'dw9784', 'ipa']


def need(ok, message):
    if not ok:
        raise ValueError(message)


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def artifact(path):
    need(path.is_file() and not path.is_symlink(), 'regular artifact: ' + str(path))
    return {'path': str(path), 'sha256': sha(path), 'bytes': path.stat().st_size}


def command(argv):
    return subprocess.check_output(argv, text=True, stderr=subprocess.PIPE, timeout=120).strip()


def signature_parts(body):
    need(body.endswith(MARKER) and len(body) > len(MARKER) + 12, 'module signature marker')
    end = len(body) - len(MARKER)
    trailer = body[end - 12:end]
    algo, digest, kind, signer, key, padding, length = struct.unpack('>BBBBB3sI', trailer)
    need((algo, digest, kind, signer, key, padding) == (0, 0, 2, 0, 0, b'\0\0\0'),
         'unexpected module signature format')
    start = end - 12 - length
    need(length > 0 and start > 0, 'signature length')
    return body[:start], body[start:end - 12]


def verify_pkcs7(body, signature, certificate_pem, temporary):
    (temporary/'body').write_bytes(body)
    (temporary/'signature').write_bytes(signature)
    command(['openssl', 'cms', '-verify', '-binary', '-inform', 'DER', '-noverify',
             '-nointern', '-certfile', str(certificate_pem), '-in', str(temporary/'signature'),
             '-content', str(temporary/'body'), '-out', '/dev/null'])


def module_files(root):
    return sorted(p for p in root.rglob('*') if p.is_file() and
                  (p.name.endswith('.ko') or p.name.endswith('.ko.zst') or p.name.endswith('.ko.xz') or p.name.endswith('.ko.gz')))


def module_body(path, stored=None):
    stored = path.read_bytes() if stored is None else stored
    if path.name.endswith('.zst'):
        return subprocess.check_output(['zstd', '-q', '-d', '-c'], input=stored, timeout=60)
    if path.name.endswith('.xz'):
        import lzma
        return lzma.decompress(stored)
    if path.name.endswith('.gz'):
        import gzip
        return gzip.decompress(stored)
    return stored


def normalized_members(root):
    excluded = []
    members = []
    for path in sorted(root.rglob('*')):
        rel = path.relative_to(root)
        st = path.lstat()
        if rel.as_posix() in ('build', 'source'):
            need(stat.S_ISLNK(st.st_mode), 'development entry must be symlink')
            excluded.append({'path': str(path), 'target': os.readlink(path)})
            continue
        need(stat.S_ISREG(st.st_mode) or stat.S_ISDIR(st.st_mode),
             'unexpected runtime entry (no blanket symlink removal): ' + str(path))
        need(st.st_uid == 0 and not st.st_mode & 0o022, 'unsafe module metadata')
        members.append(path)
    return members, excluded


def write_runtime_archive(tree, members, archive, module_seals):
    """Archive held bytes, then independently read every emitted file back."""
    inventory = []
    prefix = 'usr/lib/modules/' + ABI
    archive_modules = {str(p.relative_to(tree)) for p in members if any(p.name.endswith(suffix) for suffix in ('.ko', '.ko.zst', '.ko.xz', '.ko.gz'))}
    need(archive_modules == set(module_seals), 'archive module membership differs from qualified set')
    with tarfile.open(archive, 'x') as tar:
        root = tarfile.TarInfo(prefix)
        root.type = tarfile.DIRTYPE; root.mode = 0o755; root.mtime = 1783209600
        tar.addfile(root)
        for path in members:
            rel = str(path.relative_to(tree))
            name = prefix + '/' + rel
            info = tar.gettarinfo(str(path), arcname=name)
            info.uid = info.gid = 0; info.uname = info.gname = ''; info.mtime = 1783209600
            info.mode = stat.S_IMODE(info.mode)
            if info.isfile():
                stored = path.read_bytes()
                digest = hashlib.sha256(stored).hexdigest()
                need(len(stored) == info.size, 'archive input size changed')
                if rel in module_seals:
                    seal = module_seals[rel]
                    need(digest == seal['sha256'] and len(stored) == seal['bytes'], 'qualified module changed before archive')
                tar.addfile(info, io.BytesIO(stored))
                inventory.append({'path': name, 'sha256': digest, 'bytes': len(stored), 'mode': info.mode})
            else:
                need(info.isdir(), 'unexpected archive input type')
                tar.addfile(info)
    with tarfile.open(archive, 'r') as tar:
        need(tar.getmember(prefix).isdir() and tar.getmember(prefix).mode == 0o755, 'ABI directory metadata')
        files = {m.name: m for m in tar.getmembers() if m.isfile()}
        need(set(files) == {r['path'] for r in inventory}, 'archive file membership differs')
        for row in inventory:
            member = files[row['path']]
            with tar.extractfile(member) as stream:
                need(hashlib.file_digest(stream, 'sha256').hexdigest() == row['sha256'] and member.size == row['bytes'] and member.mode == row['mode'], 'archive readback differs')
    return inventory


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--workspace', type=Path, required=True)
    p.add_argument('--baseline-module-root', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--dtb-composition-receipt', type=Path, required=True)
    p.add_argument('--dtb-composition-sha256', required=True)
    args = p.parse_args()
    import sys
    need(os.geteuid() == 0 and not sys.flags.optimize, 'unoptimized root required')
    w, dest = args.workspace, args.output
    need(w.resolve() == w and dest.resolve() == dest and not os.path.lexists(dest), 'fresh canonical output required')
    out = w / 'out'
    config = out / '.config'
    phases = {}
    for name in ['built-in', 'finish-with-native-host-btf', 'module-install']:
        path = w / ('phase-' + name) / 'actual.json'
        r = json.loads(path.read_text())
        need(r['passed'] and r['workspace'] == str(w) and r['resolved_config_sha256'] == sha(config), 'phase proof mismatch')
        phases[name] = artifact(path)
    need((out / 'include/config/kernel.release').read_text().strip() == ABI, 'kernel release mismatch')
    need(args.baseline_module_root.is_dir() and args.baseline_module_root.resolve() == args.baseline_module_root, 'canonical baseline required')
    tree = out / 'boot-modules/lib/modules' / ABI
    full = out / 'full-modules/lib/modules' / ABI
    modules = module_files(tree)
    need(modules and full.is_dir(), 'normal install trees required')
    expected = {line.strip().removesuffix('.o').removesuffix('.ko') for line in (out / 'modules.order').read_text().splitlines() if line.strip()}
    observed = {str(p.relative_to(tree)).removeprefix('kernel/').split('.ko', 1)[0] for p in modules}
    full_observed = {str(p.relative_to(full)).removeprefix('kernel/').split('.ko', 1)[0] for p in module_files(full)}
    need(observed == expected == full_observed, 'complete modules.order/install membership differs')
    dest.mkdir(mode=0o700)
    cert = out / 'certs/signing_key.x509'
    old_names = sorted({command(['modinfo', '-F', 'name', str(p)]) for p in module_files(args.baseline_module_root)})
    need(old_names, 'baseline module inventory empty')
    inventory = []
    with tempfile.TemporaryDirectory(prefix='module-signature-', dir=dest) as scratch:
        t = Path(scratch)
        command(['openssl', 'x509', '-inform', 'DER', '-in', str(cert), '-out', str(t/'public.pem')])
        for path in modules:
            stored = path.read_bytes()
            stored_sha = hashlib.sha256(stored).hexdigest()
            body, signature = signature_parts(module_body(path, stored))
            need(body[:4] == b'\x7fELF' and body[4:6] == b'\x02\x01' and int.from_bytes(body[18:20], 'little') == 183, 'module is not AArch64 ELF')
            verify_pkcs7(body, signature, t/'public.pem', t)
            vermagic = command(['modinfo', '-F', 'vermagic', str(path)])
            need(vermagic.split()[0] == ABI, 'module vermagic differs')
            name = command(['modinfo', '-F', 'name', str(path)])
            need(sha(path) == stored_sha, 'module changed during qualification')
            inventory.append({'path': str(path.relative_to(tree)), 'name': name, 'sha256': stored_sha, 'bytes': len(stored), 'vermagic': vermagic, 'pkcs7_verified': True, 'signature_sha256': hashlib.sha256(signature).hexdigest()})
    names = {r['name'] for r in inventory}
    need(set(old_names) <= names and set(REQUIRED) <= names, 'old hardware module coverage missing')
    members, excluded = normalized_members(tree)
    archive = dest / ('modules-' + ABI + '.tar')
    module_seals = {r['path']: r for r in inventory}
    archive_inventory = write_runtime_archive(tree, members, archive, module_seals)
    roles = {'CONFIG_DRM_MSM': 'drivers/gpu/drm/msm/msm_drv.o',
             'CONFIG_SCSI_UFS_QCOM': 'drivers/ufs/host/ufs-qcom.o',
             'CONFIG_USB_DWC3_QCOM': 'drivers/usb/dwc3/dwc3-qcom.o',
             'CONFIG_QRTR': 'net/qrtr/af_qrtr.o',
             'CONFIG_RPMSG_QCOM_GLINK': 'drivers/rpmsg/qcom_glink_native.o'}
    config_lines = set(config.read_text().splitlines())
    linked_members = set(command(['llvm-ar', 't', str(out/'vmlinux.a')]).splitlines())
    need('CONFIG_QCOM_Q6V5_PAS=m' in config_lines and 'qcom_q6v5_pas' in names, 'remoteproc role missing')
    role_rows = {}
    for symbol, member in roles.items():
        need(symbol+'=y' in config_lines and str(out/member) in linked_members, 'built-in role missing: '+symbol)
        role_rows[symbol] = {'value': 'y', 'linked_archive_member': member, 'object': artifact(out/member)}
    builtin = {'passed': True, 'config_sha256': sha(config), 'roles': role_rows,
               'modules_builtin': artifact(out/'modules.builtin'),
               'linked_vmlinux_object': artifact(out/'vmlinux.o'),
               'linked_archive_sha256': sha(out/'vmlinux.a'),
               'remoteproc': {'symbol': 'CONFIG_QCOM_Q6V5_PAS', 'value': 'm', 'module_name': 'qcom_q6v5_pas'}}
    (dest/'builtin-role-proof.json').write_text(json.dumps(builtin, sort_keys=True, indent=2)+'\n')
    raw_dtb = out/'arch/arm64/boot/dts/qcom/milos-fairphone-fp6.dtb'
    dtb_receipt = args.dtb_composition_receipt
    need(artifact(dtb_receipt)['sha256'] == args.dtb_composition_sha256, 'DTB composition receipt seal')
    dtb_proof = json.loads(dtb_receipt.read_text())
    need(dtb_proof['schema'] == 'org.projectluma.fp6-source-composed-dtb/v1' and dtb_proof['passed'], 'native DTB source composition proof')
    need(sha(raw_dtb) == dtb_proof['raw_build_dtb']['sha256'] and raw_dtb.stat().st_size == dtb_proof['raw_build_dtb']['bytes'], 'final raw build DTB differs from composed source input')
    final_dtb = Path(dtb_proof['dtb']['path'])
    need(final_dtb.is_absolute() and final_dtb.resolve() == final_dtb, 'canonical composed DTB')
    need(artifact(final_dtb) == dtb_proof['dtb'], 'composed DTB artifact differs')
    artifacts = {'kernel': artifact(out/'arch/arm64/boot/vmlinuz'), 'dtb': artifact(final_dtb), 'raw_build_dtb': artifact(raw_dtb), 'dtb_composition_receipt': artifact(dtb_receipt), 'runtime_modules_archive': artifact(archive), 'module_symvers': artifact(out/'Module.symvers'), 'system_map': artifact(out/'System.map'), 'modules_builtin': artifact(out/'modules.builtin'), 'modules_builtin_modinfo': artifact(out/'modules.builtin.modinfo'), 'public_signing_certificate': artifact(cert), 'config': artifact(config)}
    for name, body in [('module-inventory.json', inventory), ('archive-inventory.json', archive_inventory), ('baseline-module-names.json', old_names)]:
        (dest/name).write_text(json.dumps(body, sort_keys=True, indent=2)+'\n')
    artifacts['module_inventory'] = artifact(dest/'module-inventory.json')
    artifacts['builtin_role_proof'] = artifact(dest/'builtin-role-proof.json')
    receipt = {'schema': 'org.projectluma.fp6-kernel-artifact-qualification/v1', 'passed': True, 'workspace': str(w), 'kernel_release': ABI, 'source_archive_sha256': sha(w/'patched-linux-gmu2-exact-source.tar.gz'), 'resolved_config_sha256': sha(config), 'artifacts': artifacts, 'phase_receipts': phases, 'toolchain': {'controller_sha256': sha(w/'run-fp6-selinux-kernel-phase.py'), 'preparer_sha256': sha(w/'prepare-fp6-selinux-kernel.py'), 'admission_sha256': sha(w/'module-install-admission-r1.json')}, 'module_proof': {'module_count': len(modules), 'modules_order_sha256': sha(out/'modules.order'), 'all_order_entries_present': True, 'all_vermagic_match': True, 'verified_pkcs7_signatures': len(inventory), 'old_module_names_sha256': sha(dest/'baseline-module-names.json'), 'old_module_names_count': len(old_names), 'missing_old_module_names': [], 'required_camera_ipa_names': REQUIRED, 'missing_required_names': [], 'excluded_development_links': excluded}, 'archive_inventory_sha256': sha(dest/'archive-inventory.json'), 'install_authorized': False, 'public_release_qualified': False}
    with (dest/'qualification.json').open('x') as f:
        json.dump(receipt, f, indent=2); f.flush(); os.fsync(f.fileno())
    print(json.dumps(receipt))


if __name__ == '__main__':
    main()
