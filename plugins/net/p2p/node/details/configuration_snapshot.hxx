#pragma once

#include <memory>
#include <string>
#include <vector>

#include "config.hxx"

namespace forge::plugins::net::p2p::node::detail {

struct configuration_snapshot {
   config declarative{};
   forge::net::p2p::node::options options{};
   forge::api::transport::options api_options{
       .codec = forge::api::core::codec_id{.value = "forge.raw"}, .max_inflight = 64};
   parsed_policy policy{};
   // Declarations may await a complete C++ contribution; never passed incomplete
   // to a native node or made permissive for validation.
   std::vector<forge::net::p2p::dht::profile> dht_profiles;
   std::string peer_store_name;
   std::string certificate_secret;
   std::string private_key_secret;
   pubsub_config pubsub{};
   diagnostics_config diagnostics{};
   bool pubsub_contributed = false;
   bool reset_incompatible_peer_state = true;

   void validate_candidate() const;
   [[nodiscard]] forge::net::p2p::node::options prepare_options() const;
   [[nodiscard]] std::vector<forge::net::p2p::protocol_id> reserved_protocols() const;

 private:
   void validate_options(forge::net::p2p::node::options& prepared, bool require_complete) const;
};

} // namespace forge::plugins::net::p2p::node::detail
