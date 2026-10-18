# Forge Swarm

Swarm is an empty library family, not a networking backend or product policy.
Its first leaf, [Core](core/README.md), owns bounded immutable file descriptors,
content identities and SHA-256 Merkle mechanics. Link `forge_swarm_core` and
import only the required `forge.swarm.core.*` modules.

There is no family aggregate module or target. Storage, peer protocols,
runtime scheduling, authorization, retention and payment are outside Core.
