module;

#include <forge/api/core/macros.hpp>
#include <chrono>
#include <cstddef>
#include <string>

export module forge.plugins.chain.outbox.setup;

import forge.api.chain.outbox;
import forge.api.chain.destination;
import forge.api.core.binding;
import forge.chain.api.verified_client_factory;

export namespace forge::plugins::chain::outbox {

class setup : public forge::api::core::contract<setup> {
 public:
   virtual ~setup() = default;

   // The application configures a checked chain before the recovery worker starts.
   virtual void configure(std::string store, forge::chain::api::verified_client_options verification,
                          decltype(forge::api::chain::outbox::options::prepare_first_send) prepare_first_send,
                          forge::api::chain::outbox::admission admit) = 0;

   virtual void configure_read(forge::api::core::api_ref contract, forge::api::chain::destination target,
                               forge::chain::api::verified_client_options verification, std::size_t max_calls,
                               std::chrono::milliseconds timeout, bool trust_execution = false) = 0;
};

} // namespace forge::plugins::chain::outbox

FORGE_EXPORT_API(::forge::plugins::chain::outbox::setup,
                 FORGE_API_CONTRACT("forge.plugins.chain.outbox.setup", 1, 0))
