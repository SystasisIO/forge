module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>

module forge.net.p2p.node;

import :lifecycle_stop_listener;

import forge.exceptions;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.multiformats.multiaddr;
import forge.net.p2p.exceptions;
import forge.net.p2p.negotiation;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/node_impl.hxx"
#include "details/peer_failure.hxx"

namespace forge::net::p2p {

namespace asio = boost::asio;

[[nodiscard]] exceptions::code p2p_code(const forge::exceptions::base& error);
[[nodiscard]] bool is_orderly_stream_close(const forge::exceptions::base& error) noexcept;

boost::asio::awaitable<std::vector<std::uint8_t>> async_read_length_delimited(forge::net::p2p::stream& stream,
                                                                              std::vector<std::uint8_t>& buffer,
                                                                              std::size_t max_payload_size);

[[nodiscard]] bool same_pubsub_message(const pubsub::message& left, const pubsub::message& right) noexcept {
   return left.from == right.from && left.data == right.data && left.seqno == right.seqno &&
          left.subject == right.subject;
}

[[nodiscard]] std::chrono::milliseconds validation_retry_delay(const pubsub::limits& limits, std::size_t ordinal) {
   auto delay = limits.validation_retry_initial_delay;
   for (auto attempt = std::size_t{1}; attempt < ordinal && delay < limits.validation_retry_max_delay; ++attempt) {
      if (delay > limits.validation_retry_max_delay / 2) {
         return limits.validation_retry_max_delay;
      }
      delay *= 2;
   }
   return std::min(delay, limits.validation_retry_max_delay);
}

void node::impl::finish_pubsub_validation(const peer_id& peer) {
   auto lock = std::scoped_lock{mutex};
   if (pubsub_value.active_validations > 0) {
      --pubsub_value.active_validations;
   }
   if (auto it = pubsub_value.active_validations_by_peer.find(peer);
       it != pubsub_value.active_validations_by_peer.end()) {
      if (it->second > 1) {
         --it->second;
      } else {
         pubsub_value.active_validations_by_peer.erase(it);
      }
   }
}

node::impl::pubsub_state::claim node::impl::claim_pubsub_message(const peer_id& peer, const std::string& key,
                                                                 const pubsub::message& value,
                                                                 bool requires_validation) {
   auto lock = std::scoped_lock{mutex};
   const auto begin_validation = [&](pubsub_state::validation& validation) {
      if (!requires_validation) {
         validation.state = pubsub_state::validation::status::accepted;
         return pubsub_state::claim_status::claimed;
      }
      if (pubsub_value.active_validations >= options.limits.pubsub.limits.max_validation_queue) {
         ++metrics_value.backpressure_rejections;
         const auto ordinal = std::max(validation.attempts, validation.redeliveries + 1);
         const auto retry_after =
             std::chrono::steady_clock::now() + validation_retry_delay(options.limits.pubsub.limits, ordinal);
         validation.state = pubsub_state::validation::status::retryable;
         validation.retry_after = retry_after;
         validation.request_after = retry_after;
         return pubsub_state::claim_status::backpressured;
      }
      validation.state = pubsub_state::validation::status::in_progress;
      ++validation.attempts;
      validation.requests = 0;
      ++pubsub_value.active_validations;
      ++pubsub_value.active_validations_by_peer[peer];
      return pubsub_state::claim_status::claimed;
   };

   const auto cached = pubsub_value.cache.find(key);
   if (cached == pubsub_value.cache.end()) {
      const auto generation = pubsub_value.next_validation_generation++;
      pubsub_value.cache.emplace(key, value);
      pubsub_value.history.push_back(key);
      const auto validation = pubsub_value.validations
                                  .emplace(key,
                                           pubsub_state::validation{
                                               .source = peer,
                                               .generation = generation,
                                           })
                                  .first;
      const auto status = begin_validation(validation->second);
      prune_pubsub_cache_locked();
      return pubsub_state::claim{
          .status = status,
          .generation = generation,
      };
   }

   const auto validation = pubsub_value.validations.find(key);
   if (!same_pubsub_message(cached->second, value)) {
      return pubsub_state::claim{.status = pubsub_state::claim_status::invalid};
   }
   if (validation != pubsub_value.validations.end() &&
       validation->second.state == pubsub_state::validation::status::retryable &&
       std::chrono::steady_clock::now() >= validation->second.retry_after) {
      if (validation->second.redeliveries >= options.limits.pubsub.limits.max_validation_redeliveries) {
         validation->second.state = pubsub_state::validation::status::ignored;
         validation->second.retry_after = {};
         validation->second.request_after = {};
         return pubsub_state::claim{.status = pubsub_state::claim_status::duplicate};
      }
      ++validation->second.redeliveries;
      validation->second.state = pubsub_state::validation::status::claimed;
      validation->second.source = peer;
      validation->second.retry_after = {};
      validation->second.request_after = {};
      const auto status = begin_validation(validation->second);
      prune_pubsub_cache_locked();
      return pubsub_state::claim{
          .status = status,
          .generation = validation->second.generation,
      };
   }

   ++pubsub_value.scores[peer].duplicate_messages;
   ++metrics_value.pubsub_duplicates;
   return pubsub_state::claim{.status = pubsub_state::claim_status::duplicate};
}

bool node::impl::complete_pubsub_message(const std::string& key, std::uint64_t generation,
                                         pubsub::validation_result result) {
   auto lock = std::scoped_lock{mutex};
   const auto found = pubsub_value.validations.find(key);
   if (found == pubsub_value.validations.end() || found->second.generation != generation) {
      return false;
   }
   switch (result) {
   case pubsub::validation_result::accept:
      found->second.state = pubsub_state::validation::status::accepted;
      found->second.retry_after = {};
      found->second.request_after = {};
      found->second.requests = 0;
      break;
   case pubsub::validation_result::reject:
      found->second.state = pubsub_state::validation::status::rejected;
      found->second.retry_after = {};
      found->second.request_after = {};
      found->second.requests = 0;
      break;
   case pubsub::validation_result::ignore:
      found->second.state = pubsub_state::validation::status::ignored;
      found->second.retry_after = {};
      found->second.request_after = {};
      found->second.requests = 0;
      break;
   case pubsub::validation_result::retry:
      return false;
   }
   prune_pubsub_cache_locked();
   return true;
}

void node::impl::defer_pubsub_message(const std::string& key, std::uint64_t generation) {
   auto lock = std::scoped_lock{mutex};
   const auto found = pubsub_value.validations.find(key);
   if (found == pubsub_value.validations.end() || found->second.generation != generation) {
      return;
   }
   if (found->second.attempts >= options.limits.pubsub.limits.max_validation_attempts ||
       found->second.redeliveries >= options.limits.pubsub.limits.max_validation_redeliveries) {
      found->second.state = pubsub_state::validation::status::ignored;
      found->second.retry_after = {};
      found->second.request_after = {};
      found->second.requests = 0;
      prune_pubsub_cache_locked();
      return;
   }

   const auto ordinal = std::max(found->second.attempts, found->second.redeliveries + 1);
   const auto retry_after =
       std::chrono::steady_clock::now() + validation_retry_delay(options.limits.pubsub.limits, ordinal);
   found->second.state = pubsub_state::validation::status::retryable;
   found->second.retry_after = retry_after;
   found->second.request_after = retry_after;
   prune_pubsub_cache_locked();
}

bool node::impl::should_request_pubsub_message_locked(const std::string& key, const peer_id& source,
                                                      std::chrono::steady_clock::time_point now) {
   if (!pubsub_value.cache.contains(key)) {
      return true;
   }
   const auto validation = pubsub_value.validations.find(key);
   if (validation == pubsub_value.validations.end() ||
       validation->second.state != pubsub_state::validation::status::retryable || validation->second.source != source ||
       now < validation->second.retry_after || now < validation->second.request_after) {
      return false;
   }
   if (validation->second.requests >= options.limits.pubsub.limits.max_validation_requests) {
      validation->second.state = pubsub_state::validation::status::ignored;
      validation->second.retry_after = {};
      validation->second.request_after = {};
      return false;
   }

   ++validation->second.requests;
   auto delay = options.limits.pubsub.limits.validation_retry_initial_delay;
   const auto maximum = options.limits.pubsub.limits.validation_retry_max_delay;
   for (auto request = std::size_t{1}; request < validation->second.requests && delay < maximum; ++request) {
      if (delay > maximum / 2) {
         delay = maximum;
      } else {
         delay *= 2;
      }
   }
   validation->second.request_after =
       now + std::max(options.limits.pubsub.limits.heartbeat_interval, std::min(delay, maximum));
   return true;
}

bool node::impl::can_serve_pubsub_message_locked(const std::string& key) const {
   const auto validation = pubsub_value.validations.find(key);
   return validation != pubsub_value.validations.end() &&
          validation->second.state == pubsub_state::validation::status::accepted;
}

void node::impl::remember_local_pubsub_message_locked(const std::string& key, pubsub::message value) {
   if (!pubsub_value.cache.contains(key)) {
      pubsub_value.history.push_back(key);
   }
   pubsub_value.cache[key] = std::move(value);
   pubsub_value.validations[key] = pubsub_state::validation{
       .state = pubsub_state::validation::status::accepted,
       .generation = pubsub_value.next_validation_generation++,
   };
   prune_pubsub_cache_locked();
}

void node::impl::prune_pubsub_cache_locked() {
   const auto max_cached = std::max<std::size_t>(
       options.limits.pubsub.limits.history_length * options.limits.pubsub.limits.max_messages, 1);
   while (pubsub_value.history.size() > max_cached) {
      auto evicted = false;
      for (auto history = pubsub_value.history.begin(); history != pubsub_value.history.end(); ++history) {
         const auto validation = pubsub_value.validations.find(*history);
         if (validation != pubsub_value.validations.end() &&
             validation->second.state == pubsub_state::validation::status::in_progress) {
            continue;
         }
         pubsub_value.cache.erase(*history);
         pubsub_value.validations.erase(*history);
         pubsub_value.history.erase(history);
         evicted = true;
         break;
      }
      if (!evicted) {
         break;
      }
   }
}

} // namespace forge::net::p2p
