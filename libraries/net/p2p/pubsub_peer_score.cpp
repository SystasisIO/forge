module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/ip/address.hpp>

module forge.net.p2p.node;

import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.pubsub;

#include "details/pubsub_peer_score.hxx"

namespace forge::net::p2p::detail {

bool pubsub_peer_score::topic_less::operator()(const pubsub::topic& left, const pubsub::topic& right) const noexcept {
   return left.value < right.value;
}
bool pubsub_peer_score::topic_less::operator()(const pubsub::topic& left, std::string_view right) const noexcept {
   return left.value < right;
}
bool pubsub_peer_score::topic_less::operator()(std::string_view left, const pubsub::topic& right) const noexcept {
   return left < right.value;
}

// Formulas/attribution: Go 0ed6f6f score.go/score_params.go; Rust 22fb4c78 peer_score.rs.
// Rust's fractional P1, exact-IP P6 and clearing active P3 on prune are intentional choices.
pubsub_peer_score::pubsub_peer_score(pubsub::scoring_params params, clock::time_point now)
    : _params([&] {
         pubsub::validate(params);
         params.app_specific_score = {}; // Sampling remains the caller's responsibility.
         return std::move(params);
      }()),
      _last_now(now), _last_decay(now) {}

bool pubsub_peer_score::message_less::operator()(std::span<const std::uint8_t> left,
                                                std::span<const std::uint8_t> right) const noexcept {
   return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
}

pubsub_peer_score::clock::time_point pubsub_peer_score::deadline(clock::time_point now,
                                                               std::chrono::milliseconds duration) {
   const auto delay = std::chrono::duration_cast<clock::duration>(duration);
   return now > clock::time_point::max() - delay ? clock::time_point::max() : now + delay;
}

pubsub_peer_score::clock::duration pubsub_peer_score::elapsed(clock::time_point from, clock::time_point to) {
   if (to <= from) {
      return clock::duration::zero();
   }
   if (from.time_since_epoch() < clock::duration::zero() &&
       to.time_since_epoch() > clock::duration::max() + from.time_since_epoch()) {
      return clock::duration::max();
   }
   return to - from;
}

double pubsub_peer_score::finite_value(long double value) {
   // Representation saturation only; there is no policy cap on negative scores/counters.
   constexpr auto maximum = static_cast<long double>((std::numeric_limits<double>::max)());
   return static_cast<double>(std::clamp(value, -maximum, maximum));
}

double pubsub_peer_score::add(double left, double right) {
   return finite_value(static_cast<long double>(left) + right);
}

double pubsub_peer_score::weighted(double value, double weight) {
   return weight == 0.0 ? 0.0 : finite_value(static_cast<long double>(value) * weight);
}

double pubsub_peer_score::squared(double value) {
   return finite_value(static_cast<long double>(value) * value);
}

double pubsub_peer_score::weighted_square(double value, double weight) {
   if (weight == 0.0) {
      return 0.0;
   }
   const auto wide = static_cast<long double>(value);
   if (wide <= std::sqrt((std::numeric_limits<long double>::max)())) {
      return finite_value(wide * wide * weight);
   }
   const auto scaled = wide * std::sqrt(std::abs(static_cast<long double>(weight)));
   return finite_value((weight < 0.0 ? -scaled : scaled) * scaled);
}

boost::asio::ip::address pubsub_peer_score::canonical_ip(const boost::asio::ip::address& address) {
   if (address.is_v6() && address.to_v6().is_v4_mapped()) {
      const auto bytes = address.to_v6().to_bytes();
      return boost::asio::ip::address_v4{boost::asio::ip::address_v4::bytes_type{
          bytes[12], bytes[13], bytes[14], bytes[15]}};
   }
   return address;
}

std::optional<std::vector<boost::asio::ip::address>> pubsub_peer_score::prepare_ips(
    std::span<const boost::asio::ip::address> ips) const {
   if (ips.size() > _params.limits.max_ips_per_peer) {
      return std::nullopt;
   }
   auto result = std::vector<boost::asio::ip::address>{};
   result.reserve(ips.size());
   for (const auto& input : ips) {
      const auto ip = canonical_ip(input);
      if (!ip.is_loopback() && !ip.is_unspecified() && !ip.is_multicast() &&
          std::find(result.begin(), result.end(), ip) == result.end()) {
         result.push_back(ip);
      }
   }
   return result;
}

void pubsub_peer_score::capacity_rejected() {
   if (_capacity_rejections != (std::numeric_limits<std::uint64_t>::max)()) {
      ++_capacity_rejections;
   }
}

pubsub_peer_score::ip_index pubsub_peer_score::prepare_ip_entries(
    std::span<const boost::asio::ip::address> ips) const {
   auto entries = ip_index{};
   for (const auto& ip : ips) {
      if (!_ip_peers.contains(ip)) {
         entries.emplace(ip, 0);
      }
   }
   return entries;
}

void pubsub_peer_score::add_ips(std::span<const boost::asio::ip::address> ips) {
   for (const auto& ip : ips) {
      ++_ip_peers.find(ip)->second;
   }
}

void pubsub_peer_score::remove_ips(std::span<const boost::asio::ip::address> ips) {
   for (const auto& ip : ips) {
      const auto it = _ip_peers.find(ip);
      if (--it->second == 0) {
         _ip_peers.erase(it);
      }
   }
}

void pubsub_peer_score::replace_ips(peer_state& state, std::vector<boost::asio::ip::address> ips) {
   // Prepare all allocating nodes before changing either the old facts or their counts.
   auto additions = prepare_ip_entries(ips);
   _ip_peers.merge(additions);
   add_ips(ips);
   remove_ips(state.ips);
   state.ips.swap(ips);
}

void pubsub_peer_score::schedule_retention(peer_state& state, clock::time_point until) {
   auto slot = _peer_expiry.extract(state.expiry);
   slot.key() = until;
   state.expiry = _peer_expiry.insert(std::move(slot));
}

bool pubsub_peer_score::valid_topic(const pubsub::topic& subject) const {
   return !subject.value.empty() && subject.value.size() <= _params.limits.max_topic_size;
}

void pubsub_peer_score::advance(clock::time_point& now) {
   now = std::max(now, _last_now);
   _last_now = now;
   while (!_peer_expiry.empty() && _peer_expiry.begin()->first < now) {
      const auto expired = _peer_expiry.begin();
      const auto peer = _peers.find(expired->second);
      remove_ips(peer->second.ips);
      _peers.erase(peer);
      _peer_expiry.erase(expired);
   }
   while (!_delivery_expiry.empty() && _delivery_expiry.begin()->first < now) {
      const auto expired = _delivery_expiry.begin();
      _deliveries.erase(expired->second);
      _delivery_expiry.erase(expired);
   }
   const auto interval = std::chrono::duration_cast<clock::duration>(_params.decay_interval);
   const auto delta = elapsed(_last_decay, now);
   const auto steps = delta / interval;
   if (steps == 0) {
      return;
   }
   _last_decay = now - delta % interval;
   const auto decay = [&](double& value, double factor) {
      value *= std::pow(factor, static_cast<double>(steps));
      if (value < _params.decay_to_zero) {
         value = 0.0;
      }
   };
   for (auto& [peer, state] : _peers) {
      if (!state.connected) {
         continue;
      }
      decay(state.behaviour_penalty, _params.behaviour_penalty_decay);
      for (auto& [subject, topic] : state.topics) {
         const auto& p = _params.topics.at(subject);
         decay(topic.first_deliveries, p.first_message_deliveries_decay);
         decay(topic.mesh_deliveries, p.mesh_message_deliveries_decay);
         decay(topic.mesh_failure, p.mesh_failure_penalty_decay);
         decay(topic.invalid_deliveries, p.invalid_message_deliveries_decay);
         if (topic.in_mesh) {
            topic.mesh_time = elapsed(topic.grafted, now);
            if (topic.mesh_time > p.mesh_message_deliveries_activation) {
               topic.mesh_active = true;
            }
         }
      }
   }
}

bool pubsub_peer_score::connect(const peer_id& peer, std::span<const boost::asio::ip::address> ips,
                              clock::time_point now) {
   if (peer.value.empty() || peer.value.size() > _params.limits.max_peer_id_size || !valid_peer_id(peer)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub score requires a bounded valid peer ID");
   }
   auto prepared = prepare_ips(ips);
   const auto lock = std::lock_guard{_mutex};
   advance(now);
   if (!prepared) {
      capacity_rejected();
      return false;
   }
   const auto existing = _peers.find(peer);
   if (existing != _peers.end()) {
      if (!existing->second.connected) {
         if (_connected >= _params.limits.max_connected_peers) {
            capacity_rejected();
            return false;
         }
      }
      replace_ips(existing->second, std::move(*prepared));
      if (!existing->second.connected) {
         schedule_retention(existing->second, clock::time_point::max());
         existing->second.connected = true;
         ++_connected;
      }
      return true;
   }
   if (_connected >= _params.limits.max_connected_peers || _peers.size() >= _params.limits.max_retained_peers ||
       _next_peer == (std::numeric_limits<std::uint64_t>::max)()) {
      capacity_rejected();
      return false;
   }
   auto state = peer_state{};
   state.generation = _next_peer;
   state.ips = std::move(*prepared);
   for (const auto& [subject, params] : _params.topics) {
      state.topics.emplace(subject, topic_state{});
   }
   auto additions = prepare_ip_entries(state.ips);
   const auto inserted = _peers.emplace(peer, std::move(state)).first;
   try {
      inserted->second.expiry = _peer_expiry.emplace(clock::time_point::max(), peer);
   } catch (...) {
      _peers.erase(inserted);
      throw;
   }
   // From here publication transfers preallocated nodes; no counter changes precede an allocation.
   _ip_peers.merge(additions);
   add_ips(inserted->second.ips);
   ++_next_peer;
   ++_connected;
   return true;
}

bool pubsub_peer_score::update_ips(const peer_id& peer, std::span<const boost::asio::ip::address> ips,
                                 clock::time_point now) {
   auto prepared = prepare_ips(ips);
   const auto lock = std::lock_guard{_mutex};
   advance(now);
   const auto it = _peers.find(peer);
   if (it == _peers.end() || !it->second.connected) {
      return false;
   }
   if (!prepared) {
      capacity_rejected();
      return false;
   }
   replace_ips(it->second, std::move(*prepared));
   return true;
}

void pubsub_peer_score::remove_ip(const peer_id& peer, const boost::asio::ip::address& ip, clock::time_point now) {
   const auto lock = std::lock_guard{_mutex};
   advance(now);
   const auto row = _peers.find(peer);
   if (row == _peers.end() || !row->second.connected) {
      return;
   }
   const auto address = canonical_ip(ip);
   const auto found = std::find(row->second.ips.begin(), row->second.ips.end(), address);
   if (found != row->second.ips.end()) {
      remove_ips(std::span{&*found, std::size_t{1}});
      row->second.ips.erase(found);
   }
}

void pubsub_peer_score::leave_mesh(topic_state& state, const pubsub::topic_score_params& p) {
   if (state.in_mesh && state.mesh_active && state.mesh_deliveries < p.mesh_message_deliveries_threshold) {
      state.mesh_failure = add(state.mesh_failure, squared(p.mesh_message_deliveries_threshold - state.mesh_deliveries));
   }
   state.in_mesh = false;
   state.mesh_active = false;
}

bool pubsub_peer_score::disconnect(const peer_id& peer, clock::time_point now) {
   const auto lock = std::lock_guard{_mutex};
   advance(now);
   const auto it = _peers.find(peer);
   if (it == _peers.end() || !it->second.connected) {
      return false;
   }
   --_connected;
   if (score_locked(it->second) > 0.0 || _params.retain_score.count() == 0) {
      remove_ips(it->second.ips);
      _peer_expiry.erase(it->second.expiry);
      _peers.erase(it);
      return true;
   }
   auto& state = it->second;
   for (auto& [subject, topic] : state.topics) {
      topic.first_deliveries = 0.0;
      leave_mesh(topic, _params.topics.at(subject));
   }
   // No allocation on this terminal transition; admission already reserved the slot.
   state.connected = false;
   state.retain_until = deadline(now, _params.retain_score);
   schedule_retention(state, state.retain_until);
   return true;
}

bool pubsub_peer_score::graft(const peer_id& peer, const pubsub::topic& subject, clock::time_point now) {
   return graft(peer, std::string_view{subject.value}, now);
}

bool pubsub_peer_score::graft(const peer_id& peer, std::string_view subject, clock::time_point now) {
   const auto lock = std::lock_guard{_mutex};
   advance(now);
   const auto it = _peers.find(peer);
   if (it == _peers.end() || !it->second.connected || subject.empty() || subject.size() > _params.limits.max_topic_size) {
      return false;
   }
   const auto topic = it->second.topics.find(subject);
   if (topic == it->second.topics.end()) {
      return true; // Unconfigured topics are neutral, not failed validation/admission.
   }
   if (topic->second.in_mesh) {
      return false;
   }
   topic->second.in_mesh = true;
   topic->second.mesh_active = false;
   topic->second.mesh_time = clock::duration::zero();
   topic->second.grafted = now;
   return true;
}

bool pubsub_peer_score::prune(const peer_id& peer, const pubsub::topic& subject, clock::time_point now) {
   return prune(peer, std::string_view{subject.value}, now);
}

bool pubsub_peer_score::prune(const peer_id& peer, std::string_view subject, clock::time_point now) {
   const auto lock = std::lock_guard{_mutex};
   advance(now);
   const auto it = _peers.find(peer);
   if (it == _peers.end() || subject.empty() || subject.size() > _params.limits.max_topic_size) {
      return false;
   }
   const auto topic = it->second.topics.find(subject);
   if (topic == it->second.topics.end()) {
      return true;
   }
   if (!topic->second.in_mesh) {
      return false;
   }
   leave_mesh(topic->second, _params.topics.at(topic->first));
   return true;
}

bool pubsub_peer_score::set_application_score(const peer_id& peer, double sample, clock::time_point now) {
   if (!std::isfinite(sample)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub application score must be finite");
   }
   const auto lock = std::lock_guard{_mutex};
   advance(now);
   const auto it = _peers.find(peer);
   if (it == _peers.end()) {
      return false;
   }
   it->second.application_score = sample;
   return true;
}

bool pubsub_peer_score::add_behaviour_penalty(const peer_id& peer, double amount, clock::time_point now) {
   if (!std::isfinite(amount) || amount < 0.0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub behaviour penalty must be finite and nonnegative");
   }
   const auto lock = std::lock_guard{_mutex};
   advance(now);
   const auto it = _peers.find(peer);
   if (it == _peers.end()) {
      return false;
   }
   it->second.behaviour_penalty = add(it->second.behaviour_penalty, amount);
   return true;
}

void pubsub_peer_score::tick(clock::time_point now) {
   const auto lock = std::lock_guard{_mutex};
   advance(now);
}

double pubsub_peer_score::score(const peer_id& peer, clock::time_point now) {
   const auto lock = std::lock_guard{_mutex};
   advance(now);
   const auto it = _peers.find(peer);
   return it == _peers.end() ? 0.0 : score_locked(it->second);
}

std::optional<pubsub::peer_score_snapshot> pubsub_peer_score::inspect(const peer_id& peer, clock::time_point now) {
   const auto lock = std::lock_guard{_mutex};
   advance(now);
   const auto it = _peers.find(peer);
   return it == _peers.end() ? std::nullopt : std::optional{inspect_locked(it->first, it->second)};
}

pubsub::score_snapshot pubsub_peer_score::snapshot(clock::time_point now) {
   const auto lock = std::lock_guard{_mutex};
   advance(now);
   auto result = pubsub::score_snapshot{};
   result.connected_peers = _connected;
   result.retained_peers = _peers.size() - _connected;
   result.delivery_records = _deliveries.size();
   result.capacity_rejections = _capacity_rejections;
   result.peers.reserve(_peers.size());
   for (const auto& [peer, state] : _peers) {
      result.peers.push_back(inspect_locked(peer, state));
   }
   for (const auto& [id, record] : _deliveries) {
      result.pending_validations += record.status == delivery_status::pending;
   }
   return result;
}

} // namespace forge::net::p2p::detail
