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

peer_store::impl::impl(peer_store::options options_value)
    : options_(std::move(options_value)), persistence_(options_.persistence) {
   validate(options_);
   if (!persistence_) {
      persistence_ = peer_store::make_memory_persistence();
      options_.persistence = persistence_;
   }
}

boost::asio::awaitable<void> peer_store::impl::async_upsert_rendezvous_owned(std::shared_ptr<impl> self,
                                                                             rendezvous::registration value) {
   co_await self->async_upsert_rendezvous(std::move(value));
}

boost::asio::awaitable<void> peer_store::impl::async_register_rendezvous_owned(std::shared_ptr<impl> self,
                                                                               rendezvous::registration value,
                                                                               std::size_t max_registrations_per_peer) {
   co_await self->async_register_rendezvous(std::move(value), max_registrations_per_peer);
}

boost::asio::awaitable<void> peer_store::impl::async_remove_rendezvous_owned(std::shared_ptr<impl> self, peer_id peer,
                                                                             std::string namespace_name) {
   co_await self->async_remove_rendezvous(std::move(peer), std::move(namespace_name));
}

boost::asio::awaitable<void> peer_store::impl::async_hydrate_owned(std::shared_ptr<impl> self) {
   co_await self->async_hydrate();
}

boost::asio::awaitable<peer_store::prune_result>
peer_store::impl::async_prune_expired_owned(std::shared_ptr<impl> self, std::chrono::system_clock::time_point now) {
   co_return co_await self->async_prune_expired(now);
}

boost::asio::awaitable<void> peer_store::impl::async_flush_owned(std::shared_ptr<impl> self) {
   co_await self->async_flush();
}

boost::asio::awaitable<void> peer_store::impl::async_close_owned(std::shared_ptr<impl> self) {
   co_await self->async_close();
}

peer_store::impl::persistence_admission::persistence_admission(peer_store::impl* owner) noexcept : owner_(owner) {}

peer_store::impl::persistence_admission::~persistence_admission() {
   if (owner_) {
      owner_->release_persistence_admission();
   }
}

peer_store::impl::persistence_admission::persistence_admission(persistence_admission&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)) {}

peer_store::impl::persistence_admission&
peer_store::impl::persistence_admission::operator=(persistence_admission&& other) noexcept {
   if (this == &other) {
      return *this;
   }
   if (owner_) {
      owner_->release_persistence_admission();
   }
   owner_ = std::exchange(other.owner_, nullptr);
   return *this;
}

peer_store::impl::close_admission::close_admission(peer_store::impl* owner) noexcept : owner_(owner) {}

peer_store::impl::close_admission::~close_admission() {
   if (owner_) {
      owner_->release_close_admission();
   }
}

peer_store::impl::close_admission::close_admission(close_admission&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)) {}

peer_store::impl::close_admission& peer_store::impl::close_admission::operator=(close_admission&& other) noexcept {
   if (this == &other) {
      return *this;
   }
   if (owner_) {
      owner_->release_close_admission();
   }
   owner_ = std::exchange(other.owner_, nullptr);
   return *this;
}

peer_store::impl::peer_mutation_stage::peer_mutation_stage(peer_store::impl* owner,
                                                           std::map<peer_id, peer_mutation> updates)
    : owner_(owner) {
   keys_.reserve(updates.size());
   for (const auto& [peer, _] : updates) {
      keys_.push_back(peer);
   }
   for (const auto& peer : keys_) {
      auto previous = owner_->pending_peer_mutations_.extract(peer);
      if (!previous.empty()) {
         previous_.insert(std::move(previous));
      }
   }
   while (!updates.empty()) {
      auto update = updates.extract(updates.begin());
      owner_->pending_peer_mutations_.insert(std::move(update));
   }
}

peer_store::impl::peer_mutation_stage::~peer_mutation_stage() {
   if (!owner_) {
      return;
   }
   for (const auto& peer : keys_) {
      static_cast<void>(owner_->pending_peer_mutations_.extract(peer));
   }
   while (!previous_.empty()) {
      auto previous = previous_.extract(previous_.begin());
      owner_->pending_peer_mutations_.insert(std::move(previous));
   }
}

void peer_store::impl::peer_mutation_stage::commit() noexcept {
   owner_ = nullptr;
}

void peer_store::impl::ensure_open_locked() const {
   if (closing_ || closed_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "peer store is closing or closed");
   }
}

std::size_t peer_store::impl::queued_unique_count_locked() const {
   auto count = in_flight_peer_mutations_.size();
   for (const auto& [peer, _] : pending_peer_mutations_) {
      if (!in_flight_peer_mutations_.contains(peer)) {
         ++count;
      }
   }
   return count;
}

void peer_store::impl::ensure_peer_mutation_capacity_locked(const std::vector<peer_id>& peers) const {
   auto additional = std::size_t{};
   auto unique = std::set<peer_id>{};
   for (const auto& peer : peers) {
      if (!unique.insert(peer).second || pending_peer_mutations_.contains(peer) ||
          in_flight_peer_mutations_.contains(peer)) {
         continue;
      }
      ++additional;
   }
   if (queued_unique_count_locked() + additional > options_.max_pending) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "peer store durable mutation queue is full");
   }
}

} // namespace forge::net::p2p
