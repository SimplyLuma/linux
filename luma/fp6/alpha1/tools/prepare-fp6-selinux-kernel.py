#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prepare an admitted fresh Kbuild plan; never compile, install or contact a phone.

This is the source/configuration owner for a GMU2 SELinux successor. The existing
bounded kernel controller must independently admit and execute the resulting
commands. In particular, this file is not a resource supervisor.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import tarfile

ARCHIVE_SHA = '7efbcf43ac7ecf3152c533269f694e48927854a7877c0a7b201c970e08914e05'
CONFIG_SHA = '130c9547d403a5b927930d7c1c7bff047b541bbb9cbe74b7fe7b3db29fffaf25'
RELEASE = '7.1.2-luma-fp-ims1.gmu2.selinux1'
LOCALVERSION = '-luma-fp-ims1.gmu2.selinux1'
LSM = 'landlock,lockdown,yama,loadpin,safesetid,selinux,smack,tomoyo,ipe,bpf'


def need(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def config_values(text):
    values = {}
    for line in text.splitlines():
        if line.startswith('CONFIG_') and '=' in line:
            key, value = line.split('=', 1)
        elif line.startswith('# CONFIG_') and line.endswith(' is not set'):
            key, value = line[2:-11], 'n'
        else:
            continue
        need(key not in values, 'duplicate Kconfig symbol: ' + key)
        values[key] = value
    return values


def validate_config(text):
    values = config_values(text)
    required = ['AUDIT', 'SECURITY', 'SECURITYFS', 'SECURITY_NETWORK',
                'SECURITY_SELINUX', 'NETWORK_SECMARK', 'NET', 'INET',
                'EXT4_FS_SECURITY', 'TMPFS_XATTR', 'DEFAULT_SECURITY_SELINUX',
                'SECURITY_SELINUX_DEVELOP']
    for name in required:
        need(values.get('CONFIG_' + name) == 'y', 'required Kconfig: ' + name)
    need(values.get('CONFIG_LSM') == json.dumps(LSM), 'existing LSM order changed')
    need(values.get('CONFIG_DEFAULT_SECURITY_DAC') == 'n', 'DAC legacy default remains')
    need(values.get('CONFIG_SECURITY_SELINUX_BOOTPARAM') == 'n', 'disable switch introduced')
    return values


def verify_source(archive, source, *, expected_archive_sha=ARCHIVE_SHA):
    """Compare every archived entry without extracting or mutating source."""
    need(source.is_dir() and source.resolve() == source, 'source must be canonical directory')
    need(digest(archive) == expected_archive_sha, 'recovered archive differs')
    expected = set()
    with tarfile.open(archive, 'r|gz') as entries:
        for entry in entries:
            rel = Path(entry.name)
            need(not rel.is_absolute() and '..' not in rel.parts and str(rel) == entry.name,
                 'noncanonical archive entry')
            need(entry.name not in expected, 'duplicate archive entry')
            expected.add(entry.name)
            p = source / rel
            need(p.parent.resolve().is_relative_to(source), 'source ancestry escaped')
            st = p.lstat()
            need(stat.S_IMODE(st.st_mode) == entry.mode, 'source mode differs: ' + entry.name)
            if entry.isfile():
                need(stat.S_ISREG(st.st_mode), 'source type differs')
                h = hashlib.sha256()
                with entries.extractfile(entry) as f:
                    for block in iter(lambda: f.read(1024 * 1024), b''):
                        h.update(block)
                need(digest(p) == h.hexdigest(), 'source body differs: ' + entry.name)
            elif entry.isdir():
                need(stat.S_ISDIR(st.st_mode), 'source directory differs')
            elif entry.issym():
                need(stat.S_ISLNK(st.st_mode) and os.readlink(p) == entry.linkname,
                     'source symlink differs')
                need(p.resolve().is_relative_to(source), 'source symlink escaped')
            else:
                raise ValueError('unsupported source archive entry')
    actual = {str(p.relative_to(source)) for p in source.rglob('*')}
    need(actual == expected, 'source membership differs')
    return len(expected)


def build_plan(source, output, config, fragment):
    base = ['make', '-C', str(source), 'O=' + str(output), 'ARCH=arm64', 'LLVM=1',
            'LD=ld.lld --threads=2 --thinlto-jobs=2', 'LOCALVERSION=' + LOCALVERSION,
            'KBUILD_BUILD_USER=luma', 'KBUILD_BUILD_HOST=kernel-cross',
            'KBUILD_BUILD_TIMESTAMP=2026-10-15 00:00:00 UTC', 'KBUILD_BUILD_VERSION=1']
    return {
        'schema': 'org.projectluma.fp6-selinux-kernel-plan/v1',
        'status': 'source-only; execution requires separate bounded-controller admission',
        'release': RELEASE, 'source_archive_sha256': ARCHIVE_SHA,
        'base_config_sha256': CONFIG_SHA, 'fragment_sha256': digest(fragment),
        'fresh_output_required': True, 'source': str(source), 'output': str(output),
        'configuration': {
            'copy_base_config': [str(config), str(output / '.config')],
            'merge': [str(source / 'scripts/kconfig/merge_config.sh'), '-m', '-O',
                      str(output), str(config), str(fragment)],
            'resolve': base + ['olddefconfig'],
            'verify': ['python3', str(Path(__file__).resolve()), 'verify-config',
                       '--config', str(output / '.config')],
            'review_full_config_diff_before_compile': True,
        },
        'phases': [
            {'name': 'built-in', 'cpu': 4, 'memory_bytes': 4 * 1024**3,
             'commands': [base + ['-j4', 'vmlinux_o', 'modules_prepare',
                                  'qcom/milos-fairphone-fp6.dtb']]},
            {'name': 'finish-with-native-host-btf', 'cpu': 2, 'memory_bytes': 8 * 1024**3,
             'commands': [base + ['-j1', 'vmlinuz.efi', 'modules',
                                  'qcom/milos-fairphone-fp6.dtb']]},
            {'name': 'module-install', 'cpu': 2, 'memory_bytes': 4 * 1024**3,
             'commands': [base + ['-j2', 'INSTALL_MOD_PATH=' + str(output / 'full-modules'),
                                  'modules_install'],
                          base + ['-j2', 'INSTALL_MOD_STRIP=1',
                                  'INSTALL_MOD_PATH=' + str(output / 'boot-modules'),
                                  'modules_install']]},
        ],
        'admission_required': ['Linux x86_64 tools and exact dependency hashes',
                               'clang/LLVM/LLD 22.1.8 Fedora 22.1.8-4.fc44 and pahole1.30',
                               'fresh Linux case-sensitive filesystem; no restored ARM host tools',
                               '12GiB workspace cap, parent budget and 12GiB host reserve',
                               'sequential phases, noSwap, resource/error cancellation supervisor',
                               'new module signing key/public-cert ownership and matching kernel trust'],
        'boot_policy_required': ['enforcing=1', 'no selinux=0 or overriding lsm= argument'],
        'postbuild_required': ['exact release/ARM64 ELF/BTF/module signature+vermagic checks',
                               'full config diff and unchanged inherited driver/DTB evidence',
                               'normal canonical board tuple/initramfs and complete source offer',
                               'writable-state label migration, enforcing clean boot and recovery',
                               'native no-Android functionality, physical OTA and rollback'],
        'compiled': False, 'install_authorized': False, 'public_release_qualified': False,
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='command', required=True)
    verify = sub.add_parser('verify-config')
    verify.add_argument('--config', type=Path, required=True)
    plan = sub.add_parser('plan')
    for name in ['archive', 'source', 'output', 'config', 'receipt']:
        plan.add_argument('--' + name, type=Path, required=True)
    args = p.parse_args()
    if args.command == 'verify-config':
        validate_config(args.config.read_text())
        print(json.dumps({'configuration_requirements_passed': True,
                          'runtime_enforcement_proven': False}))
        return
    paths = [args.archive, args.source, args.output, args.config, args.receipt]
    need(all(x.is_absolute() for x in paths), 'absolute paths required')
    need(not os.path.lexists(args.output), 'fresh output required')
    need(args.output.parent.resolve() == args.output.parent, 'output parent alias refused')
    need(not args.output.is_relative_to(args.source), 'out-of-tree build required')
    need(digest(args.config) == CONFIG_SHA, 'original config differs')
    fragment = Path(__file__).resolve().parents[2] / 'config/mobile/fp6-selinux/kernel.fragment'
    entries = verify_source(args.archive, args.source)
    result = build_plan(args.source, args.output, args.config, fragment)
    result['source_entries_verified'] = entries
    with args.receipt.open('x') as f:
        json.dump(result, f, indent=2)
        f.write('\n')


if __name__ == '__main__':
    main()
