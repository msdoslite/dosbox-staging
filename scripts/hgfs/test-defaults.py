#!/usr/bin/env python3
"""Check documented HGFS defaults and ordinary MOUNT without enabling HGFS."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / 'build'
with tempfile.TemporaryDirectory(prefix='defaults-', dir=BUILD) as directory:
    work = Path(directory)
    share = work / 'share'
    share.mkdir()
    (share / 'HELLO.TXT').write_bytes(b'ordinary built-in DOS mount\r\n')
    config = work / 'test.conf'
    config.write_text('[sdl]\noutput=texture\n[dos]\nshell_history_file=\n')
    output = work / 'default.conf'
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
               DYLD_LIBRARY_PATH=str(BUILD / 'deps/lib'))
    result = subprocess.run([str(BUILD / 'dosbox'), '--noprimaryconf', '--nolocalconf',
                             '--conf', str(config), '-c', f'config -writeconf "{output}"',
                             '-c', f'mount d "{share}"', '-c', 'copy d:\\HELLO.TXT d:\\COPY.TXT',
                             '-c', 'exit'], env=env, cwd=ROOT, capture_output=True, timeout=30)
    assert result.returncode == 0, result.stderr.decode(errors='replace')
    assert (share / 'COPY.TXT').read_bytes() == (share / 'HELLO.TXT').read_bytes()
    assert b'HGFS: SHARED folder enabled' not in result.stdout + result.stderr
    text = output.read_text()
    section = text.split('[hgfs]', 1)[1].split('\n[', 1)[0]
    assert 'VMSMOUNT /L:D /SHARE:games' in section
    assert 'share8_write' in section
    assert 'read-only' in section
    (BUILD / 'hgfs-default-section.conf').write_text('[hgfs]' + section + '\n')
print('Default HGFS section and ordinary MOUNT with HGFS disabled passed.')
