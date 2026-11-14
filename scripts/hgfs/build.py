#!/usr/bin/env python3
"""Build the macOS experimental fork and DOS driver from a clean checkout."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def run(*args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def download(item, cache):
    archive = cache / item['file']
    if not archive.exists():
        temporary = archive.with_suffix('.download')
        run('curl', '--fail', '--location', '--retry', '3', item['url'], '-o', temporary)
        temporary.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != item['sha256']:
        raise SystemExit(f'Checksum mismatch: {archive.name}; remove it and retry')
    return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, default=ROOT / '.cache')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if platform.system() != 'Darwin':
        raise SystemExit('This bootstrap script currently supports macOS only.')
    for tool in ['brew', 'cmake', 'clang++', 'curl', 'tar']:
        if not shutil.which(tool):
            raise SystemExit(f'Missing build prerequisite: {tool}')
    cache = args.cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((ROOT / 'scripts/hgfs/dependencies.json').read_text())
    archives = {name: download(item, cache) for name, item in manifest.items()}
    build = ROOT / 'build'
    sources = build / 'dependency-sources'
    sources.mkdir(parents=True, exist_ok=True)
    iir = sources / 'iir'
    if not iir.exists():
        iir.mkdir()
        run('tar', '-xf', archives['iir'], '-C', iir, '--strip-components=1')
    prefix = build / 'deps'
    run('cmake', '-S', iir, '-B', build / 'iir', '-DIIR1_BUILD_TESTING=OFF',
        '-DIIR1_BUILD_DEMO=OFF', f'-DCMAKE_INSTALL_PREFIX={prefix}')
    run('cmake', '--build', build / 'iir', '-j', args.jobs)
    run('cmake', '--install', build / 'iir')
    brew = subprocess.check_output(['brew', '--prefix'], text=True).strip()
    run('cmake', '-S', ROOT, '-B', build, '-DCMAKE_BUILD_TYPE=Release',
        '-DOPT_MT32EMU=OFF', '-DOPT_TESTS=OFF', '-DUSE_SYSTEM_LIBS=ON',
        f'-DCMAKE_PREFIX_PATH={prefix};{brew}',
        f'-DCMAKE_CXX_FLAGS=-ffile-prefix-map={ROOT}=.',
        '-DCMAKE_EXE_LINKER_FLAGS=-framework AudioToolbox -framework AudioUnit -framework Cocoa')
    env = dict(os.environ, CPLUS_INCLUDE_PATH=str(prefix / 'include'))
    run('cmake', '--build', build, '-j', args.jobs, env=env)
    watcom = sources / 'watcom'
    host = 'armo64' if platform.machine() == 'arm64' else 'bino64'
    if not (watcom / host / 'wcc').exists():
        watcom.mkdir(exist_ok=True)
        run('tar', '-xf', archives['watcom'], '-C', watcom,
            './' + host, './h', './lib286', './binw', './license.txt')
    driver = build / 'vmsmount'
    shutil.copytree(ROOT / 'contrib/vmsmount', driver, dirs_exist_ok=True)
    env = dict(os.environ, WATCOM=str(watcom), INCLUDE=str(watcom / 'h'),
               PATH=str(watcom / host) + os.pathsep + os.environ['PATH'])
    run('wmake', '-h', 'vmsmount.exe', 'UPX=/usr/bin/true', cwd=driver, env=env)
    print('Built build/dosbox and build/vmsmount/vmsmount.exe')


if __name__ == '__main__':
    main()
