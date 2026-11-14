#!/usr/bin/env python3
"""Run host-side HGFS tests with address/undefined-behavior sanitizers."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / 'build/hgfs-tests'
BUILD.mkdir(parents=True, exist_ok=True)
TESTS = ROOT / 'tests/hgfs'
BACKEND = ROOT / 'src/hardware/vmware_hgfs.cpp'
for name, sources in [
    ('protocol', ['hgfs_test.cpp', 'hgfs_boundary_test.cpp', 'hgfs_lifecycle_test.cpp']),
    ('multi', ['hgfs_multi_test.cpp']),
]:
    binary = BUILD / name
    subprocess.run(['clang++', '-std=c++17', '-g', '-fsanitize=address,undefined',
                    *[str(TESTS / source) for source in sources], str(BACKEND),
                    '-o', str(binary)], check=True)
    with tempfile.TemporaryDirectory(prefix='fixture-', dir=BUILD) as fixture:
        subprocess.run([str(binary), str(Path(fixture) / 'share')], check=True)
