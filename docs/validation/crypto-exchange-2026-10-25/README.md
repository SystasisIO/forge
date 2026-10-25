# Bounded source exchange and grouped contract testing

Date: 2026-10-25. **PASS for the restricted local engineering scope only.**
Experimental crypto profile; no production enable, generic FairSwap circuit,
Labs/OpenAI qualification, economic acceptance or live blockchain claim.

## Scope

- `forge.crypto.exchange`: same physical declarations/implementation compiled
  separately for native and guest; existing SHA256/Keccak/Raw/policy errors.
  Up to 16 direct-source blocks, 2048 bytes each, one authenticated bounded fault.
- `forge.net.tlsnotary.exchange` in the existing native library binds verified
  source commitments, exact coverage and retained ciphertext. Callers must
  authenticate the whole verifier result, visible request/response semantics,
  independently expected context and current operation nonce before signing.
- Existing `forge.tooling.testing.host::transact` owns one ObjectDB/oracle
  transaction across explicitly driven real-WASM invocations. Failed children,
  preflight errors and nested groups poison it even if exceptions are caught.
  It does not route inline/notification effects or simulate chain authorities.
- Guest packaging registers the new archive and its hash/tamper coverage in
  the canonical SDK foundation manifest. Crypto core exposes only existing bytes
  values to guest code; native archives/BMIs are never imported by WASM.

Donors and rejected constructions: [traceability](../../donors/forge-crypto-exchange-blocks-v1.md).
No donor code, hash primitive, MPC engine, transport or chain runner is copied.

## Evidence

macOS arm64 / LLVM 22.1.8 / Release / Ninja / `-j 4`:

| Gate | Result |
| --- | --- |
| Independent Keccak vectors + native checker | 1/1 CTest PASS |
| Real paired MPC against local HTTPS, bind/nonce/substitution negatives | 1/1 PASS |
| SDK foundation/package-prefix and tampered exchange archive | 2/2 PASS |
| Real WASM SDK E2E, including six grouped-transaction cases | 1 CTest / 59 cases PASS |
| Downstream actual WASM/native exchange, ABI and goldens | 3/3 PASS |
| Actual downstream jobs→bill→token and ABI | 2/2 PASS, 27 groups |
| Downstream bill regression/signing/ABI/provenance/oracle | 6/6 PASS |

The 16 gates are scoped, not a complete Forge/Turbine test run. Fresh installed
guest SDK and official native build-tree consumers passed; a fresh full native
installed-package consumer was not run in this task. TLSNotary/MPZ pins and Rust
sources remain unchanged from the accepted commitments delivery.

Two independent downstream test reviews and source/runtime crypto, host, guest
and package reviews found no remaining blockers inside this scope. Raw logs,
selected source hashes, XML and review artifacts accompany this report.

## Corrections and limits

Fixed hostile nested allocation before manifest validation; downstream native
and guest tests distinguish the early allocation limit from EOF. Added the new
guest archive to foundation hashing and explicit tamper tests. Test drivers now
reject any unconsumed extra inline/notification/context-free/deferred effect.
The downstream consumer avoids a redundant full-tariff read while preserving
version/hash, price, asset, recipient and payout checks at their owning boundary.

The first SDK build needed the exact pinned cdt-musl submodule initialized.
The broad SDK consumer ALL build retained a donorcompat freestanding-sysroot
failure (`std::regex`/locale/time); scoped required fixtures passed. A command
with an incorrect fixture target was corrected without source/limit changes.
Target-only rebuilding left an old generated contract.json; byte comparison
detected it and the standard ALL artifact target regenerated it. Original
failures are retained; SDK ALL PASS is not claimed.

One direct raw HTTP body up to 32 KiB is supported. Async services, JSON/base64,
redirect/download chains, arbitrary files, GPU-model identity, WAN, production
privacy, finality, resource policy and the full cost of a $0.012 operation remain
unqualified. A fresh secret key per offer is mandatory; the helper cannot detect
caller key reuse. Public disputes reveal their block; a public key reveals all
ciphertext available to its holder. Retained evidence alone is not an offline
TLS proof or an authentication capability.

Native financial settlement, inclusion/finality and post-reveal payment liveness
remain consumer/chain responsibilities. The testhost's oracle/rows rollback
does not prove live Spine permissions, consensus or token custody. Internal
ObjectDB allocation seals are not misrepresented as protocol state.

Linux, real Labs, deployment, full native package qualification and CI were not
run. No production/commercial activation is authorized by this report.
