# Swarm File Storage Donor Mapping

Scope: PR2 ordinary-file storage, restart checkpoints, ownership and atomic
publication. See [Core mapping](forge-swarm-core-v1.md) and the
[plan](../iterations/forge-content-swarm-v1.md). Donors are reference sources,
not production build dependencies.

## Reviewed Sources

- libtorrent `b4e9e2471e7abce2a58585a7d7fbc4e8d1edc47e`:
  `src/file_storage.cpp`, `src/part_file.cpp`, `src/storage_utils.cpp`,
  `src/torrent.cpp`: mapping, partial state, resume and piece completion.
- Syncthing `1919c89de488d8e1088579f5b11f28aa375c5a8e`:
  `lib/model/folder_sendrecv.go`, `lib/model/sharedpullerstate.go`,
  `lib/osutil/atomic.go`, `lib/osutil/traversessymlink.go`,
  `lib/fs/basicfs.go`, `lib/osutil/osutil.go`: temporary reuse, write/sync/close
  ownership, replacement and path traversal.
- Forge Asio compute/gate/notification/affine shutdown; existing native patterns
  in `libraries/crypto/keystore/persistence.cpp` and `libraries/net/http/file.cpp`.

## Accepted And Rejected Patterns

| Mechanism | Forge decision |
| --- | --- |
| File-local mapping and positional writes | Independent files/pieces and sparse ordinary files; no piece spans files. |
| Unfinished vs complete piece | Separate bounded block progress and verified availability; authenticated hash check before commit. |
| Resume | Rehash actual bytes, including unchanged size/mtime. Reject presence-only seed confidence. |
| Syncthing temporary reuse | Recover existing bytes only after verification; no unchecked skip-hash path. |
| Temporary -> sync -> rename -> directory sync | Adopt with checked errors, not ignored donor sync failures. |
| RenameOrCopy | Reject overwrite/copy-delete fallback; publish one root with atomic no-replace rename. |
| Lstat then open | Insufficient alone. Use pinned directory descriptors/O_NOFOLLOW with opened inode/type/device/size checks. |
| Execution | Caller-owned separate Forge compute disk/CPU executors, no donor thread pool. |
| Wait/drain | Forge gate/notification; generic stop-token gate overload, no second Swarm waiter. |

## Ownership And Durability

Core defines filesystem-neutral store/checkpoint/writer/lease contracts.
Storage owns the native layout and bounded active state. Writer generations
and I/O reservation identities are separate: retiring an old guard cannot
release a replacement writer. No state mutex spans I/O or suspension.

Checkpoint order is verified payload -> file/internal-directory sync -> durable
staging-parent name -> atomic metadata commit. Sidecar replacement syncs the
parent; external mode explicitly syncs it before the selected adapter commits.
There is exactly one authority. Versioned canonical Raw binds identity and
generation with a corruption checksum; it is not authentication or payload proof.

Final publication atomically renames one staging root without replacement,
syncs the parent and saves a published checkpoint. Failure after rename retains
its actual location, not a fictional rollback or deletion. Live availability
and acknowledged durable generation remain distinct.

## Evidence Boundary

Tests exercise real file output, partial resume, corrupt/replaced bytes,
checkpoint failures and ownership. Test executable interposition injects short
I/O, syscall errors and bounded pauses; child processes exit before/after sync
barriers. No test bypass enters production code. Process crashes do not prove
physical power-loss behavior, storage firmware or hardware fsync honesty.

The destination parent and process credentials are trusted local administration
boundaries. Path checks do not sandbox an administrator who directly rewrites
or reparents held inodes. No network or production-readiness claim follows.

Installed-package acceptance imports `swarm_storage` independently. Focused
results and exact-head reviews are captured separately in the acceptance receipt.
