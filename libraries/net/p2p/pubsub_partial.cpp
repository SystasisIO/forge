module;

#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

module forge.net.p2p.node;
import forge.net.p2p.pubsub;
import forge.net.p2p.exceptions;

#include "details/pubsub_partial.hxx"

namespace forge::net::p2p::detail {

pubsub_partial::pubsub_partial() : _state(std::make_shared<state>()) {}

std::shared_ptr<pubsub_partial::registration> pubsub_partial::prepare(pubsub::topic subject,
                                                                   pubsub::partial_options options) {
   auto value = std::make_shared<registration>();
   value->options = std::move(options);
   const auto lock = std::scoped_lock{_state->mutex};
   if (_state->closed || _state->next == (std::numeric_limits<std::uint64_t>::max)()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "Partial registration owner closed");
   }
   value->generation = _state->next++;
   value->token = partial_topic_access::make(_state, std::move(subject), value->generation);
   return value;
}

std::shared_ptr<pubsub_partial::registration> pubsub_partial::install(const std::shared_ptr<registration>& value,
                                                                   const pubsub::limits& limits) {
   const auto lock = std::scoped_lock{_state->mutex};
   if (_state->closed) { FORGE_THROW_EXCEPTION(exceptions::closed, "Partial registration owner closed"); }
   const auto& subject = value->token.subject().value;
   if (!_state->topics.contains(subject) && _state->topics.size() >= limits.max_topics) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "Partial topic limit reached");
   }
   const auto [row, inserted] = _state->topics.try_emplace(subject);
   auto previous = std::exchange(row->second.value, value);
   _state->groups -= row->second.groups.size();
   _state->group_bytes -= row->second.bytes;
   row->second.groups.clear();
   row->second.bytes = 0;
   return previous;
}

std::shared_ptr<pubsub_partial::registration> pubsub_partial::find(const pubsub::topic& subject) const {
   const auto lock = std::scoped_lock{_state->mutex};
   const auto row = _state->topics.find(subject.value);
   return row == _state->topics.end() ? nullptr : row->second.value;
}

bool pubsub_partial::current(const pubsub::partial_topic& token) const noexcept {
   const auto lock = std::scoped_lock{_state->mutex};
   const auto row = _state->topics.find(token.subject().value);
   return !_state->closed && row != _state->topics.end() &&
       partial_topic_access::matches(token, _state, row->second.value->generation);
}

std::shared_ptr<pubsub_partial::registration> pubsub_partial::require(const pubsub::partial_topic& token) const {
   const auto lock = std::scoped_lock{_state->mutex};
   const auto row = _state->topics.find(token.subject().value);
   if (_state->closed || row == _state->topics.end() ||
       !partial_topic_access::matches(token, _state, row->second.value->generation)) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "Partial token is stale or belongs to another node");
   }
   return row->second.value;
}

std::shared_ptr<pubsub_partial::registration> pubsub_partial::close(const pubsub::topic& subject) {
   const auto lock = std::scoped_lock{_state->mutex};
   const auto row = _state->topics.find(subject.value);
   if (row == _state->topics.end()) { return {}; }
   auto value = row->second.value;
   _state->groups -= row->second.groups.size();
   _state->group_bytes -= row->second.bytes;
   _state->topics.erase(row);
   return value;
}

std::shared_ptr<pubsub_partial::registration> pubsub_partial::close(const pubsub::partial_topic& token) {
   const auto lock = std::scoped_lock{_state->mutex};
   const auto row = _state->topics.find(token.subject().value);
   if (_state->closed || row == _state->topics.end() ||
       !partial_topic_access::matches(token, _state, row->second.value->generation)) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "Partial token is stale or belongs to another node");
   }
   auto value = row->second.value;
   _state->groups -= row->second.groups.size();
   _state->group_bytes -= row->second.bytes;
   _state->topics.erase(row);
   return value;
}

void pubsub_partial::stop() noexcept {
   auto retired = std::map<std::string, state::entry>{};
   {
      const auto lock = std::scoped_lock{_state->mutex};
      _state->closed = true;
      retired.swap(_state->topics);
      _state->groups = _state->group_bytes = 0;
   }
   for (const auto& [_, row] : retired) { row.value->stop.request_stop(); }
}

void pubsub_partial::advertise(const pubsub::partial_topic& token, std::vector<std::uint8_t> group,
                               const pubsub::limits& limits) {
   if (group.size() > limits.max_partial_group_id_size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "Partial group exceeds byte limit");
   }
   const auto value = require(token);
   const auto lock = std::scoped_lock{_state->mutex};
   const auto row = _state->topics.find(token.subject().value);
   if (row == _state->topics.end() || row->second.value != value) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "Partial registration replaced");
   }
   if (const auto found = row->second.groups.find(group); found != row->second.groups.end()) {
      found->second = limits.partial_group_ttl;
      return;
   }
   if (row->second.groups.size() >= limits.max_partial_groups_per_topic || _state->groups >= limits.max_partial_groups ||
       group.size() > limits.max_partial_group_bytes - _state->group_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "Partial advertisement registry is full");
   }
   const auto bytes = group.size();
   row->second.groups.emplace(std::move(group), limits.partial_group_ttl);
   row->second.bytes += bytes;
   _state->group_bytes += bytes;
   ++_state->groups;
}

void pubsub_partial::forget(const pubsub::partial_topic& token, const std::vector<std::uint8_t>& group) {
   const auto value = require(token);
   const auto lock = std::scoped_lock{_state->mutex};
   const auto row = _state->topics.find(token.subject().value);
   if (row == _state->topics.end() || row->second.value != value) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "Partial registration replaced");
   }
   if (row->second.groups.erase(group)) {
      row->second.bytes -= group.size();
      _state->group_bytes -= group.size();
      --_state->groups;
   }
}

void pubsub_partial::heartbeat() noexcept {
   const auto lock = std::scoped_lock{_state->mutex};
   for (auto& [_, row] : _state->topics) {
      for (auto group = row.groups.begin(); group != row.groups.end();) {
         if (--group->second == 0) {
            row.bytes -= group->first.size();
            _state->group_bytes -= group->first.size();
            --_state->groups;
            group = row.groups.erase(group);
         } else { ++group; }
      }
   }
}

std::vector<std::shared_ptr<pubsub_partial::registration>> pubsub_partial::registrations() const {
   auto out = std::vector<std::shared_ptr<registration>>{};
   const auto lock = std::scoped_lock{_state->mutex};
   for (const auto& [_, row] : _state->topics) {
      if (!row.groups.empty() && row.value->options.gossip && !_state->active_gossip.contains(row.value->token.subject().value)) {
         out.push_back(row.value);
      }
   }
   return out;
}

pubsub_partial::callback::callback(std::shared_ptr<state> owner, std::shared_ptr<registration> registration,
                                   std::size_t bytes, bool gossip)
    : _owner(std::move(owner)), _registration(std::move(registration)), _bytes(bytes), _gossip(gossip) {}

pubsub_partial::callback::~callback() {
   if (!_armed) { return; }
   std::vector<std::vector<std::uint8_t>>{}.swap(_groups);
   const auto lock = std::scoped_lock{_owner->mutex};
   --_owner->callbacks;
   _owner->callback_bytes -= _bytes;
   if (_gossip) { _owner->active_gossip.erase(_registration->token.subject().value); }
}

std::vector<std::vector<std::uint8_t>> pubsub_partial::callback::take_groups() noexcept { return std::move(_groups); }

std::shared_ptr<pubsub_partial::callback> pubsub_partial::admit(const std::shared_ptr<registration>& value,
                                                              std::size_t bytes, bool gossip, const pubsub::limits& limits) {
   auto lease = std::shared_ptr<callback>{new callback{_state, value, bytes, gossip}};
   const auto lock = std::scoped_lock{_state->mutex};
   const auto row = _state->topics.find(value->token.subject().value);
   if (_state->closed || row == _state->topics.end() || row->second.value != value ||
       (gossip && (_state->active_gossip.contains(value->token.subject().value) || row->second.groups.empty())) ||
       _state->callbacks >= limits.max_partial_callbacks ||
       bytes > limits.max_partial_callback_bytes - _state->callback_bytes) {
      ++_state->rejections;
      return {};
   }
   if (gossip) {
      const auto remaining = limits.max_partial_callback_bytes - _state->callback_bytes - bytes;
      if (row->second.bytes > remaining || row->second.groups.size() >
          (remaining - row->second.bytes) / sizeof(std::vector<std::uint8_t>)) {
         ++_state->rejections;
         return {};
      }
      bytes += row->second.bytes + row->second.groups.size() * sizeof(std::vector<std::uint8_t>);
      _state->active_gossip.insert(value->token.subject().value);
   }
   ++_state->callbacks;
   _state->callback_bytes += bytes;
   lease->_bytes = bytes;
   lease->_armed = true;
   // Admission and busy ownership are committed before copying the immutable callback snapshot.
   if (gossip) {
      lease->_groups.reserve(row->second.groups.size());
      for (const auto& [id, _] : row->second.groups) { lease->_groups.push_back(id); }
   }
   return lease;
}

void pubsub_partial::failed() noexcept { const auto lock = std::scoped_lock{_state->mutex}; ++_state->failures; }

void pubsub_partial::snapshot(pubsub::snapshot& out) const noexcept {
   const auto lock = std::scoped_lock{_state->mutex};
   out.partial_groups = _state->groups;
   out.partial_group_bytes = _state->group_bytes;
   out.partial_callbacks = _state->callbacks;
   out.partial_callback_bytes = _state->callback_bytes;
   out.partial_callback_failures = _state->failures;
   out.partial_callback_rejections = _state->rejections;
}

} // namespace forge::net::p2p::detail
