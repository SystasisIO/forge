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
#include <random>
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
#include <boost/asio/ip/address.hpp>
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
#include "details/pubsub_peer_score.hxx"
#include "details/pubsub_router.hxx"

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
    const pubsub::message& value, bool requires_validation, const std::shared_ptr<session_state>& session) {
   auto lock = std::scoped_lock{mutex};
   if (session && (session->info.remote_peer != peer || !pubsub_session_live_locked(session))) {
      return {};
   }
   // A request may have been staged after verified arrival released the inbound lock.
   pubsub_value.router->fulfill(key);
   const auto now = std::chrono::steady_clock::now();
   const auto id = std::span{reinterpret_cast<const std::uint8_t*>(key.data()), key.size()};
   const auto begin_validation = [&](pubsub_state::validation& validation, std::size_t redeliveries) {
      if (requires_validation && pubsub_value.active_validations >= options.limits.pubsub.limits.max_validation_queue) {
         ++metrics_value.backpressure_rejections;
         const auto ordinal = std::max(validation.attempts, redeliveries + 1);
         const auto retry_after =
             std::chrono::steady_clock::now() + validation_retry_delay(options.limits.pubsub.limits, ordinal);
         validation.state = pubsub_state::validation::status::retryable;
         validation.retry_after = retry_after;
         validation.request_after = retry_after;
         return pubsub_state::claim_status::backpressured;
      }
      // Prepare every allocating admission resource before publishing either counter or active state.
      const auto [admission, inserted] = pubsub_value.active_validations_by_peer.try_emplace(peer, 0);
      try {
         if (pubsub_value.scoring && validation.score_generation == 0) {
            const auto ticket = pubsub_value.scoring->validation_start(peer, value.subject, id, now);
            if (!ticket) {
               if (inserted) {
                  pubsub_value.active_validations_by_peer.erase(admission);
               }
               ++metrics_value.backpressure_rejections;
               validation.state = pubsub_state::validation::status::retryable;
               validation.retry_after = now + options.limits.pubsub.limits.validation_retry_initial_delay;
               validation.request_after = validation.retry_after;
               return pubsub_state::claim_status::backpressured;
            }
            validation.score_generation = ticket->generation;
         }
      } catch (...) {
         if (inserted) {
            pubsub_value.active_validations_by_peer.erase(admission);
         }
         throw;
      }
      validation.state = requires_validation ? pubsub_state::validation::status::in_progress
                                            : pubsub_state::validation::status::claimed;
      ++validation.attempts;
      validation.requests = 0;
      if (requires_validation) {
         ++pubsub_value.active_validations;
         ++admission->second;
      } else if (inserted) {
         pubsub_value.active_validations_by_peer.erase(admission);
      }
      return pubsub_state::claim_status::claimed;
   };

   const auto cached = pubsub_value.cache.find(key);
   if (cached == pubsub_value.cache.end()) {
      if (pubsub_value.scoring && pubsub_value.scoring->duplicate_delivery(peer, value.subject, id, now)) {
         ++metrics_value.pubsub_duplicates;
         if (const auto row = pubsub_value.scores.find(peer); row != pubsub_value.scores.end()) {
            ++row->second.duplicate_messages;
         }
         return pubsub_state::claim{.status = pubsub_state::claim_status::duplicate};
      }
      const auto capacity = options.limits.pubsub.limits.history_length * options.limits.pubsub.limits.max_messages;
      if (pubsub_value.cache.size() >= capacity) {
         ++metrics_value.backpressure_rejections;
         return pubsub_state::claim{.status = pubsub_state::claim_status::backpressured};
      }
      const auto generation = pubsub_value.next_validation_generation++;
      pubsub_value.cache.emplace(key, value);
      try {
         const auto validation = pubsub_value.validations.emplace(key, pubsub_state::validation{
            .source = peer, .generation = generation, .cache_epoch = pubsub_value.epoch}).first;
         try {
            pubsub_value.history.push_back(key);
         } catch (...) {
            pubsub_value.validations.erase(validation);
            throw;
         }
         try {
            return pubsub_state::claim{.status = begin_validation(validation->second, 0), .generation = generation};
         } catch (...) {
            pubsub_value.history.pop_back();
            pubsub_value.validations.erase(validation);
            throw;
         }
      } catch (...) {
         pubsub_value.cache.erase(key);
         throw;
      }
   }

   const auto validation = pubsub_value.validations.find(key);
   if (!same_pubsub_message(cached->second, value)) {
      return pubsub_state::claim{.status = pubsub_state::claim_status::invalid};
   }
   if (pubsub_value.scoring) {
      static_cast<void>(pubsub_value.scoring->duplicate_delivery(peer, value.subject, id, now));
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
      // Copy the sender before admission: neither a throwing copy nor admission allocation can
      // consume the retry or leave a claimed row without a validation worker.
      static_assert(std::is_nothrow_move_assignable_v<peer_id>);
      auto source = peer;
      const auto redeliveries = validation->second.redeliveries + 1;
      const auto status = begin_validation(validation->second, redeliveries);
      validation->second.source = std::move(source);
      validation->second.redeliveries = redeliveries;
      if (status == pubsub_state::claim_status::claimed) {
         validation->second.retry_after = {};
         validation->second.request_after = {};
      }
      return pubsub_state::claim{
          .status = status,
          .generation = validation->second.generation,
      };
   }

   if (const auto score = pubsub_value.scores.find(peer); score != pubsub_value.scores.end()) {
      ++score->second.duplicate_messages;
   }
   ++metrics_value.pubsub_duplicates;
   return pubsub_state::claim{.status = pubsub_state::claim_status::duplicate};
}

bool node::impl::complete_pubsub_message(const std::string& key, std::uint64_t generation,
    pubsub::validation_result result, const std::shared_ptr<session_state>& session) {
   auto lock = std::scoped_lock{mutex};
   const auto found = pubsub_value.validations.find(key);
   if (found == pubsub_value.validations.end() || found->second.generation != generation) {
      return false;
   }
   if (found->second.state != pubsub_state::validation::status::in_progress &&
       found->second.state != pubsub_state::validation::status::claimed) {
      return false;
   }
   if (session && found->second.source != session->info.remote_peer) {
      return false;
   }
   if (session && !pubsub_session_live_locked(session)) {
      // Retirement cannot attribute a late validator result to a reconnected peer lifetime.
      if (pubsub_value.scoring && found->second.score_generation != 0) {
         static_cast<void>(pubsub_value.scoring->validation_complete(
            std::span{reinterpret_cast<const std::uint8_t*>(key.data()), key.size()}, found->second.score_generation,
            pubsub::validation_result::ignore, std::chrono::steady_clock::now()));
      }
      found->second.state = pubsub_state::validation::status::ignored;
      found->second.retry_after = {};
      found->second.request_after = {};
      found->second.requests = 0;
      found->second.score_generation = 0;
      return false;
   }
   if (pubsub_value.scoring && found->second.score_generation != 0 &&
       !pubsub_value.scoring->validation_complete(
          std::span{reinterpret_cast<const std::uint8_t*>(key.data()), key.size()}, found->second.score_generation,
          result, std::chrono::steady_clock::now())) {
      // The native admission has finished even if its scoring ticket expired. Retire it neutrally
      // so normal cache expiry can reclaim the row; never recreate the expired scoring generation.
      found->second.state = pubsub_state::validation::status::ignored;
      found->second.retry_after = {};
      found->second.request_after = {};
      found->second.requests = 0;
      found->second.score_generation = 0;
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
   return true;
}

void node::impl::defer_pubsub_message(const std::string& key, std::uint64_t generation,
    const std::shared_ptr<session_state>& session) {
   auto lock = std::scoped_lock{mutex};
   const auto found = pubsub_value.validations.find(key);
   if (found == pubsub_value.validations.end() || found->second.generation != generation) {
      return;
   }
   if (found->second.state != pubsub_state::validation::status::in_progress &&
       found->second.state != pubsub_state::validation::status::claimed) {
      return;
   }
   if (session && found->second.source != session->info.remote_peer) {
      return;
   }
   if (session && !pubsub_session_live_locked(session)) {
      if (pubsub_value.scoring && found->second.score_generation != 0) {
         static_cast<void>(pubsub_value.scoring->validation_complete(
            std::span{reinterpret_cast<const std::uint8_t*>(key.data()), key.size()}, found->second.score_generation,
            pubsub::validation_result::ignore, std::chrono::steady_clock::now()));
      }
      found->second.state = pubsub_state::validation::status::ignored;
      found->second.retry_after = {};
      found->second.request_after = {};
      found->second.requests = 0;
      found->second.score_generation = 0;
      return;
   }
   if (found->second.attempts >= options.limits.pubsub.limits.max_validation_attempts ||
       found->second.redeliveries >= options.limits.pubsub.limits.max_validation_redeliveries) {
      if (pubsub_value.scoring) {
         static_cast<void>(pubsub_value.scoring->validation_complete(
            std::span{reinterpret_cast<const std::uint8_t*>(key.data()), key.size()}, found->second.score_generation,
            pubsub::validation_result::ignore, std::chrono::steady_clock::now()));
      }
      found->second.state = pubsub_state::validation::status::ignored;
      found->second.retry_after = {};
      found->second.request_after = {};
      found->second.requests = 0;
      return;
   }

   const auto ordinal = std::max(found->second.attempts, found->second.redeliveries + 1);
   const auto retry_after =
       std::chrono::steady_clock::now() + validation_retry_delay(options.limits.pubsub.limits, ordinal);
   found->second.state = pubsub_state::validation::status::retryable;
   found->second.retry_after = retry_after;
   found->second.request_after = retry_after;
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
   if (pubsub_value.cache.contains(key)) {
      return;
   }
   if (pubsub_value.cache.size() >= options.limits.pubsub.limits.history_length * options.limits.pubsub.limits.max_messages) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "GossipSub heartbeat cache is full");
   }
   pubsub_value.cache.emplace(key, std::move(value));
   try {
      pubsub_value.validations.emplace(key, pubsub_state::validation{
       .state = pubsub_state::validation::status::accepted,
       .generation = pubsub_value.next_validation_generation++,
       .cache_epoch = pubsub_value.epoch,
      });
      pubsub_value.history.push_back(key);
   } catch (...) {
      pubsub_value.validations.erase(key);
      pubsub_value.cache.erase(key);
      throw;
   }
}

void node::impl::prune_pubsub_cache_locked() {
   for (auto history = pubsub_value.history.begin(); history != pubsub_value.history.end();) {
         const auto validation = pubsub_value.validations.find(*history);
         if (validation != pubsub_value.validations.end() &&
             (validation->second.state == pubsub_state::validation::status::in_progress ||
              pubsub_value.epoch - validation->second.cache_epoch < options.limits.pubsub.limits.history_length)) {
            ++history;
            continue;
         }
         pubsub_value.cache.erase(*history);
         pubsub_value.validations.erase(*history);
         history = pubsub_value.history.erase(history);
   }
}

} // namespace forge::net::p2p
