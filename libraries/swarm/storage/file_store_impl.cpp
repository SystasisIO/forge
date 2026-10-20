module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/error.hpp>
#include <boost/system/system_error.hpp>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

module forge.swarm.storage.file_store;

import forge.asio.gate;
import forge.asio.notification;

#include "details/file_layout.hxx"
#include "details/file_store_impl.hxx"
#include "details/file_store_impl_writer.hxx"

namespace forge::swarm::storage {

file_store::impl::impl(options value) : config{std::move(value)} {
   if (!config.disk.valid() || !config.cpu.valid() || config.max_unfinished == 0U ||
       config.max_unfinished > core::max_unfinished_pieces || config.max_parallel_io == 0U ||
       config.max_parallel_io > 32U || config.max_io_size < core::block_size || config.max_io_size > 1024U * 1024U) {
      throw exceptions::invalid_options{"file store requires bounded caller-owned disk and CPU executors"};
   }
}
file_store::impl::~impl() = default;

file_store::impl::cancellation::cancellation(const std::shared_ptr<impl>& state, std::stop_token token)
   : caller{token, [value = source]() mutable { value.request_stop(); }},
     owner{state->stop.get_token(), [value = source]() mutable { value.request_stop(); }} {}

file_store::impl::maintenance_request::maintenance_request(std::shared_ptr<impl> value) : owner{std::move(value)} {
   const auto lock = std::scoped_lock{owner->mutex};
   if (owner->closing) { throw core::exceptions::storage_closed{"file store is closing"}; }
   if (owner->maintenance_waiters >= owner->config.max_parallel_io) {
      throw core::exceptions::storage_busy{"checkpoint admission bound reached"};
   }
   ++owner->maintenance_waiters;
}
file_store::impl::maintenance_request::~maintenance_request() {
   const auto lock = std::scoped_lock{owner->mutex};
   --owner->maintenance_waiters;
   owner->changed.notify();
}

std::uint32_t file_store::impl::ordinal(core::piece_id piece) const {
   if (piece.file >= config.manifest.files.size() || piece.index >=
       core::piece_count(config.manifest.files[piece.file].size, config.manifest.piece_size)) {
      throw core::exceptions::invalid_geometry{"piece is outside the descriptor"};
   }
   return offsets[piece.file] + piece.index;
}

void file_store::impl::changed_locked() noexcept { ++generation; changed.notify(); }

core::availability file_store::impl::snapshot() const {
   const auto lock = std::scoped_lock{mutex};
   auto value = core::availability{generation, durable_generation, published, verified, {}};
   for (const auto& [piece, state] : pending) { value.unfinished.push_back({piece, state.blocks}); }
   return value;
}

std::unique_ptr<core::store::writer> file_store::impl::begin(core::piece_id piece) {
   const auto index = ordinal(piece);
   const auto lock = std::scoped_lock{mutex};
   if (closing) { throw core::exceptions::storage_closed{"file store is closing"}; }
   if (published || maintenance || core::bitmap_test(verified, index) || readers.contains(piece)) {
      throw core::exceptions::storage_busy{"piece is verified, published or temporarily reserved"};
   }
   auto found = pending.find(piece);
   if (found == pending.end()) {
      if (pending.size() >= config.max_unfinished) { throw core::exceptions::storage_busy{"unfinished piece bound reached"}; }
      const auto length = core::piece_length(config.manifest, piece);
      pending.emplace(piece, pending_piece{0, false, false,
         std::vector<std::uint8_t>(((length + core::block_size - 1U) / core::block_size + 7U) / 8U)});
      found = pending.find(piece);
   }
   if (found->second.owned || found->second.busy) { throw core::exceptions::storage_busy{"piece already has a writer"}; }
   if (next_writer == std::numeric_limits<std::uint64_t>::max()) { throw exceptions::invalid_state{"writer generation exhausted"}; }
   const auto epoch = ++next_writer;
   auto result = std::make_unique<writer>(shared_from_this(), piece, epoch);
   found->second.generation = epoch;
   found->second.owned = true;
   changed_locked();
   return result;
}

void file_store::impl::release_writer(core::piece_id piece, std::uint64_t epoch) noexcept {
   const auto lock = std::scoped_lock{mutex};
   const auto found = pending.find(piece);
   if (found != pending.end() && found->second.generation == epoch) {
      found->second.owned = false;
      // A result already in flight may retire, but cannot advance this abandoned writer.
      found->second.generation = 0;
      changed_locked();
   }
}

void file_store::impl::set_verified_locked(core::piece_id piece, bool value) {
   const auto index = ordinal(piece);
   const auto bit = static_cast<std::uint8_t>(1U << (index % 8U));
   if (value) { verified[index / 8U] |= bit; }
   else { verified[index / 8U] &= static_cast<std::uint8_t>(~bit); }
}

void file_store::impl::discard(core::piece_id piece) {
   static_cast<void>(ordinal(piece));
   const auto lock = std::scoped_lock{mutex};
   if (closing) { throw core::exceptions::storage_closed{"file store is closing"}; }
   const auto found = pending.find(piece);
   if (published || maintenance || readers.contains(piece) ||
       (found != pending.end() && (found->second.busy || found->second.owned))) {
      throw core::exceptions::storage_busy{"piece has active ownership"};
   }
   pending.erase(piece);
   set_verified_locked(piece, false);
   changed_locked();
}

std::shared_ptr<void> file_store::impl::lease() {
   const auto self = shared_from_this();
   {
      const auto lock = std::scoped_lock{mutex};
      if (closing) { throw core::exceptions::storage_closed{"file store is closing"}; }
      ++leases;
   }
   return std::shared_ptr<void>{this, [self](void*) {
      const auto lock = std::scoped_lock{self->mutex};
      --self->leases;
      self->changed.notify();
   }};
}

void file_store::impl::request_stop() noexcept {
   {
      const auto lock = std::scoped_lock{mutex};
      closing = true;
      changed_locked();
   }
   stop.request_stop();
}

void file_store::impl::check_stop(std::stop_token token) const {
   if (token.stop_requested() || stop.stop_requested()) {
      throw forge::asio::exceptions::canceled{"file store operation was canceled"};
   }
}

file_store::impl::operation::operation(std::shared_ptr<impl> value, core::piece_id key, bool write,
   std::uint64_t epoch, std::stop_token token)
   : owner{std::move(value)}, piece{key}, writing{write}, cancel{owner, token} {
   const auto index = owner->ordinal(piece);
   const auto lock = std::scoped_lock{owner->mutex};
   if (owner->closing) { throw core::exceptions::storage_closed{"file store is closing"}; }
   if (owner->maintenance || owner->active >= owner->config.max_parallel_io) {
      throw core::exceptions::storage_busy{"file store I/O admission bound reached"};
   }
   if (writing) {
      const auto found = owner->pending.find(piece);
      if (found == owner->pending.end() || !found->second.owned || found->second.generation != epoch) {
         throw core::exceptions::invalid_write{"piece writer is no longer current"};
      }
      if (found->second.busy) { throw core::exceptions::storage_busy{"piece writer has an operation in flight"}; }
      if (owner->next_operation == std::numeric_limits<std::uint64_t>::max()) {
         throw exceptions::invalid_state{"operation reservation exhausted"};
      }
      reservation = ++owner->next_operation;
      found->second.busy_generation = reservation;
      found->second.busy = true;
   } else {
      if (!core::bitmap_test(owner->verified, index)) { throw core::exceptions::unverified_piece{"piece is not verified"}; }
      ++owner->readers[piece];
   }
   ++owner->active;
}

file_store::impl::operation::~operation() {
   const auto lock = std::scoped_lock{owner->mutex};
   if (writing) {
      const auto found = owner->pending.find(piece);
      if (found != owner->pending.end() && found->second.busy_generation == reservation) {
         found->second.busy = false;
         found->second.busy_generation = 0;
      }
   } else {
      const auto found = owner->readers.find(piece);
      if (found != owner->readers.end() && --found->second == 0U) { owner->readers.erase(found); }
   }
   --owner->active;
   owner->changed.notify();
}

boost::asio::awaitable<core::availability> file_store::impl::wait_change(
   std::shared_ptr<impl> self, std::uint64_t observed, std::stop_token token) {
   for (;;) {
      const auto epoch = self->changed.epoch();
      {
         const auto lock = std::scoped_lock{self->mutex};
         if (self->closing) { throw core::exceptions::storage_closed{"file store is closing"}; }
         if (self->generation != observed) { break; }
      }
      co_await self->changed.async_wait(epoch, token);
   }
   co_return self->snapshot();
}

} // namespace forge::swarm::storage
