# Forge Swarm

Swarm's root is an empty grouping namespace, not a networking backend or product policy.
Its first leaf, [Core](core/README.md), owns bounded immutable file descriptors,
content identities, SHA-256 Merkle mechanics and filesystem-neutral store
contracts. [DB](db/README.md) declares indexed metadata, not live storage or
payload. `storage/` implements ordinary-file storage, resume and the Object
Store catalog adapter with caller-owned Forge Asio executors.

[Protocol](protocol/README.md) owns bounded network values and metadata assembly;
[API](api/README.md) declares the live server/duplex streaming contract, reusing
Forge.API authentication and codecs without owning a network node.
[Conversion](conversion/README.md) explicitly projects indexed content into public
metadata and validates imported descriptors without allocating or saving DB rows.
Link the individual leaf target and import only the required modules.

There is no family aggregate module or target. Production runtime scheduling,
authorization, retention and payment are outside these value/contract components;
the runtime implementation follows separately.
