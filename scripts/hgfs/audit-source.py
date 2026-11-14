#!/usr/bin/env python3
"""Check the committed source tree for local paths and accidental runtime assets."""
import io
import os
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[2]
archive = subprocess.check_output(['git', 'archive', 'HEAD'], cwd=ROOT)
private_paths = [str(Path.home()).encode(), str(ROOT.resolve()).encode()]
extra = os.environ.get('HGFS_PRIVATE_PATH')
if extra:
    private_paths.append(extra.encode())
email = subprocess.run(['git', 'config', '--global', '--get', 'user.email'],
                       capture_output=True, text=True)
if email.returncode == 0 and email.stdout.strip():
    private_paths.append(email.stdout.strip().encode())
failures = []
count = 0
with tarfile.open(fileobj=io.BytesIO(archive)) as source:
    for entry in source:
        if not entry.isfile():
            if entry.issym() and entry.linkname.startswith('/'):
                failures.append(entry.name + ': absolute symlink')
            continue
        count += 1
        path = Path(entry.name)
        if path.parts[0] in {'.cache', 'build', 'runtime', 'dist'}:
            failures.append(entry.name + ': local output included')
        content = source.extractfile(entry).read()
        if any(value and value in content for value in private_paths):
            failures.append(entry.name + ': private path or email')
        if path.suffix.lower() in {'.img', '.dmg', '.iso', '.wad', '.sav', '.dsg'}:
            failures.append(entry.name + ': disk/game/save asset')
        if path.parts[0] in {'contrib', 'tests', 'scripts'} and path.suffix.lower() in {'.exe', '.com', '.obj', '.o'}:
            failures.append(entry.name + ': generated executable/object')
if failures:
    raise SystemExit('\n'.join(failures))
print(f'Source archive audit passed ({count} files; no private paths, runtime images, games, or saves).')
