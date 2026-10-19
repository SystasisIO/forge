module;

#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <forge/api/core/macros.hpp>

export module forge.plugins.net.tlsnotary.session.api;

import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
export import forge.net.tlsnotary.types;

export namespace forge::plugins::net::tlsnotary::session {

// Local API: resource streams and cancellation tokens are deliberately not RPC values.
class api : public forge::api::core::contract<api> {
 public:
   virtual ~api() = default;
   virtual boost::asio::awaitable<forge::net::tlsnotary::result>
   prove(forge::net::tlsnotary::prover_request request) = 0;
   virtual boost::asio::awaitable<forge::net::tlsnotary::result>
   verify(forge::net::tlsnotary::verifier_request request) = 0;
   virtual bool cancel(std::uint64_t id) noexcept = 0;
   [[nodiscard]] virtual forge::net::tlsnotary::metrics snapshot() const = 0;
};
} // namespace forge::plugins::net::tlsnotary::session
FORGE_EXPORT_API(::forge::plugins::net::tlsnotary::session::api,
                 FORGE_API_CONTRACT("forge.plugins.net.tlsnotary.session", 1, 0))
