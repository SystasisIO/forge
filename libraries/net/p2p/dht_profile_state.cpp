module;

#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include <forge/exceptions/macros.hpp>

module forge.net.p2p.node;

import forge.net.p2p.dht;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.protocol;

#include "details/dht_profile_state.hxx"

namespace forge::net::p2p::detail {

dht_profile_state::dht_profile_state(peer_id local, dht::profile profile_value,
                                     dht::record_store::options records_options)
    : profile(std::move(profile_value)), routing(std::move(local), profile.limits),
      records(profile, std::move(records_options)) {}

std::map<protocol_id, std::unique_ptr<dht_profile_state>>
make_dht_profile_states(const peer_id& local, std::vector<dht::profile> profiles,
                        const std::map<protocol_id, dht::record_store::options>& records_options,
                        public_key_resolver public_keys) {
   auto protocols = std::set<protocol_id>{};
   for (const auto& profile : profiles) {
      validate(profile);
      if (const auto configured = records_options.find(profile.protocol); configured != records_options.end()) {
         validate(profile, configured->second);
      }
      if (!protocols.insert(profile.protocol).second) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P DHT profile protocol IDs must be unique");
      }
   }
   auto backend_identities = std::set<const dht::record_store::persistence*>{};
   for (const auto& [protocol, options] : records_options) {
      if (!protocols.contains(protocol)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P DHT records are configured for an unknown profile");
      }
      validate(options);
      if (options.persistence && !backend_identities.insert(options.persistence.get()).second) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P DHT profiles require distinct persistence backends");
      }
   }
   auto out = std::map<protocol_id, std::unique_ptr<dht_profile_state>>{};
   for (auto& profile : profiles) {
      const auto protocol = profile.protocol;
      const auto configured = records_options.find(protocol);
      auto effective = configured == records_options.end() ? dht::record_store::options_for(profile)
                                                           : configured->second;
      if (!effective.public_keys) {
         effective.public_keys = public_keys;
      }
      out.emplace(protocol, std::make_unique<dht_profile_state>(local, std::move(profile), std::move(effective)));
   }
   return out;
}

} // namespace forge::net::p2p::detail
