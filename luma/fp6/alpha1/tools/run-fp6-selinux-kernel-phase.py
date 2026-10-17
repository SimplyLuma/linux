#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run one separately admitted Kbuild phase, never deploy a kernel.

Source candidate only: no admission is bundled. Root must seal actual resource,
tool, source and (after configuration) reviewed configuration-diff evidence.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import sys
import time

SPEC = importlib.util.spec_from_file_location(
    'kernel_preparer', Path(__file__).with_name('prepare-fp6-selinux-kernel.py'))
OWNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(OWNER)
GiB = 1024**3
FLOOR = 2281701376
HOST_RESERVE = 12 * GiB
WORKSPACE_CAP = 12 * GiB
PARENT = 'luma-fp6-export-r1.slice'


def run(command):
    return subprocess.check_output(command, text=True, timeout=20).strip()


def interrupted(signum, frame):
    raise InterruptedError('controller received signal ' + str(signum))


def cancel_owned(unit, child, result, *, runner=subprocess.run, killpg=os.killpg):
    if unit:
        try:
            runner(['systemctl', 'stop', unit + '.scope'], timeout=30, check=True)
        except (OSError, subprocess.TimeoutExpired, subprocess.CalledProcessError) as stop_error:
            result['scope_stop_error'] = str(stop_error)
            try:
                runner(['systemctl', 'kill', '--signal=KILL', unit + '.scope'],
                       timeout=10, check=True)
            except (OSError, subprocess.TimeoutExpired, subprocess.CalledProcessError) as kill_error:
                result['scope_kill_error'] = str(kill_error)
    if child and child.poll() is None:
        try:
            killpg(child.pid, signal.SIGKILL)
            child.wait(timeout=30)
        except (ProcessLookupError, subprocess.TimeoutExpired) as child_error:
            result['child_stop_error'] = str(child_error)


def bounded_command(unit, cpu, memory, output, temp, command):
    return ['systemd-run', '--scope', '--unit=' + unit, '--slice=' + PARENT,
            '-p', 'MemoryMax=' + str(memory), '-p', 'MemorySwapMax=0',
            '-p', 'CPUQuota=' + str(cpu * 100) + '%',
            'bwrap', '--die-with-parent', '--unshare-net', '--unshare-pid',
            '--ro-bind', '/', '/', '--bind', str(output), str(output),
            '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp',
            '--setenv', 'TMPDIR', str(temp), '--setenv', 'CCACHE_DISABLE', '1',
            '--setenv', 'PATH', '/usr/bin:/bin', '--chdir', str(output), '--'] + command


def regular(path, digest):
    st = path.lstat()
    OWNER.need(path.is_file() and not path.is_symlink() and st.st_uid == 0
               and not st.st_mode & 0o022, 'root-owned immutable input required: ' + str(path))
    OWNER.need(OWNER.digest(path) == digest, 'input seal differs: ' + str(path))


def available():
    return next(int(x.split()[1]) * 1024 for x in Path('/proc/meminfo').read_text().splitlines()
                if x.startswith('MemAvailable:'))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--admission', type=Path, required=True)
    p.add_argument('--admission-sha256', required=True)
    p.add_argument('--phase', choices=['configure', 'reconfigure', 'built-in',
                                     'finish-with-native-host-btf', 'module-install'], required=True)
    args = p.parse_args()
    OWNER.need(not sys.flags.optimize and os.geteuid() == 0
               and platform.system() == 'Linux' and platform.machine() == 'x86_64',
               'unoptimized root Linux/x86_64 execution required')
    # Avoid an unsealed user PATH or host ccache launcher choosing different tools.
    os.environ['PATH'] = '/usr/bin:/bin'
    for name in ['MAKEFLAGS', 'MFLAGS', 'GNUMAKEFLAGS', 'KCONFIG_CONFIG',
                 'LLVM', 'LLVM_IAS', 'CC', 'HOSTCC', 'CXX', 'HOSTCXX', 'LD',
                 'HOSTLD', 'AR', 'NM', 'OBJCOPY', 'OBJDUMP', 'STRIP',
                 'CROSS_COMPILE', 'KCFLAGS', 'KCPPFLAGS', 'KBUILD_OUTPUT',
                 'KBUILD_EXTMOD', 'LDFLAGS', 'CFLAGS', 'CPPFLAGS', 'PAHOLE']:
        os.environ.pop(name, None)
    regular(args.admission, args.admission_sha256)
    a = json.loads(args.admission.read_text())
    OWNER.need(a['schema'] == 'org.projectluma.fp6-selinux-kernel-admission/v1', 'admission schema')
    OWNER.need(a['phase'] == args.phase and a['parent_slice'] == PARENT, 'phase/parent differs')
    workspace = Path(a['workspace'])
    OWNER.need(workspace.is_absolute() and workspace.resolve() == workspace
               and workspace != Path('/') and workspace.stat().st_uid == 0
               and not workspace.stat().st_mode & 0o022, 'private workspace required')
    source, output = workspace / 'source', workspace / 'out'
    archive, config = Path(a['source_archive']), Path(a['original_config'])
    regular(archive, OWNER.ARCHIVE_SHA)
    regular(config, OWNER.CONFIG_SHA)
    regular(Path(__file__), a['controller_sha256'])
    regular(Path(OWNER.__file__), a['preparer_sha256'])
    fragment = Path(a['fragment'])
    regular(fragment, a['fragment_sha256'])
    OWNER.need(OWNER.verify_source(archive, source) == 99929, 'whole source count differs')
    OWNER.need(Path('/proc/sys/kernel/random/boot_id').read_text().strip() == a['host_boot_id'],
               'host rebooted since admission')
    required_tools = {'make', 'clang', 'ld.lld', 'pahole', 'bwrap', 'systemd-run',
                      'systemctl', 'du', 'python3', 'llvm-ar', 'llvm-nm',
                      'llvm-objcopy', 'llvm-objdump', 'llvm-readelf', 'llvm-strip',
                      'flex', 'bison', 'bc', 'openssl', 'pkg-config', 'perl',
                      'gzip', 'zstd', 'tar', 'find', 'sed', 'awk', 'bash', 'sh'}
    if args.phase == 'module-install':
        required_tools |= {'depmod', 'modinfo'}
    OWNER.need(required_tools <= set(a['tools']), 'incomplete tool identity map')
    for name, record in a['tools'].items():
        executable = Path(shutil.which(name) or '/missing')
        OWNER.need(executable.resolve() == Path(record['path']), 'PATH provider differs: ' + name)
        regular(executable.resolve(), record['sha256'])
    OWNER.need(a['tool_dependencies'], 'native dynamic/devel provider closure absent')
    for path, seal in a['tool_dependencies'].items():
        dependency = Path(path)
        OWNER.need(dependency.is_absolute() and dependency.resolve() == dependency,
                   'dependency must name resolved absolute provider')
        regular(dependency, seal)
    OWNER.need(run(['clang', '--version']).splitlines()[0] ==
               'clang version 22.1.8 (Fedora 22.1.8-4.fc44)', 'clang differs')
    OWNER.need(run(['ld.lld', '--version']).startswith('LLD 22.1.8'), 'linker differs')
    OWNER.need(run(['pahole', '--version']) == 'v1.30', 'pahole differs')
    plan = OWNER.build_plan(source, output, config, fragment)
    if args.phase in ('configure', 'reconfigure'):
        if args.phase == 'configure':
            OWNER.need(not os.path.lexists(output), 'fresh output required')
        else:
            OWNER.need(output.is_dir() and output.resolve() == output and output.stat().st_uid == 0
                       and not output.stat().st_mode & 0o022, 'private canonical output required')
            regular(output / '.config', a['resolved_config_sha256'])
            previous = Path(a['previous_phase_receipt'])
            regular(previous, a['previous_phase_receipt_sha256'])
            prior = json.loads(previous.read_text())
            OWNER.need(prior['passed'] and prior['phase'] == 'finish-with-native-host-btf'
                       and prior['workspace'] == str(workspace)
                       and prior['resolved_config_sha256'] == a['resolved_config_sha256'],
                       'reconfigure requires completed prior build')
        cpu, memory = 2, 4 * GiB
        commands = [plan['configuration']['merge'], plan['configuration']['resolve'],
                    plan['configuration']['verify']]
    else:
        OWNER.need(output.is_dir() and output.resolve() == output and output.stat().st_uid == 0
                   and not output.stat().st_mode & 0o022, 'private canonical output required')
        phase = next(x for x in plan['phases'] if x['name'] == args.phase)
        cpu, memory, commands = phase['cpu'], phase['memory_bytes'], phase['commands']
        regular(output / '.config', a['resolved_config_sha256'])
        OWNER.validate_config((output / '.config').read_text())
        old = OWNER.config_values(config.read_text())
        new = OWNER.config_values((output / '.config').read_text())
        changes = {k: [old.get(k), new.get(k)] for k in sorted(set(old) | set(new))
                   if old.get(k) != new.get(k)}
        OWNER.need(changes == a['reviewed_config_delta'], 'unreviewed full Kconfig delta')
        if args.phase != 'built-in':
            previous = Path(a['previous_phase_receipt'])
            regular(previous, a['previous_phase_receipt_sha256'])
            prior = json.loads(previous.read_text())
            expected = 'built-in' if args.phase == 'finish-with-native-host-btf' else 'finish-with-native-host-btf'
            OWNER.need(prior['passed'] and prior['phase'] == expected
                       and prior['workspace'] == str(workspace)
                       and prior['resolved_config_sha256'] == a['resolved_config_sha256'],
                       'prior phase evidence differs')

    parent = int(run(['systemctl', 'show', PARENT, '-p', 'MemoryMax', '--value']))
    OWNER.need(parent <= 32 * GiB, 'parent memory bound weakened')
    initial_usage = int(run(['du', '-sx', '-B1', str(workspace)]).split()[0])
    OWNER.need(initial_usage < WORKSPACE_CAP, 'workspace cap already exceeded')
    # Configuration creates only Kconfig host tools and text outputs. Reserve and
    # enforce its own bounded growth; compilation keeps the full workspace cap.
    phase_workspace_cap = min(WORKSPACE_CAP, initial_usage + 512 * 1024**2) if args.phase in ('configure', 'reconfigure') else WORKSPACE_CAP
    OWNER.need(available() >= HOST_RESERVE + memory + (phase_workspace_cap - initial_usage),
               'host lacks worst-case workspace+phase reserve')
    parent_used = int(run(['systemctl', 'show', PARENT, '-p', 'MemoryCurrent', '--value']))
    OWNER.need(parent - parent_used >= memory + phase_workspace_cap - initial_usage,
               'parent lacks phase+workspace margin')
    fs = os.statvfs(workspace)
    OWNER.need(fs.f_bavail * fs.f_frsize >= FLOOR + phase_workspace_cap - initial_usage,
               'filesystem lacks workspace reserve')
    if args.phase == 'configure':
        output.mkdir(mode=0o700)
        shutil.copyfile(config, output / '.config')
    temp = output / 'private-tmp'
    temp.mkdir(exist_ok=True, mode=0o700)
    OWNER.need(temp.resolve() == temp and temp.stat().st_uid == 0
               and not temp.stat().st_mode & 0o022, 'private temporary directory required')
    evidence = workspace / ('phase-' + args.phase)
    evidence.mkdir(mode=0o700)
    started = time.monotonic()
    result = {'phase': args.phase, 'workspace': str(workspace), 'steps': [], 'passed': False,
              'source_archive_sha256': OWNER.ARCHIVE_SHA, 'install_authorized': False,
              'workspace_cap_bytes': phase_workspace_cap}
    active_unit = None
    child = None
    handlers = {sig: signal.signal(sig, interrupted)
                for sig in [signal.SIGTERM, signal.SIGHUP, signal.SIGINT]}
    try:
        for index, command in enumerate(commands):
            active_unit = 'luma-selinux-kbuild-' + args.phase + '-' + str(os.getpid()) + '-' + str(index)
            log = evidence / ('step-' + str(index) + '.log')
            bounded = bounded_command(active_unit, cpu, memory, output, temp, command)
            with log.open('x') as stream:
                child = subprocess.Popen(bounded, stdout=stream, stderr=subprocess.STDOUT,
                                         start_new_session=True,
                                         env={'PATH': '/usr/bin:/bin', 'HOME': str(output),
                                              'TMPDIR': str(temp), 'LANG': 'C.UTF-8',
                                              'LC_ALL': 'C.UTF-8'})
                while child.poll() is None:
                    fs = os.statvfs(workspace)
                    OWNER.need(fs.f_bavail * fs.f_frsize >= FLOOR, 'filesystem floor reached')
                    OWNER.need(available() >= HOST_RESERVE, 'host RAM reserve reached')
                    OWNER.need(log.stat().st_size <= 64 * 1024**2, 'log bound exceeded')
                    OWNER.need(time.monotonic() - started < 4 * 3600, 'phase deadline')
                    usage = int(run(['du', '-sx', '-B1', str(workspace)]).split()[0])
                    OWNER.need(usage <= phase_workspace_cap, 'workspace allocation cap exceeded')
                    time.sleep(5)
            result['steps'].append({'command': command, 'exit': child.returncode})
            OWNER.need(child.returncode == 0, 'normal Kbuild command failed')
            child = None
            active_unit = None
        OWNER.validate_config((output / '.config').read_text())
        result['resolved_config_sha256'] = OWNER.digest(output / '.config')
        if args.phase not in ('configure', 'reconfigure'):
            OWNER.need(result['resolved_config_sha256'] == a['resolved_config_sha256'], 'config mutated')
        if args.phase == 'reconfigure':
            values = OWNER.config_values((output / '.config').read_text())
            for symbol in ('VIDEO_IMX896', 'VIDEO_OV13B10', 'VIDEO_S5KKD1SP', 'VIDEO_DW9714', 'VIDEO_DW9784'):
                OWNER.need(values.get('CONFIG_' + symbol) == 'm', 'required camera module: ' + symbol)
        result['passed'] = True
    except BaseException as exc:
        result['error'] = type(exc).__name__ + ': ' + str(exc)
        # Do not let a repeated termination interrupt cancellation or evidence.
        for sig in handlers:
            signal.signal(sig, signal.SIG_IGN)
        cancel_owned(active_unit, child, result)
        raise
    finally:
        result['elapsed_seconds'] = time.monotonic() - started
        with (evidence / 'actual.json').open('x') as f:
            json.dump(result, f, indent=2)
            f.write('\n')
        for sig, handler in handlers.items():
            signal.signal(sig, handler)


if __name__ == '__main__':
    main()
