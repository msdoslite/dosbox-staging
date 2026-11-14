#!/usr/bin/env python3
"""Exercise eight shared drives in a disposable copy of a user-supplied DOS image."""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


class Guest:
    def __init__(self, image, offset):
        self.work = ROOT / 'build/hgfs-guest'
        self.work.mkdir(parents=True, exist_ok=True)
        self.image = self.work / 'guest.img'
        if image.resolve() == self.image.resolve():
            raise SystemExit('The input image must not be the disposable test image.')
        shutil.copyfile(image, self.image)
        self.disk_spec = str(self.image) + '@@' + str(offset)
        self.process = None
        self.config = self.work / 'test.conf'

    def disk(self, command, *args, check=True):
        return subprocess.run([command, '-i', self.disk_spec, *map(str, args)],
                              check=check, capture_output=True)

    def install(self, source, target):
        self.disk('mcopy', '-o', source, '::' + target)

    def read(self, name):
        return self.disk('mtype', '::' + name).stdout

    def run(self, commands, timeout=120):
        for name in ['RESULT.TXT', 'STATUS.TXT']:
            self.disk('mdel', '::' + name, check=False)
        batch = self.work / 'AUTOEXEC.BAT'
        batch.write_bytes(('@echo off\n' + commands + '\necho DONE> C:\\RESULT.TXT\n')
                          .replace('\n', '\r\n').encode('ascii'))
        self.install(batch, 'AUTOEXEC.BAT')
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
                   DYLD_LIBRARY_PATH=str(ROOT / 'build/deps/lib'))
        with (self.work / 'host.log').open('w') as log:
            self.process = subprocess.Popen(
                [str(ROOT / 'build/dosbox'), '--noprimaryconf', '--nolocalconf',
                 '--conf', str(self.config)], env=env, cwd=ROOT,
                stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    if self.disk('mtype', '::RESULT.TXT', check=False).returncode == 0:
                        return
                    if self.process.poll() is not None:
                        raise RuntimeError('Guest exited before completing the tests.')
                    time.sleep(0.1)
                raise RuntimeError('Guest tests timed out; inspect build/hgfs-guest/host.log')
            finally:
                try:
                    os.killpg(self.process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                self.process.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--offset', type=int, default=32256)
    args = parser.parse_args()
    for command in ['mcopy', 'mtype', 'mdir', 'mdel', 'nasm']:
        if not shutil.which(command):
            raise SystemExit('Missing test prerequisite: ' + command)
    guest = Guest(args.image, args.offset)
    # This test supports the documented FreeDOS FAT16 test layout only.
    # Replace configuration in the copy, never in the caller's source image.
    config_sys = guest.work / 'FDCONFIG.SYS'
    config_sys.write_bytes(b'LASTDRIVE=Z\r\nFILES=60\r\nBUFFERS=20\r\nSHELL=C:\\COMMAND.COM /P=C:\\AUTOEXEC.BAT\r\n')
    guest.install(config_sys, 'FDCONFIG.SYS')
    guest.disk('mmd', '::VMSMOUNT', check=False)
    guest.install(ROOT / 'build/vmsmount/vmsmount.exe', 'VMSMOUNT/VMSMOUNT.EXE')
    for source, target in [('multi-handles.asm', 'HANDLES.COM'), ('filetest.asm', 'FILETEST.COM')]:
        text = (ROOT / 'tests/hgfs' / source).read_text().replace('D:\\SHARED\\', 'D:\\')
        assembly = guest.work / source
        assembly.write_text(text)
        subprocess.run(['nasm', '-f', 'bin', str(assembly), '-o', str(guest.work / target)], check=True)
        guest.install(guest.work / target, target)
    folders = []
    configuration = '[hgfs]\ntrace = true\n'
    for index in range(8):
        folder = guest.work / ('share' + str(index + 1))
        if folder.exists():
            shutil.rmtree(folder)
        folder.mkdir()
        (folder / 'HELLO.TXT').write_text(('games' if index == 0 else 'work' if index == 1 else str(index)) + '\r\n')
        folders.append(folder)
        configuration += (f'share{index+1}_name = share{index+1}\n'
                          f'share{index+1}_root = {folder}\n'
                          f'share{index+1}_write = {str(index != 7).lower()}\n')
    configuration += f'''[sdl]
output = texture
[cpu]
core = normal
cpu_cycles = 30000
[dos]
shell_history_file =
[autoexec]
imgmount c "{guest.image}" -t hdd -fs fat -size 512,63,16,128
boot c:
'''
    guest.config.write_text(configuration)
    lines = []
    for index, drive in enumerate('DEFGHIJK', 1):
        lines.append(f'C:\\VMSMOUNT\\VMSMOUNT /L:{drive} /SHARE:share{index}')
        lines.append(f'type {drive}:\\HELLO.TXT > C:\\{drive}.LOG')
    lines += [r'C:\FILETEST.COM > C:\FILETEST.LOG', 'if errorlevel 1 goto failed',
              r'C:\HANDLES.COM > C:\HANDLES.LOG', 'if errorlevel 1 goto failed',
              r'C:\VMSMOUNT\VMSMOUNT /L:D /SHARE:share1',
              r'C:\VMSMOUNT\VMSMOUNT /L:E /SHARE:share3', 'if not errorlevel 1 goto failed',
              r'C:\VMSMOUNT\VMSMOUNT /L:C /SHARE:share1', 'if not errorlevel 1 goto failed',
              r'copy D:\HELLO.TXT E:\FROMD.TXT', r'copy E:\HELLO.TXT D:\FROME.TXT',
              r'echo DENIED> K:\DENIED.TXT']
    for _ in range(40):
        lines += [r'C:\VMSMOUNT\VMSMOUNT /L:L /SHARE:missing > NUL', 'if not errorlevel 1 goto failed']
    for _ in range(64):
        lines += [r'C:\VMSMOUNT\VMSMOUNT /U /L:D > NUL',
                  r'C:\VMSMOUNT\VMSMOUNT /L:D /SHARE:share1 > NUL', 'if errorlevel 1 goto failed']
    for drive in 'DFHJEGIK':
        lines.append(f'C:\\VMSMOUNT\\VMSMOUNT /U /L:{drive}')
    lines += ['echo PASS> C:\\STATUS.TXT', 'goto done', ':failed',
              'echo FAIL> C:\\STATUS.TXT', ':done']
    guest.run('\n'.join(lines))
    assert guest.read('STATUS.TXT').strip() == b'PASS'
    assert b'FILETEST PASS' in guest.read('FILETEST.LOG')
    assert b'PASS:' in guest.read('HANDLES.LOG')
    for index, drive in enumerate('DEFGHIJK'):
        assert guest.read(drive + '.LOG').strip() == (folders[index] / 'HELLO.TXT').read_bytes().strip()
    assert (folders[1] / 'FROMD.TXT').read_bytes() == (folders[0] / 'HELLO.TXT').read_bytes()
    assert (folders[0] / 'FROME.TXT').read_bytes() == (folders[1] / 'HELLO.TXT').read_bytes()
    assert not (folders[7] / 'DENIED.TXT').exists()
    report = {'result': 'passed', 'checks': ['eight simultaneous drives', 'AH=5Bh preserves existing data and returns error 80',
              'fresh AH=5Bh create', 'extended create collision', 'file APIs', 'busy detach',
              'inherited/interleaved handles', 'occupied C/E rejected', 'read-only K',
              'cross-drive copies', '40 invalid shares', '64 remounts', 'non-LIFO detach']}
    (guest.work / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Guest checks passed: eight drives, create-new, file APIs, copies, permissions, and lifecycle.')


if __name__ == '__main__':
    main()
