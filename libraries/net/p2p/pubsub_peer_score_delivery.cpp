module;

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/ip/address.hpp>

module forge.net.p2p.node;

import forge.net.p2p.identity;
import forge.net.p2p.pubsub;

#include "details/pubsub_peer_score.hxx"

namespace forge::net::p2p::detail {

std::optional<pubsub_peer_score::validation> pubsub_peer_score::validation_start(
    const peer_id& source, const pubsub::topic& subject, std::span<const std::uint8_t> id, clock::time_point now) {
   const auto lock = std::lock_guard{_mutex};
   advance(now);
   const auto peer = _peers.find(source);
   if (peer == _peers.end() || !peer->second.connected || !valid_topic(subject)) {
      return std::nullopt;
   }
   if (id.empty() || id.size() > _params.limits.max_message_id_size) {
      capacity_rejected();
      return std::nullopt;
   }
   if (_deliveries.contains(id)) {
      return std::nullopt;
   }
   if (_deliveries.size() >= _params.limits.max_messages ||
       _next_delivery == (std::numeric_limits<std::uint64_t>::max)()) {
      capacity_rejected();
      return std::nullopt;
   }
   auto ticket = validation{{id.begin(), id.end()}, _next_delivery};
   auto record = delivery_record{};
   record.generation = ticket.generation;
   record.subject = subject;
   record.first = {source, peer->second.generation};
   record.expires = deadline(now, _params.seen_message_ttl);
   // Reserve before publishing the validation owner. Message pressure never evicts pending attribution.
   record.duplicates.reserve(_params.limits.max_delivery_peers - 1);
   const auto inserted = _deliveries.emplace(ticket.message_id, std::move(record)).first;
   try {
      inserted->second.expiry = _delivery_expiry.emplace(inserted->second.expires, ticket.message_id);
   } catch (...) {
      _deliveries.erase(inserted);
      throw;
   }
   ++_next_delivery;
   return ticket;
}

void pubsub_peer_score::mark_delivery(const peer_id& source, std::uint64_t generation,
                                    const pubsub::topic& subject, bool first, bool invalid) {
   const auto peer = _peers.find(source);
   if (peer == _peers.end() || peer->second.generation != generation) {
      return;
   }
   const auto topic = peer->second.topics.find(subject);
   if (topic == peer->second.topics.end()) {
      return;
   }
   auto& state = topic->second;
   const auto& params = _params.topics.at(subject);
   if (invalid) {
      state.invalid_deliveries = add(state.invalid_deliveries, 1.0);
      return;
   }
   if (first) {
      state.first_deliveries = std::min(add(state.first_deliveries, 1.0), params.first_message_deliveries_cap);
   }
   if (state.in_mesh) {
      state.mesh_deliveries = std::min(add(state.mesh_deliveries, 1.0), params.mesh_message_deliveries_cap);
   }
}

bool pubsub_peer_score::validation_complete(const validation& ticket, pubsub::validation_result result,
                                          clock::time_point now) {
   return validation_complete(ticket.message_id, ticket.generation, result, now);
}

bool pubsub_peer_score::validation_complete(std::span<const std::uint8_t> id, std::uint64_t generation,
                                          pubsub::validation_result result, clock::time_point now) {
   const auto lock = std::lock_guard{_mutex};
   advance(now);
   if (id.empty() || id.size() > _params.limits.max_message_id_size) {
      return false;
   }
   const auto it = _deliveries.find(id);
   if (it == _deliveries.end() || it->second.generation != generation ||
       it->second.status != delivery_status::pending) {
      return false;
   }
   auto& record = it->second;
   switch (result) {
   case pubsub::validation_result::retry:
      return true;
   case pubsub::validation_result::ignore:
      record.status = delivery_status::ignored;
      record.first_pending_duplicate = false;
      record.duplicates.clear();
      return true;
   case pubsub::validation_result::accept:
      record.status = delivery_status::valid;
      record.validated = now;
      mark_delivery(record.first.peer, record.first.generation, record.subject, true, false);
      // Duplicates seen while validation was pending count even after a slow validation.
      for (const auto& peer : record.duplicates) {
         mark_delivery(peer.peer, peer.generation, record.subject, false, false);
      }
      record.first_pending_duplicate = false;
      return true;
   case pubsub::validation_result::reject:
      record.status = delivery_status::invalid;
      mark_delivery(record.first.peer, record.first.generation, record.subject, false, true);
      if (record.first_pending_duplicate) {
         // Donors reject the first delivery and its one tracked pending duplicate separately.
         mark_delivery(record.first.peer, record.first.generation, record.subject, false, true);
      }
      for (const auto& peer : record.duplicates) {
         mark_delivery(peer.peer, peer.generation, record.subject, false, true);
      }
      record.first_pending_duplicate = false;
      record.duplicates.clear();
      return true;
   }
   return false;
}

bool pubsub_peer_score::duplicate_delivery(const peer_id& source, const pubsub::topic& subject,
                                         std::span<const std::uint8_t> id, clock::time_point now) {
   const auto lock = std::lock_guard{_mutex};
   advance(now);
   const auto peer = _peers.find(source);
   if (peer == _peers.end() || !peer->second.connected || id.empty() ||
       id.size() > _params.limits.max_message_id_size) {
      return false;
   }
   const auto it = _deliveries.find(id);
   if (it == _deliveries.end() || it->second.subject != subject) {
      return false;
   }
   auto& record = it->second;
   const auto generation = peer->second.generation;
   if (record.status == delivery_status::ignored) {
      return true;
   }
   if (record.status == delivery_status::invalid) {
      // Donors penalize each invalid redelivery, not just the first one from a peer.
      mark_delivery(source, generation, subject, false, true);
      return true;
   }
   const auto matches = [&](const peer_reference& stored) {
      return stored.peer == source && stored.generation == generation;
   };
   if (matches(record.first)) {
      // The first peer already owns one bounded attribution slot; do not allocate another or double valid credit.
      if (record.status == delivery_status::pending) {
         record.first_pending_duplicate = true;
      }
      return true;
   }
   if (std::any_of(record.duplicates.begin(), record.duplicates.end(), matches)) {
      return true;
   }
   if (record.duplicates.size() >= _params.limits.max_delivery_peers - 1) {
      capacity_rejected();
      return false;
   }
   record.duplicates.push_back({source, generation});
   const auto params = _params.topics.find(subject);
   if (record.status == delivery_status::valid && params != _params.topics.end() &&
       elapsed(record.validated, now) <= params->second.mesh_message_deliveries_window) {
      mark_delivery(source, generation, subject, false, false);
   }
   return true;
}

bool pubsub_peer_score::reject_invalid(const peer_id& source, const pubsub::topic& subject, clock::time_point now) {
   const auto lock = std::lock_guard{_mutex};
   advance(now);
   const auto peer = _peers.find(source);
   if (peer == _peers.end() || !peer->second.connected || !valid_topic(subject)) {
      return false;
   }
   mark_delivery(source, peer->second.generation, subject, false, true);
   return true;
}

} // namespace forge::net::p2p::detail
