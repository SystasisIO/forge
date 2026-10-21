module;

#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

module forge.net.p2p.node;

import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.multiformats.multiaddr;

#include "details/open_address_hints.hxx"

namespace forge::net::p2p::detail {

open_address_hints::open_address_hints(const peer_id& target,
   std::vector<forge::multiformats::multiaddr> addresses, address_resolution::limits bounds,
   std::size_t attempts, std::chrono::milliseconds attempt_timeout, std::shared_ptr<cancellation_latch> stop)
   : root_limit{bounds.max_roots}, max_direct_endpoints{attempts}, direct_attempt_timeout{attempt_timeout},
     cancellation{std::move(stop)} {
   if (addresses.size() > bounds.max_roots) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P operation address hint count exceeds native root limit");
   }
   for (auto& address : addresses) {
      const auto& components = address.components();
      auto size = std::size_t{};
      if (components.size() > bounds.max_multiaddr_size) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P operation address hint exceeds native size limit");
      }
      for (const auto& component : components) {
         if (component.value.size() > bounds.max_multiaddr_size - size) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P operation address hint exceeds native size limit");
         }
         size += component.value.size();
      }
      if (address.to_string().size() > bounds.max_multiaddr_size) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P operation address hint exceeds native size limit");
      }
      auto circuit = std::optional<std::size_t>{};
      for (auto index = std::size_t{}; index < components.size(); ++index) {
         if (components[index].code == forge::multiformats::protocol_code::p2p_circuit) {
            if (circuit || index == 0U || index + 2U != components.size() ||
                components[index - 1U].code != forge::multiformats::protocol_code::p2p ||
                components[index + 1U].code != forge::multiformats::protocol_code::p2p) {
               FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P operation circuit hint has invalid peer suffixes");
            }
            circuit = index;
         }
      }
      auto root = forge::multiformats::multiaddr{};
      auto expected = target;
      if (circuit) {
         if (peer_id::from_string(components.back().value) != target) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P operation circuit hint targets a different peer");
         }
         expected = peer_id::from_string(components[*circuit - 1U].value);
         if (expected == target) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P operation circuit hint relays through its target");
         }
         for (auto index = std::size_t{}; index < *circuit; ++index) root.push(components[index]);
      } else { root = std::move(address); }
      const auto& prefix = root.components();
      for (auto index = std::size_t{}; index < prefix.size(); ++index) {
         if (prefix[index].code == forge::multiformats::protocol_code::p2p &&
             (index + 1U != prefix.size() || peer_id::from_string(prefix[index].value) != expected)) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P operation hint disagrees with expected peer");
         }
      }
      const auto dnsaddr = !prefix.empty() && prefix.front().code == forge::multiformats::protocol_code::dnsaddr;
      const auto identity_only = circuit && prefix.size() == 1U && prefix.front().code == forge::multiformats::protocol_code::p2p;
      if (!dnsaddr && !identity_only) {
         const auto endpoint = parse_endpoint(root);
         if (!endpoint.is_direct_tcp() && !endpoint.is_direct_quic()) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P operation hint requires a supported carrier");
         }
      }
      auto* roots = &direct;
      if (circuit) {
         auto found = std::ranges::find_if(relays, [&](const auto& value) { return value.peer == expected; });
         if (found == relays.end()) { relays.push_back({expected, {}}); found = std::prev(relays.end()); }
         roots = &found->carriers;
      }
      if (!identity_only && std::ranges::none_of(*roots, [&](const auto& value) { return value.components() == root.components(); })) {
         roots->push_back(std::move(root));
      }
   }
}

std::vector<forge::multiformats::multiaddr> open_address_hints::carriers(const peer_id& peer) const {
   const auto found = std::ranges::find_if(relays, [&](const auto& value) { return value.peer == peer; });
   return found == relays.end() ? std::vector<forge::multiformats::multiaddr>{} : found->carriers;
}

} // namespace forge::net::p2p::detail
