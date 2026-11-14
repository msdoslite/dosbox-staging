#!/usr/bin/env python3
"""Create a local source-only snapshot. This does not publish or upload anything."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
if subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT).strip():
    raise SystemExit('Commit the intended source changes before creating an archive.')
subprocess.run([sys.executable, str(ROOT / 'scripts/hgfs/audit-source.py')], check=True)
output = ROOT / 'dist'
output.mkdir(exist_ok=True)
revision = subprocess.check_output(['git', 'rev-parse', '--short', 'HEAD'], cwd=ROOT, text=True).strip()
archive = output / f'dosbox-staging-hgfs-source-{revision}.tar.gz'
subprocess.run(['git', 'archive', '--format=tar.gz', '--prefix=dosbox-staging-hgfs/',
                '-o', str(archive), 'HEAD'], cwd=ROOT, check=True)
print('Created ' + str(archive.relative_to(ROOT)))
