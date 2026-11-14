# Local VMSMOUNT changes

Upstream: https://github.com/eduardocasino/vmsmount

Base commit: c9a068c569778a32a5e929940882e30a0d52519d

License: GPL-2.0-or-later; retain the original headers and license.txt.

This source copy preserves upstream files except for the changes below:

- main.c, redir.h, redir.c: one resident driver with per-letter mappings,
  session selection, targeted unload, and refusal to unload open files.
- vmaux.c, vmaux.h: an opt-in Staging share-selection RPC and channel cleanup.
- redir.c: honor create-new bit 8 in INT 2F/1117 (DOS AH=5Bh); return error 80
  for an existing file without modifying it. Keep normal create/truncate behavior.
- redir.c: distinguish the partial SFT declaration from the full DOS entry stride,
  and preserve inherited handles during child process termination.
- messages.h: mark the modified version and document the added command options.

The rest of the source remains upstream code. Build scripts use a copied tree
under build/vmsmount, so generated objects and executables do not enter this tree.

References for the DOS interfaces:
https://raw.githubusercontent.com/FDOS/kernel/master/kernel/dosfns.c
https://raw.githubusercontent.com/FDOS/kernel/master/hdr/sft.h
https://fd.lod.bz/rbil/interrup/network/2f1117.html
