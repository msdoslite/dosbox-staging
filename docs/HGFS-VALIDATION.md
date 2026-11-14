# HGFS source-fork validation

Validation date: 2026-11-14.

Scope: local source-only preparation; nothing was pushed or published.
Platform: macOS arm64. Guest: FreeDOS 1.4 in a disposable image copy.
Upstream Staging base: 27b1b7ca719c21ef3444f7ad1cf2d9cd439527cc.
VMSMOUNT base: c9a068c569778a32a5e929940882e30a0d52519d.
Implementation revision tested: 16a8244 (subsequent report-only commits do not change code).

## Build

A separate clean local clone built Staging and VMSMOUNT successfully using
scripts/hgfs/build.py. No objects, headers, libraries, executables, or DOS images
from the development workspace were used as build inputs. Only the two downloaded
archives were reused through the explicit cache option; their SHA-256 checks
passed. IIR and Open Watcom sources/releases are pinned in dependencies.json.
Homebrew prerequisites and the platform compiler were installed beforehand.
This validates clean-checkout buildability, not byte-identical builds on all hosts.

## Checks passed

- AddressSanitizer/UBSan host tests: file operations, error paths, 460 denied
  boundary requests, 20,000 malformed packets, permission and root lifecycle,
  isolated sessions, foreign/stale handles, and independent loss of a root.
- All eight named drives D: through K: simultaneously.
- Legacy AH=5Bh creates a missing file; collision returns DOS error 80 and
  preserves existing contents. Ordinary AH=3Ch still truncates an existing file.
- File API tests: reads, writes, seek/truncate, EOF, attributes, timestamps,
  extended create collision, rename, directories, and nonempty-directory errors.
- Interleaved/inherited open handles, busy-unmount refusal, and an open E: file
  surviving D: removal. A child process cannot close its parent's live host handle.
- Occupied C:/E: rejected; read-only K: rejects writes; copies between D: and E:.
- Forty unknown-share failures, 64 remount cycles, and non-LIFO detach.
- HGFS disabled by default: ordinary built-in DOS MOUNT and file copying work.
- Generated configuration includes the documented [hgfs] section and eight slots.
- The final guest test was repeated after formatting only the newly added driver
  mount-management functions. Unrelated upstream code was not reformatted.

## Source/archive checks

The source archive is generated from Git, not from the working directory.
The audit checks local home/workspace paths and personal Git email, absolute
symlinks, disk images, games, saves, build/cache/runtime directories, and generated
DOS executables. Those private/runtime assets are not part of the source snapshot.
Original public upstream notices, author credits, translations, and generic path
examples remain intact. Imported vendor whitespace is intentionally preserved.

The new documentation/comments are English and disclose Codex/ChatGPT assistance.
The .gitignore comes from Toptal gitignore.io with OS/editor/build templates and
small project exclusions. Checked-in VMSMOUNT makefiles are explicitly retained.

## Not claimed

MS-DOS 6.22, other host platforms, comprehensive Unicode/LFN compatibility, full
sharing/byte-range lock semantics, and isolation from a malicious host process
moving open files remain unvalidated or outside scope. No portable macOS binary
release, signing/notarization, remote fork, or release publication was performed.
