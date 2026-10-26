module;

#include <span>
#include <string_view>

export module forge.net.tlsnotary.exchange;

export import forge.net.tlsnotary.types;
export import forge.crypto.exchange;

export namespace forge::net::tlsnotary {

// The caller authenticates ALL of `verified`, binds freshness/context to the
// operation, and validates visible source semantics (not just retained hashes).
// This gate binds a complete bounded raw body and downloaded ciphertext to it.
void bind_exchange(const result& verified, const forge::crypto::exchange::manifest& offered,
                   const forge::crypto::digest::sha256& expected_context, std::string_view expected_server,
                   byte_range body, std::span<const forge::crypto::core::bytes> ciphertext);

} // namespace forge::net::tlsnotary
