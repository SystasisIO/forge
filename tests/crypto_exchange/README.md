# Bounded exchange verification

`exchange_tests.cpp` exercises the actual `forge.crypto.exchange` leaf. It
separates malformed witnesses (`invalid_input`) from authenticated bad source,
padding or ciphertext length (`invalid_delivery`). The suite includes sixteen
ordered blocks, partial final blocks, counter separation and explicit nested
Raw allocation bounds.

`vectors.json` is a fixed independent oracle. `generate_vectors.py` was run once
with PyCryptodome **3.23.0**, using its `Crypto.Hash.keccak` and Python `hashlib`.
It checks the standard empty Keccak vector before calculating
`Keccak256(BE256(counter) || key32)` for counters 0, 64, 65 and 1039. Complete
2080-byte ciphertext digests cover blocks 0, 1 and 15. Native tests contain the
fixed expected values; ordinary tests never regenerate or bless them.

The companion real-WASM probes live with the downstream integration fixture,
which compiles the same Forge physical sources through the Contract SDK.
The primitive does not authenticate a TLS source, enforce key freshness or
implement a generic fair-exchange protocol. The separate
`tests/tlsnotary/exchange_tests.cpp` consumes a real local MPC result, checks
fixed authenticated request/header bytes without disclosing the credential,
and calls the production `bind_exchange` before simulated buyer authorization.

These tests require explicit execution and retained logs. Their presence is
not a runtime PASS, commercial acceptance, live-chain rollback or Labs proof.
