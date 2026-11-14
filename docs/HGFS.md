# Shared-folder drives for a booted DOS guest

This experimental, unofficial fork adds file-level host folders to DOSBox Staging
while a real DOS kernel runs from an image. VMware is not required. The included
modified VMSMOUNT driver is required for named shares and multiple letters.

## Configuration

The generated default configuration includes a documented `[hgfs]` section.
Nothing is exported by default; each share is read-only unless explicitly enabled.

```ini
[hgfs]
share1_name = games
share1_root = /path/to/games
share1_write = true

share2_name = work
share2_root = /path/to/work
share2_write = true

share3_name = reference
share3_root = /path/to/reference
share3_write = false
```

Use existing absolute paths without quotes, including when paths contain spaces.
Do not use shell variables or `~`. Names are case-insensitive and contain 1–16
ASCII letters, digits, underscores, or hyphens. There are eight named slots.
Their numbers do not determine DOS drive letters. Invalid or unavailable shares
are skipped with a warning; valid shares remain usable.

In the guest, copy `build/vmsmount/vmsmount.exe` to a directory in PATH, then run:

```dos
VMSMOUNT /L:D /SHARE:games
VMSMOUNT /L:E /SHARE:work
VMSMOUNT /L:F /SHARE:reference
DIR D:\
COPY D:\EXAMPLE.TXT E:\EXAMPLE.TXT
```

Use free letters from D: through Z:, within the guest's LASTDRIVE setting. Add the
commands to the guest AUTOEXEC.BAT for automatic mounting. One resident driver
serves all letters; common options such as `/B` and `/LFN` belong to the first load.
Contents appear directly at the drive root, without a SHARED subdirectory.

To remove only E:, close its files, change to C:, and run `VMSMOUNT /U /L:E`.
Other letters keep working, including their open files. Busy drives cannot be
removed. Removing the final drive unloads the TSR if its interrupt handlers can
be safely unhooked. With several drives, `/U` without `/L` is rejected.

Fully restart the emulator after editing `[hgfs]`. Reloading VMSMOUNT does not
reload the host configuration. The older prototype's `[dosbox] hgfs*` settings
are not used in this fork; migrate them to `[hgfs]`.

The optional `root`, `write`, and `flat` settings provide an unnamed share for
VMSMOUNT without `/SHARE`. They do not alter named-share permissions. `trace`
enables host protocol diagnostics. Leave it disabled for normal use.

## Boot image

Use your own legally obtained DOS image with Staging's normal IMGMOUNT and BOOT
commands. No DOS image, game, save, or personal host folder is distributed here.
Image geometry depends on the image; do not reuse a geometry blindly. Never modify
an image with external tools while the emulator is using it.

These drives implement DOS file operations, not FAT sectors. Disk editors,
FORMAT, and FDISK do not operate on them as ordinary image-backed disks.

## Compatibility and limits

Validated on macOS arm64 with FreeDOS. Other guest kernels and platforms are not
claimed as tested. The host backend is compiled on macOS/Linux and disabled on
unsupported platforms; the supplied bootstrap is macOS-only.

- Reads, writes, truncation, attributes, timestamps, directories, copies, and
  independent mount lifecycles are tested. Legacy AH=5Bh create-new is fixed:
  existing files are preserved and DOS error 80 is returned.
- Full byte-range locking/sharing-mode semantics and comprehensive Unicode/LFN
  compatibility remain outside the tested feature set.
- Paths, symbolic links, multiply-linked files, malformed packets, and foreign
  or stale handles are checked. This is not a sandbox for the whole emulator.
- Deliberate host-side moves between a path check and a filesystem operation are
  outside the threat model. Do not move files while a guest is saving them.
- Temporary permission revocation denies access. Restoring permissions to the
  same directory restores access. Deleting/recreating a root requires a restart;
  the server does not silently switch to a replacement inode.

## Build from a clean clone

Prerequisites: macOS, Xcode Command Line Tools, Python 3, and Homebrew packages:

```sh
brew install cmake pkg-config sdl3 sdl3_image speexdsp asio fluidsynth libpng opusfile nasm mtools
python3 scripts/hgfs/build.py --jobs 4
```

The script downloads checksum-pinned IIR sources and an archived Open Watcom
release, builds IIR locally, builds Staging, and compiles the vendored DOS driver.
Outputs are `build/dosbox` and `build/vmsmount/vmsmount.exe`. It does not require
any files from the original development workspace and does not modify a DOS image.
Downloads are cached in `.cache`; use `--cache DIR` for an existing download cache.
A checksum mismatch stops the build. Homebrew prerequisites are not vendored or
bit-for-bit pinned; this is a reproducible procedure, not a reproducible-binary claim.

Upstream builds remain available through the original platform instructions.
The convenience build uses system libraries and disables MT-32 and upstream tests.

## Tests

See [HGFS-VALIDATION.md](HGFS-VALIDATION.md) for the local preparation results.

```sh
python3 scripts/hgfs/test-protocol.py
python3 scripts/hgfs/test-defaults.py
python3 scripts/hgfs/test-guest.py --image /path/to/freedos.img --offset 32256
```

Protocol tests use ASan/UBSan, disposable folders, boundary/malformed-request tests,
permission/root lifecycle tests, and session isolation checks. They do not need DOS.
The default-settings test exercises ordinary built-in DOS folder mounting with
HGFS disabled and checks the generated configuration documentation.

The guest harness currently expects a FreeDOS FAT16 HDD image with 512-byte sectors,
63 sectors/track, 16 heads, 128 cylinders, partition offset 32256, and COMMAND.COM
in the C: root. It replaces boot scripts **only in a copy** under build/hgfs-guest.
It checks all eight letters D:–K:, AH=5Bh, file APIs, busy and non-LIFO unmount,
inherited/open handles, occupied letters, read-only access, copies, failed share
selection, and repeated remounts. Never pass its output image back as input.
Local logs and fixture images stay under the ignored build directory.

## Distribution and attribution

This is an experimental source fork, not an official DOSBox Staging release.
No maintenance or compatibility commitment is implied. Preserve upstream license
and copyright notices. The modified VMSMOUNT source and its license are included
in contrib/vmsmount; see its PATCHES.md for provenance and changes.

The source release excludes build caches, guest images, games, local configs,
logs, and downloaded toolchains. A portable macOS app is not provided by this
source-only preparation; local executables may depend on Homebrew libraries.
Before distributing binaries, package their dependencies and corresponding
sources/notices, and test outside the build environment.

Implementation, tests, documentation, and refactoring were developed with
assistance from OpenAI's Codex (ChatGPT). This disclosure does not imply endorsement
by OpenAI or the upstream DOSBox Staging/VMSMOUNT projects.

For a local source-only snapshot, commit the intended files and run:

```sh
python3 scripts/hgfs/audit-source.py
python3 scripts/hgfs/archive-source.py
```

The archive is produced from the Git commit, not by copying the working directory.
The audit checks that local home/workspace paths, configured personal email,
disk images, games, saves, caches, and generated guest executables are absent.
Upstream public author notices, translations, and generic path examples are retained.
The scripts do not create a remote repository, push, upload, or publish releases.
