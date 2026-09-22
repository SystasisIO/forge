module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.net.p2p.peer_store;

import forge.asio.gate;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.scoring;

#include "details/peer_store_impl.hxx"

namespace forge::net::p2p {

namespace {

[[nodiscard]] bool has_endpoint_source(const peer_store::endpoint_sources& sources) noexcept {
   return sources.learned || sources.identify_unsigned || sources.identify_signed;
}

void refresh_endpoint_score(peer_store::endpoint_record& endpoint) {
   endpoint.score = score_path(path::observation{
       .kind = endpoint.kind,
       .latency = endpoint.last_latency,
       .failures = endpoint.failures,
       .successes = endpoint.successes,
       .last_success = endpoint.successes > 0 && endpoint.failures == 0,
   });
}

void refresh_record_score(peer_store::record& record, path::kind kind, bool last_success) {
   record.score = score_path(path::observation{
       .kind = kind,
       .latency = record.last_latency,
       .failures = record.failures,
       .successes = record.successes,
       .last_success = last_success,
   });
}

void normalize_for_storage(peer_store::record& value) {
   if (value.discovered_by == discovery::source::mdns) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "mDNS observations cannot be persisted");
   }
   for (auto& endpoint : value.endpoints) {
      refresh_endpoint_score(endpoint);
   }
   const auto kind = value.endpoints.empty() ? path::kind::direct : value.endpoints.front().kind;
   refresh_record_score(value, kind, value.successes > 0);
}

void add_peer_record_bytes(std::size_t& total, std::size_t size, std::size_t maximum) {
   if (size > maximum - total) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P peer record exceeds byte limit");
   }
   total += size;
}

void validate_peer_record(const peer_store::record& value, const peer_store::options& options) {
   if (value.endpoints.size() > options.max_endpoints_per_peer ||
       value.protocols.size() > options.max_protocols_per_peer ||
       value.relay_reservations.size() > options.max_relay_reservations_per_peer) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P peer record exceeds collection limit");
   }

   auto bytes = std::size_t{};
   const auto add = [&](std::size_t size) { add_peer_record_bytes(bytes, size, options.max_peer_record_bytes); };
   add(value.protocol_version.size());
   add(value.agent_version.size());
   add(value.public_key.size());
   add(value.signed_peer_record.size());
   for (const auto& protocol : value.protocols) {
      add(protocol.value.size());
   }
   for (const auto& endpoint : value.endpoints) {
      if (!has_endpoint_source(endpoint.sources)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P endpoint record has no provenance");
      }
      add(endpoint.address.to_string().size());
   }
   if (value.observed_endpoint) {
      add(value.observed_endpoint->to_string().size());
   }
   for (const auto& relay : value.relay_reservations) {
      if (relay.endpoints.size() > options.max_relay_endpoints_per_reservation) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P peer relay reservation exceeds endpoint limit");
      }
      add(relay.voucher.size());
      for (const auto& endpoint : relay.endpoints) {
         add(endpoint.to_string().size());
      }
   }
   if (!value.public_key.empty()) {
      validate_public_key(decode_public_key(value.public_key), value.peer);
   }
}

void validate_rendezvous_record(const rendezvous::registration& value, const peer_store::options& options) {
   if (value.endpoints.size() > options.max_endpoints_per_peer) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P Rendezvous record exceeds endpoint limit");
   }

   auto bytes = std::size_t{};
   const auto add = [&](std::size_t size) { add_peer_record_bytes(bytes, size, options.max_peer_record_bytes); };
   add(value.namespace_name.size());
   add(value.peer.value.size());
   add(value.signed_peer_record.size());
   for (const auto& endpoint : value.endpoints) {
      add(endpoint.to_string().size());
   }
}

[[nodiscard]] std::string current_failure_message() {
   try {
      throw;
   } catch (const std::exception& error) {
      return error.what();
   } catch (...) {
      return "unknown persistence failure";
   }
}

[[nodiscard]] std::string durability_failure_message(const peer_store::apply_result& result) {
   return result.durability_failure.empty() ? "persistence commit completed without durable acknowledgement"
                                            : result.durability_failure;
}

[[noreturn]] void throw_durability_uncertain(const peer_store::apply_result& result) {
   FORGE_THROW_EXCEPTION(exceptions::durability_uncertain, "peer state durability could not be confirmed",
                         forge::exceptions::ctx("reason", durability_failure_message(result)));
}

} // namespace

void peer_store::impl::mark_persistence_failure_locked(std::string message) {
   degraded_ = true;
   ++persistence_failures_;
   last_failure_ = std::move(message);
}

void peer_store::impl::mark_durability_uncertain_locked(std::string message) {
   durability_uncertain_ = true;
   mark_persistence_failure_locked(std::move(message));
}

void peer_store::impl::mark_persistence_healthy_locked(bool durability_confirmed) {
   if (durability_confirmed) {
      durability_uncertain_ = false;
   }
   if (durability_uncertain_) {
      return;
   }
   degraded_ = false;
   last_failure_.clear();
}

std::pair<peer_store::mutation_batch, std::map<peer_id, peer_store::impl::peer_mutation>>
peer_store::impl::take_pending_batch_locked() {
   auto batch = peer_store::mutation_batch{};
   auto mutations = std::move(pending_peer_mutations_);
   pending_peer_mutations_.clear();
   batch.peer_upserts.reserve(mutations.size());
   batch.peer_removals.reserve(mutations.size());
   for (const auto& [peer, mutation] : mutations) {
      if (mutation.value) {
         batch.peer_upserts.push_back(*mutation.value);
      } else {
         batch.peer_removals.push_back(peer);
      }
   }
   in_flight_peer_mutations_ = mutations;
   return {std::move(batch), std::move(mutations)};
}

void peer_store::impl::complete_peer_mutations_locked(const std::map<peer_id, peer_mutation>& values) {
   for (const auto& [peer, _] : values) {
      in_flight_peer_mutations_.erase(peer);
   }
}

void peer_store::impl::requeue_peer_mutations_locked(const std::map<peer_id, peer_mutation>& values) {
   for (const auto& [peer, mutation] : values) {
      if (!pending_peer_mutations_.contains(peer)) {
         pending_peer_mutations_.emplace(peer, mutation);
      }
      in_flight_peer_mutations_.erase(peer);
   }
}

boost::asio::awaitable<void> peer_store::impl::apply_pending_locked_gate(bool flush_backend) {
   auto batch = peer_store::mutation_batch{};
   auto mutations = std::map<peer_id, peer_mutation>{};
   {
      auto lock = std::scoped_lock{mutex_};
      std::tie(batch, mutations) = take_pending_batch_locked();
   }

   if (!mutations.empty()) {
      auto result = peer_store::apply_result{};
      try {
         result = co_await persistence_->async_apply(batch);
      } catch (...) {
         auto lock = std::scoped_lock{mutex_};
         requeue_peer_mutations_locked(mutations);
         mark_persistence_failure_locked(current_failure_message());
         throw;
      }
      {
         auto lock = std::scoped_lock{mutex_};
         complete_peer_mutations_locked(mutations);
         if (result.durability_confirmed) {
            mark_persistence_healthy_locked(batch.durable);
         } else {
            mark_durability_uncertain_locked(durability_failure_message(result));
         }
      }
      if (!result.durability_confirmed) {
         throw_durability_uncertain(result);
      }
   }

   if (!flush_backend) {
      co_return;
   }
   try {
      co_await persistence_->async_flush();
   } catch (...) {
      auto result = peer_store::apply_result{
          .durability_confirmed = false,
          .durability_failure = current_failure_message(),
      };
      {
         auto lock = std::scoped_lock{mutex_};
         mark_durability_uncertain_locked(durability_failure_message(result));
      }
      throw_durability_uncertain(result);
   }
   auto lock = std::scoped_lock{mutex_};
   mark_persistence_healthy_locked(true);
}

peer_store::impl::persistence_admission peer_store::impl::admit_persistence_operation() {
   auto lock = std::scoped_lock{mutex_};
   ensure_open_locked();
   if (persistence_admissions_ >= options_.max_persistence_waiters) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "peer store persistence waiter limit reached");
   }
   ++persistence_admissions_;
   return persistence_admission{this};
}

void peer_store::impl::release_persistence_admission() noexcept {
   auto drainers = std::map<const void*, std::function<void()>>{};
   {
      auto lock = std::scoped_lock{mutex_};
      if (persistence_admissions_ > 0) {
         --persistence_admissions_;
      }
      if (persistence_admissions_ == 0) {
         drainers.swap(persistence_admission_drainers_);
      }
   }
   for (const auto& [_, drain] : drainers) {
      drain();
   }
}

boost::asio::awaitable<void> peer_store::impl::wait_for_persistence_admissions() {
   while (true) {
      auto timer = std::make_shared<boost::asio::steady_timer>(co_await boost::asio::this_coro::executor);
      timer->expires_at(std::chrono::steady_clock::time_point::max());
      const auto* drainer_id = timer.get();
      {
         auto lock = std::scoped_lock{mutex_};
         if (persistence_admissions_ == 0) {
            co_return;
         }
         persistence_admission_drainers_.emplace(drainer_id, [weak = std::weak_ptr{timer}] {
            if (auto current = weak.lock()) {
               boost::asio::post(current->get_executor(), [current] { current->cancel(); });
            }
         });
      }
      auto error = boost::system::error_code{};
      co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
      auto drained = false;
      {
         auto lock = std::scoped_lock{mutex_};
         persistence_admission_drainers_.erase(drainer_id);
         drained = persistence_admissions_ == 0;
      }
      if (drained) {
         co_return;
      }
      if (error == boost::asio::error::operation_aborted) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "peer store close was canceled while draining operations");
      }
      if (error) {
         FORGE_THROW_EXCEPTION(exceptions::internal, "peer store close admission wait failed",
                               forge::exceptions::ctx("error", error.message()));
      }
   }
}

std::optional<peer_store::impl::close_admission> peer_store::impl::admit_close_operation() {
   auto lock = std::scoped_lock{mutex_};
   if (closed_) {
      return std::nullopt;
   }
   if (close_waiters_ >= options_.max_persistence_waiters) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "peer store close waiter limit reached");
   }
   closing_ = true;
   ++close_waiters_;
   return close_admission{this};
}

void peer_store::impl::release_close_admission() noexcept {
   auto lock = std::scoped_lock{mutex_};
   if (close_waiters_ > 0) {
      --close_waiters_;
   }
}

void peer_store::impl::hydrate_page_locked(peer_store::hydration_page page) {
   for (const auto& value : page.peers) {
      validate_peer_record(value, options_);
   }
   for (const auto& value : page.rendezvous_registrations) {
      validate_rendezvous_record(value, options_);
   }
   rendezvous_sequence_ = std::max(rendezvous_sequence_, page.rendezvous_sequence_high_watermark);
   auto removals = std::map<peer_id, peer_mutation>{};
   auto removal_peers = std::vector<peer_id>{};
   for (const auto& value : page.peers) {
      if (!pending_peer_mutations_.contains(value.peer) && !in_flight_peer_mutations_.contains(value.peer) &&
          !records_.contains(value.peer) && records_.size() >= options_.max_peers) {
         removals.emplace(value.peer, peer_mutation{.value = std::nullopt});
         removal_peers.push_back(value.peer);
      }
   }
   ensure_peer_mutation_capacity_locked(removal_peers);
   auto removal_stage = peer_mutation_stage{this, std::move(removals)};
   for (auto& value : page.peers) {
      if (pending_peer_mutations_.contains(value.peer) || in_flight_peer_mutations_.contains(value.peer)) {
         continue;
      }
      if (!records_.contains(value.peer) && records_.size() >= options_.max_peers) {
         continue;
      }
      normalize_for_storage(value);
      (void)store_peer_operational(std::move(value));
   }
   for (auto& value : page.rendezvous_registrations) {
      rendezvous_sequence_ = std::max(rendezvous_sequence_, value.sequence);
      const auto key = rendezvous_map_key{value.namespace_name, value.peer};
      if (rendezvous_.contains(key) || rendezvous_.size() < options_.max_rendezvous) {
         store_rendezvous_operational(std::move(value));
      }
   }
   removal_stage.commit();
}

boost::asio::awaitable<void> peer_store::impl::async_hydrate() {
   auto admission = admit_persistence_operation();
   auto ticket = co_await persistence_gate_.acquire();

   const auto hydrate_kind = [this](peer_store::hydration_kind kind,
                                    std::size_t maximum) -> boost::asio::awaitable<void> {
      auto cursor = std::optional<std::vector<std::byte>>{};
      auto remaining = maximum;
      while (remaining > 0) {
         const auto limit = std::min(remaining, options_.hydration_page_limit);
         auto page = peer_store::hydration_page{};
         try {
            page = co_await persistence_->async_hydrate(
                peer_store::hydration_request{.kind = kind, .cursor = cursor, .limit = limit});
         } catch (...) {
            auto lock = std::scoped_lock{mutex_};
            mark_persistence_failure_locked(current_failure_message());
            throw;
         }

         const auto page_size = page.peers.size() + page.rendezvous_registrations.size();
         const auto wrong_kind =
             (kind != peer_store::hydration_kind::peers && !page.peers.empty()) ||
             (kind != peer_store::hydration_kind::rendezvous && !page.rendezvous_registrations.empty());
         if (page_size > limit || wrong_kind || (page.cursor && (page.cursor == cursor || page_size == 0))) {
            auto lock = std::scoped_lock{mutex_};
            mark_persistence_failure_locked("persistence returned an invalid hydration page");
            FORGE_THROW_EXCEPTION(exceptions::internal, "persistence returned an invalid hydration page");
         }

         const auto next_cursor = page.cursor;
         try {
            auto lock = std::scoped_lock{mutex_};
            hydrate_page_locked(std::move(page));
         } catch (...) {
            auto lock = std::scoped_lock{mutex_};
            mark_persistence_failure_locked(current_failure_message());
            throw;
         }
         remaining -= page_size;
         if (!next_cursor) {
            co_return;
         }
         cursor = next_cursor;
      }
   };

   co_await hydrate_kind(peer_store::hydration_kind::peers, options_.max_peers);
   co_await hydrate_kind(peer_store::hydration_kind::rendezvous, options_.max_rendezvous);

   auto lock = std::scoped_lock{mutex_};
   mark_persistence_healthy_locked();
}

void peer_store::impl::prune_operational_locked(std::chrono::system_clock::time_point,
                                                const peer_store::prune_result& result) {
   for (const auto& peer : result.peers) {
      if (pending_peer_mutations_.contains(peer) || in_flight_peer_mutations_.contains(peer)) {
         continue;
      }
      erase_peer_operational(peer);
   }
   for (const auto& value : result.rendezvous_registrations) {
      const auto key = rendezvous_map_key{value.namespace_name, value.peer};
      erase_rendezvous_operational(key);
   }
}

boost::asio::awaitable<peer_store::prune_result>
peer_store::impl::async_prune_expired(std::chrono::system_clock::time_point now) {
   auto admission = admit_persistence_operation();
   auto ticket = co_await persistence_gate_.acquire();

   co_await apply_pending_locked_gate(false);

   auto result = peer_store::prune_result{};
   try {
      result = co_await persistence_->async_prune_expired(now, options_.prune_page_limit);
   } catch (...) {
      auto lock = std::scoped_lock{mutex_};
      mark_persistence_failure_locked(current_failure_message());
      throw;
   }

   auto lock = std::scoped_lock{mutex_};
   prune_operational_locked(now, result);
   mark_persistence_healthy_locked();
   co_return result;
}

boost::asio::awaitable<void> peer_store::impl::async_flush() {
   auto admission = admit_persistence_operation();
   auto ticket = co_await persistence_gate_.acquire();
   co_await apply_pending_locked_gate(true);
}

boost::asio::awaitable<void> peer_store::impl::async_close() {
   auto admission = admit_close_operation();
   if (!admission) {
      co_return;
   }

   co_await wait_for_persistence_admissions();

   auto ticket = co_await persistence_gate_.acquire();

   {
      auto lock = std::scoped_lock{mutex_};
      if (closed_) {
         co_return;
      }
      closing_ = true;
   }

   try {
      while (true) {
         co_await apply_pending_locked_gate(false);
         auto lock = std::scoped_lock{mutex_};
         if (pending_peer_mutations_.empty()) {
            break;
         }
      }
   } catch (...) {
      throw;
   }

   try {
      co_await persistence_->async_flush();
   } catch (...) {
      auto result = peer_store::apply_result{
          .durability_confirmed = false,
          .durability_failure = current_failure_message(),
      };
      {
         auto lock = std::scoped_lock{mutex_};
         mark_durability_uncertain_locked(durability_failure_message(result));
      }
      throw_durability_uncertain(result);
   }
   {
      auto lock = std::scoped_lock{mutex_};
      mark_persistence_healthy_locked(true);
   }
   try {
      co_await persistence_->async_close();
   } catch (...) {
      auto lock = std::scoped_lock{mutex_};
      mark_persistence_failure_locked(current_failure_message());
      throw;
   }

   auto lock = std::scoped_lock{mutex_};
   mark_persistence_healthy_locked(true);
   closing_ = false;
   closed_ = true;
}

peer_store::persistence_status peer_store::impl::persistence_state() const {
   auto lock = std::scoped_lock{mutex_};
   return peer_store::persistence_status{
       .pending_peer_mutations = queued_unique_count_locked(),
       .failure_count = persistence_failures_,
       .degraded = degraded_,
       .closing = closing_,
       .closed = closed_,
       .last_failure = last_failure_,
   };
}

} // namespace forge::net::p2p
