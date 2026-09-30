# forge.crypto.keystore

Encrypted local-file signing provider for one-shot clients. The file container
uses bounded scrypt and AES-256-GCM with authenticated metadata. Writes are
atomic and use private owner-only filesystem permissions.

Every `store::create(path, ...)` and `store::open(path, ...)` now acquires
exclusive process ownership. A second independent opener receives typed
`exceptions::in_use`, including another opener in the same process. Ownership
uses a persistent `<keystore>.lock` sibling: atomic replacement of the encrypted
file cannot release it. Never delete, replace, restore or move that lock file
while an owner is running. This is local filesystem ownership, not distributed
fencing or a defense against another process with the owner's OS privileges.

`forge.crypto.keystore.ownership` lets a wallet service retain ownership while
destroying the decrypted store for a lock operation:

```cpp
import forge.crypto.keystore.store;

auto owner = forge::crypto::keystore::ownership::acquire(wallet_path);
{
   auto unlocked = forge::crypto::keystore::store::open(owner, password);
   // Use the provider; destruction releases plaintext, not process ownership.
}
// The next explicit unlock reuses owner. No new path-based opener can take it.
```

The parent directory must already exist and be private both for direct ownership
and path-based `store::open`; path-based `store::create` retains its existing directory creation
behavior. `store::owner()` can retain the owner of a newly created store. A
capability admits only one decrypted store at a time (`ownership::claim()` is
the scoped store-access primitive); a failed decrypt releases that access claim.
Forked processes must not use inherited stores. Each read/sign/write checks
that the ownership handle still belongs to this process and the original lock
inode; loss fails closed as `ownership_lost`.

Migration: older versions allowed opening a private file in a non-private parent
directory. That is intentionally rejected now: stop existing clients, move the
encrypted file into an owner-only `0700` directory and update the configured
path. Preserve the file bytes; no wallet re-encryption is required. Never perform
this move under an active owner. This change does not move files automatically.

The store has no runtime. `keys_sync`, `describe_sync` and `sign_digest_sync`
expose the same operations for caller-owned bounded blocking pools; the async
provider methods delegate to them. The wallet plugin offloads them and all
keystore creation/decryption/writes through the existing Forge scheduler.

Every encryption generates its salt and GCM nonce internally; callers cannot
override either value. A post-publication directory-sync failure raises the
typed `durability_unknown` exception. The live store already reflects the
published file in that case, but the caller must treat its survival across an
immediate system crash as unknown.

The container follows security patterns from ERC-2335 and age, but it is a
Forge format and does not copy either project's chain-specific representation.

`forge.crypto.keystore.password` is the only supported boundary for CLI
secrets. It reads a bounded password from a hidden interactive terminal,
standard input or an owner-only regular file. Callers pass only a password-file
path or the decision to consume stdin through argv; the password itself is
never a command argument or an ordinary config option.

Decrypt validates the complete container shape before running scrypt or AES:
the format has a 16-byte salt, a 12-byte GCM nonce, a 16-byte GCM tag and an
exact ciphertext length. Scrypt `N` must be a power of two and all KDF values
must stay inside caller-provided limits. Malformed shape and KDF values fail as
typed `exceptions::invalid_file`; valid wire bytes are unchanged.
