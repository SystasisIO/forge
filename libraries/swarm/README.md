# Forge Swarm

Swarm is an empty library family, not a networking backend or product policy.
Its first leaf, [Core](core/README.md), owns bounded immutable file descriptors,
content identities, SHA-256 Merkle mechanics and filesystem-neutral store
contracts. [DB](db/README.md) declares indexed metadata, not live storage or
payload. `storage/` implements ordinary-file storage, resume and the Object
Store catalog adapter with caller-owned Forge Asio executors. Link `forge_swarm_core` and
import only the required `forge.swarm.core.*` modules.

There is no family aggregate module or target. Peer protocols,
runtime scheduling, authorization, retention and payment are outside Core.
