module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module forge.net.p2p.node;

import forge.net.p2p.identity;
import forge.net.p2p.pubsub;
import forge.multiformats.varint;

#include "details/pubsub_control_queue.hxx"

namespace forge::net::p2p::detail {

pubsub_control_queue::pubsub_control_queue(const pubsub::limits& limits)
    : _limits(limits), _capacity(capacity_for(limits)) {}

std::size_t pubsub_control_queue::capacity_for(const pubsub::limits& limits) noexcept {
   const auto ceiling = std::min(limits.max_remote_topic_entries, limits.max_control_entries);
   if (ceiling == 0 || limits.max_topics == 0 || limits.max_peers_per_topic == 0) { return 0; }
   // Only multiply when the product fits below the already representable ceiling.
   if (limits.max_topics > ceiling / limits.max_peers_per_topic) { return ceiling; }
   return std::min(ceiling, limits.max_topics * limits.max_peers_per_topic);
}

std::optional<pubsub_control_queue::prepared> pubsub_control_queue::prepare(std::vector<command> commands) {
   if (commands.size() > _capacity - _size ||
       commands.size() > (std::numeric_limits<std::uint64_t>::max)() - _revision) { return std::nullopt; }
   auto out = prepared{};
   auto bytes = std::size_t{};
   auto options = pubsub::options{.limits = _limits};
   for (auto& command : commands) {
      if (command.generation == 0 || command.args.subject.value.empty() ||
          command.args.subject.value.size() > _limits.max_topic_size) { return std::nullopt; }
      auto rpc_value = pubsub::rpc{.control_value = pubsub::control{}};
      if (command.operation == kind::graft) {
         rpc_value.control_value->grafts.push_back(pubsub::control::graft{.subject = command.args.subject});
      } else {
         rpc_value.control_value->prunes.push_back(command.args);
      }
      options.preferred = pubsub::version::v1_1;
      const auto modern = pubsub::codec::encode(rpc_value, options).size();
      const auto modern_control = pubsub::codec::control_payload_size(*rpc_value.control_value, options);
      options.preferred = pubsub::version::v1_0;
      const auto legacy = pubsub::codec::encode(rpc_value, options).size();
      const auto legacy_control = pubsub::codec::control_payload_size(*rpc_value.control_value, options);
      const auto wire_bytes = std::max(modern, legacy);
      const auto remaining = _limits.max_outbound_queue_bytes - _bytes - bytes;
      if (wire_bytes > remaining ||
          command.peer.value.size() > remaining - wire_bytes ||
          command.args.subject.value.size() > remaining - wire_bytes - command.peer.value.size()) {
         return std::nullopt;
      }
      const auto charge = wire_bytes + command.peer.value.size() + command.args.subject.value.size();
      auto item = std::make_shared<intent>(intent{.peer = command.peer, .generation = command.generation,
         .revision = ++_revision, .operation = command.operation, .args = std::move(command.args),
         .wire_bytes = wire_bytes, .control_bytes = std::max(modern_control, legacy_control), .charged_bytes = charge});
      auto& row = out.rows[item->peer];
      if (!row.emplace(item->args.subject.value, std::move(item)).second) { return std::nullopt; }
      bytes += charge;
   }
   return out;
}

bool pubsub_control_queue::leased(const intent& value) const noexcept {
   const auto found = _inflight.find(value.peer);
   if (found == _inflight.end()) { return false; }
   return std::any_of(found->second->items.begin(), found->second->items.end(),
       [&](const auto& item) { return item.get() == &value; });
}

void pubsub_control_queue::release(const intent& value) noexcept {
   --_size;
   _bytes -= value.charged_bytes;
}

void pubsub_control_queue::commit(prepared value) noexcept {
   while (!value.rows.empty()) {
      auto row = value.rows.extract(value.rows.begin());
      const auto existing = _rows.find(row.key());
      for (const auto& [_, item] : row.mapped()) {
         ++_size;
         _bytes += item->charged_bytes;
      }
      if (existing == _rows.end()) {
         _rows.insert(std::move(row));
         continue;
      }
      while (!row.mapped().empty()) {
         auto topic = row.mapped().extract(row.mapped().begin());
         const auto old = existing->second.find(topic.key());
         if (old != existing->second.end()) {
            if (!leased(*old->second)) { release(*old->second); }
            existing->second.erase(old);
         }
         existing->second.insert(std::move(topic));
      }
   }
}

std::vector<peer_id> pubsub_control_queue::peers() const {
   auto out = std::vector<peer_id>{};
   out.reserve(_rows.size());
   for (const auto& [peer, _] : _rows) { out.push_back(peer); }
   return out;
}

std::shared_ptr<const pubsub_control_queue::batch> pubsub_control_queue::acquire(
    const peer_id& peer, std::uint64_t generation, std::size_t ephemeral_bytes) {
   const auto row = _rows.find(peer);
   if (ephemeral_bytes > _limits.max_outbound_queue_bytes - _bytes) { ephemeral_bytes = 0; }
   if ((row == _rows.end() && (generation == 0 || ephemeral_bytes == 0)) || _inflight.contains(peer) ||
       _inflight.size() >= _capacity) { return {}; }
   auto out = std::make_shared<batch>();
   out->peer = peer;
   out->generation = generation;
   out->ephemeral_bytes = ephemeral_bytes;
   out->items.reserve(row == _rows.end() ? 0 : std::min(row->second.size(), _limits.max_control_entries));
   auto bytes = std::size_t{};
   auto grafts = std::size_t{};
   if (row != _rows.end()) {
      // PRUNE cannot be hidden behind a full outbound GRAFT quota. Unselected
      // immutable intents remain current for the next heartbeat lease.
      for (const auto operation : {kind::prune, kind::graft}) {
         for (const auto& [_, item] : row->second) {
            if (item->operation != operation ||
                ((!out->items.empty() || generation != 0) && item->generation != out->generation)) { continue; }
            if (out->items.size() >= _limits.max_control_entries ||
                (operation == kind::graft && grafts == _limits.max_graft_per_peer) ||
                item->control_bytes > _limits.max_rpc_size - bytes) { continue; }
            const auto candidate = bytes + item->control_bytes;
            const auto header = 1U + forge::multiformats::varint_encoded_size(candidate);
            if (header > _limits.max_rpc_size - candidate) { continue; }
            out->generation = item->generation;
            out->items.push_back(item);
            bytes = candidate;
            if (operation == kind::graft) { ++grafts; }
         }
      }
   }
   if (out->items.empty() && ephemeral_bytes == 0) { return {}; }
   _inflight.emplace(peer, out);
   _bytes += ephemeral_bytes;
   return out;
}

pubsub::rpc pubsub_control_queue::rpc(const batch& value) {
   auto out = pubsub::rpc{.control_value = pubsub::control{}};
   out.control_value->grafts.reserve(value.items.size());
   out.control_value->prunes.reserve(value.items.size());
   for (const auto& item : value.items) {
      if (item->operation == kind::graft) {
         out.control_value->grafts.push_back(pubsub::control::graft{.subject = item->args.subject});
      } else {
         out.control_value->prunes.push_back(item->args);
      }
   }
   return out;
}

bool pubsub_control_queue::current(const intent& value) const noexcept {
   const auto row = _rows.find(value.peer);
   if (row == _rows.end()) { return false; }
   const auto topic = row->second.find(value.args.subject.value);
   return topic != row->second.end() && topic->second->generation == value.generation &&
       topic->second->revision == value.revision;
}

bool pubsub_control_queue::current(const batch& value) const noexcept {
   return std::all_of(value.items.begin(), value.items.end(), [&](const auto& item) { return current(*item); });
}

void pubsub_control_queue::discard(const intent& value) noexcept {
   const auto row = _rows.find(value.peer);
   if (row == _rows.end()) { return; }
   const auto topic = row->second.find(value.args.subject.value);
   if (topic == row->second.end() || topic->second->revision != value.revision ||
       topic->second->generation != value.generation) { return; }
   if (!leased(*topic->second)) { release(*topic->second); }
   row->second.erase(topic);
   if (row->second.empty()) { _rows.erase(row); }
}

void pubsub_control_queue::discard(const peer_id& peer, const std::string& topic, kind operation) noexcept {
   const auto row = _rows.find(peer);
   if (row == _rows.end()) { return; }
   const auto item = row->second.find(topic);
   if (item != row->second.end() && item->second->operation == operation) { discard(*item->second); }
}

void pubsub_control_queue::finish(const std::shared_ptr<const batch>& value, bool written) noexcept {
   if (!value) { return; }
   const auto active = _inflight.find(value->peer);
   if (active == _inflight.end() || active->second != value) { return; }
   if (written) { acknowledge(value); }
   for (const auto& item : value->items) {
      if (!current(*item)) { release(*item); }
   }
   _bytes -= value->ephemeral_bytes;
   _inflight.erase(active);
}

void pubsub_control_queue::acknowledge(const std::shared_ptr<const batch>& value) noexcept {
   if (!value) { return; }
   const auto active = _inflight.find(value->peer);
   if (active == _inflight.end() || active->second != value) { return; }
   for (const auto& item : value->items) { discard(*item); }
}

void pubsub_control_queue::forget(const peer_id& peer) noexcept {
   const auto row = _rows.find(peer);
   if (row == _rows.end()) { return; }
   for (const auto& [_, item] : row->second) {
      if (!leased(*item)) { release(*item); }
   }
   _rows.erase(row);
}

void pubsub_control_queue::clear() noexcept {
   while (!_rows.empty()) { forget(_rows.begin()->first); }
}

std::size_t pubsub_control_queue::size() const noexcept { return _size; }
std::size_t pubsub_control_queue::bytes() const noexcept { return _bytes; }

} // namespace forge::net::p2p::detail
