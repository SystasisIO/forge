module;

#include <forge/exceptions/macros.hpp>
#include <boost/asio/awaitable.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.api.transport.options;
import forge.config.core.component;
import forge.net.p2p.dht;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.exceptions;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.scoring;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.types;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.db.store.api;

#include "details/configuration_snapshot.hxx"
#include "details/object_dht_record_store_adapter.hxx"

namespace forge::plugins::net::p2p::node::detail {

void configuration_snapshot::validate_options(forge::net::p2p::node::options& prepared, bool require_complete) const {
   try {
      for (const auto& profile : dht_profiles) {
         forge::net::p2p::validate_declaration(profile);
         const auto defaults = forge::net::p2p::dht::record_store::options_for(profile);
         const auto configured = prepared.dht_records.find(profile.protocol);
         const auto& records = configured == prepared.dht_records.end() ? defaults : configured->second;
         forge::net::p2p::validate(profile, records);
         if (!peer_store_name.empty()) {
            object_dht_record_store_adapter::validate_options(profile.protocol, records);
         }
         if (profile.kind == forge::net::p2p::dht::profile_kind::custom && profile.capabilities.values &&
             profile.value_policies.empty()) {
            if (require_complete) {
               FORGE_THROW_EXCEPTION(exceptions::invalid_config, "custom DHT values require a complete C++ policy set");
            }
            // Only this validation copy omits incomplete profiles and their
            // records. Startup always requires the complete consumer policy.
            prepared.dht_records.erase(profile.protocol);
            continue;
         }
         prepared.dht_profiles.push_back(profile);
      }
      forge::net::p2p::validate_policy(prepared);
   } catch (const forge::net::p2p::exceptions::invalid_options& error) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, error.what());
   }
}

void configuration_snapshot::validate_candidate() const {
   auto prepared = options;
   validate_options(prepared, false);
}

forge::net::p2p::node::options configuration_snapshot::prepare_options() const {
   auto prepared = options;
   validate_options(prepared, true);
   return prepared;
}

std::vector<forge::net::p2p::protocol_id> configuration_snapshot::reserved_protocols() const {
   auto result = std::vector<forge::net::p2p::protocol_id>{};
   result.reserve(dht_profiles.size());
   for (const auto& profile : dht_profiles) { result.push_back(profile.protocol); }
   return result;
}

} // namespace forge::plugins::net::p2p::node::detail
