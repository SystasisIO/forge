# Wallet lifecycle and block-execution composition

This note records inspected donors and implementation boundaries. Current
delivery acceptance is tracked in
[the implementation report](../iterations/wallet-signer-implementation.md);
prior v5 evidence does not establish acceptance of the combined candidate.

## Baseline and donors

- Forge base: `9ccc98f276897e4be3f497f3800c2ff0f8235eea`.
- Spring `e6a99f68b67abc4d89fe716755b2e1394a4991f7`:
  `plugins/wallet_plugin/wallet_manager.cpp` (create/open/lock/unlock, timeout,
  directory ownership) and the matching public header. Accept separation of
  encrypted wallet persistence from its unlocked lifetime. Reject the donor's
  racy lock-file creation/removal and polling watcher, public digest signing,
  private-key export, server-generated password and implicit unlocked creation.
- Forge `libraries/crypto/keystore`: reuse encrypted container, bounded scrypt,
  AES-GCM, atomic replacement, permission checks and typed durability errors.
  Preserve bytes and algorithms. Ownership is a persistent sibling lock held
  independently from an open decrypted store, shared by daemon and one-shot CLI.
- Forge `libraries/asio/README.md`, task scheduler and compute executor: the
  scheduler owns delayed expiry admission/lifetime; the existing application-owned
  compute pool runs blocking filesystem/KDF work. Never nest compute admission
  behind expiry tasks waiting for the same wallet gate, block an I/O worker or
  create a private wallet runtime. Drain admitted work before releasing the gate.
  Retain immediate release of canceled queued capacity before completion
  notification, required by idle-timer replacement. Retain notification ownership
  through async initiation/mutex unlock when another worker completes early;
  notification/affine regressions exercise this shared primitive. Wallet uses
  compute/gate operations, not a private affine lane.
- Forge Chain Signer: retain the single registration, exact policy, all-key
  identity preflight, bounded admission, cancellation and signature verification.
  The new optional execution handler belongs inside that boundary, not in a
  second signer registry or a proxy that bypasses policy.
- CometBFT `2a9c48af3b601cd729f2956fb76af5dc4e107217`, `privval/file.go`:
  bounded durable last-sign state is a **downstream product requirement**.
  Forge gains no database, last-slot store or consensus-specific H/R/S model.

## Required evidence

- Ownership exclusion across processes and atomic file replacements; retain
  ownership across decrypted-store destruction and explicit reopen; reject
  inherited process handles, unsafe lock files and lock-file replacement.
- Wallet create/open/restart locked; failed passwords; bounded resource use;
  successful-use-only idle refresh; sign/lock/import/remove serialization;
  uncertain persistence enters fault state.
- mTLS-only management with exact client/wallet/operation permissions and
  sensitive request redaction independent from configuration redaction.
- Execution handler never receives a denied request or a mismatched provider
  identity. Cached results receive fresh checks. Expired continuations cannot
  hold admission or initiate cryptography after the request has completed.

No donor repository is a build dependency.

Pending P2P connection-gate shutdown experiments and their tests are excluded
from this delivery; the original teardown-test race and intermittent downstream
shutdown remain open. See the
[libp2p handoff](../iterations/p2p-shutdown-sync-handoff.md). The retained Asio
corrections are independent Wallet/runtime requirements, not incident closure.
