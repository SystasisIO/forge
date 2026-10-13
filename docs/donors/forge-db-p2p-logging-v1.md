# Operational Logging Donor Traceability

This note records inspected patterns, not a new wire-compatibility claim. The
change instruments existing DB/P2P owners; protocol IDs, wire messages and
persisted layouts must remain unchanged.

| Donor | Inspected source | Accepted pattern | Forge owner/evidence |
| --- | --- | --- | --- |
| Spring `e6a99f68b67abc4d89fe716755b2e1394a4991f7` | `plugins/net_plugin/include/eosio/net_plugin/net_logger.hpp`, `plugins/net_plugin/net_plugin.cpp`, `programs/nodeos/logging.json` | Named categories, peer/connection context, state transitions distinct from message details | Native P2P owner events and plugin lifecycle capture tests |
| BitShares Core | `libraries/net/node.cpp` (upstream inspected during planning) | Separate connection/retry/cleanup diagnostics | Event vocabulary only; no FC runtime or blockchain policy imported |
| Go libp2p `9cfe2cc00be5b20a0be737f002c99f81b92255c5` | `p2p/host/autorelay/relay_finder.go`, `p2p/protocol/identify/id.go`, `p2p/net/swarm/swarm.go` | Debug for routine selection/retries, structured peer context, errors for service failure | Existing AutoRelay/Identify/topology transitions, no extra discovery loop |
| Rust libp2p `22fb4c784fc55ad8b15d05fdc9f98d663107d4cb` | `protocols/identify/src/handler.rs` | Explicit capacity/negotiation outcome diagnostics | Bounded failure reporting and existing resource state |
| RocksDB `ebaa925006d471de2a839dfffc3f04d49a2cc6a9` | `include/rocksdb/options.h`, `include/rocksdb/env.h`, `include/rocksdb/listener.h`, `logging/auto_roll_logger.cc` | Supplied Logger replaces default file logger; EventListener exposes structured background errors | Private native adapter, no LOG/LOG.old files, preserved WAL/transaction tests |
| Vendored libmdbx | `vendor/libmdbx/mdbx.h` debug API, `mdbx.c` global logger and fatal handling | One process-global callback, unchanged checking flags and fatal semantics | Private driver diagnostics; no per-store callback race or untrusted text dump |

Rejected patterns: copying Spring peer macros, logging full protocol messages,
blockchain-specific block/transaction categories in neutral P2P, per-read INFO,
logging arbitrary exception/vendor text, a second telemetry runtime, or treating
RocksDB `info_log = nullptr` as disabling diagnostics (it selects the file logger).

OpenTelemetry guidance: keep export optional and prefer a local Collector for
production delivery/retention policy. Queue loss and collector failure remain
observable; neither network availability nor DB commit depends on telemetry.
Sources: https://opentelemetry.io/docs/collector/ and
https://opentelemetry.io/docs/collector/resiliency/ . No OpenTelemetry SDK is
introduced; Forge's existing log records/exporter remain the implementation.

Focused test results and final reviewed SHA belong to the acceptance receipt,
not to this source-pattern note. Unrun interop or production-soak checks must not
be inferred from logging coverage.
