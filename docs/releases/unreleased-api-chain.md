# Verified reads and first-send admission in the Chain API

A contract can declare `FORGE_CHAIN_READ` beside its actions. The generated
method takes a finalized block ID and passes a verified snapshot to the local
handler. An action can take an explicit reference block and expiration. Existing
calls to generated action methods continue to use their default signing context.

The preview `forge::api::chain::runtime` C++ interface changes. Applications
using the bundled `forge.plugins.chain.outbox` receive the updated runtime.
Custom implementations must accept optional `action_options` in `begin` and
implement `finalized` and scoped `read` using authenticated chain data. Scoped
reads accept an optional block and an optional typed API reference. Implement
`read_target` to return the configured destination and `same_read_profile` to
check whether a typed operation can borrow an existing snapshot.

Applications using the bundled plugin can call `setup::configure_read` during
initialization to register a separate verified profile for a typed API. The
profile has its own destination, trust configuration, deadline and concurrency
limit. `read<Interface>(view, handler)` resolves that source and selects one
verified finalized block for all typed reads in the handler. A deployment can
enable this path with only read profiles configured.

Applications can use `outbox::options::prepare_first_send` to decide whether to
send a staged packet after recovery. The callback commits the application's
decision and the first-send marker in the same transaction. Previously admitted
packets that may already have been sent continue under their saved delivery
policy. See the [Chain API](../../libraries/api/chain/README.md) and
[outbox plugin](../../plugins/chain/outbox/README.md).
