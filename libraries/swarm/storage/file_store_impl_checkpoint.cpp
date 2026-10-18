module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/scope/scope_exit.hpp>
#include <algorithm>
#include <cstdint>
#include <exception>
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

namespace forge::swarm::storage {

core::resume_state file_store::impl::checkpoint_locked(std::uint64_t value) const {
   auto result = core::resume_state{1U, id, value, published, verified, {}};
   for (const auto& [piece, state] : pending) { result.unfinished.push_back({piece, state.blocks}); }
   return result;
}

boost::asio::awaitable<void> file_store::impl::initialize(std::shared_ptr<impl> self, std::stop_token token) {
   const auto storage_key = co_await self->config.cpu.execute({"swarm.open.validate", token}, [self, token] {
      self->check_stop(token);
      self->id = core::calculate_swarm_id(self->config.manifest);
      auto count = 0U;
      for (const auto& file : self->config.manifest.files) {
         self->offsets.push_back(count);
         count += core::piece_count(file.size, self->config.manifest.piece_size);
      }
      self->verified.resize((count + 7U) / 8U);
      return self->id.value.str() + "-" + core::sha256::hash(self->config.destination.filename().string()).str();
   });
   auto restored = std::optional<core::resume_state>{};
   if (self->config.checkpoint) {
      restored = co_await self->config.checkpoint->async_load(self->id, token);
      if (restored) {
         restored = co_await self->config.cpu.execute({"swarm.checkpoint.validate", token},
            [self, state = std::move(*restored)]() mutable {
               core::validate_checkpoint(state, self->config.manifest);
               return std::move(state);
            });
      }
      if (restored && self->config.mode == open_mode::create) {
         throw exceptions::conflict{"create refuses an existing external checkpoint"};
      }
   }
   co_await self->config.disk.execute({"swarm.open.files", token}, [self, storage_key, token, external = restored.has_value()] {
      self->check_stop(token);
      self->layout = std::make_unique<detail::file_layout>(
         self->config.manifest, self->config.destination, storage_key, self->config.mode,
         self->config.checkpoint != nullptr, external);
   });
   if (!self->config.checkpoint && self->config.mode != open_mode::create) {
      auto bytes = co_await self->config.disk.execute({"swarm.open.checkpoint", token}, [self] { return self->layout->read_checkpoint(); });
      if (bytes) {
         restored = co_await self->config.cpu.execute({"swarm.checkpoint.decode", token}, [self, bytes = std::move(*bytes)] {
            return core::decode_checkpoint(bytes, self->config.manifest);
         });
      }
   }
   if (restored) {
      if (restored->generation == std::numeric_limits<std::uint64_t>::max()) {
         throw core::exceptions::invalid_checkpoint{"checkpoint generation exhausted"};
      }
      if (restored->unfinished.size() > self->config.max_unfinished) {
         throw core::exceptions::invalid_checkpoint{"checkpoint exceeds the configured unfinished bound"};
      }
      self->next_checkpoint = self->durable_generation = restored->generation;
      for (auto& item : restored->unfinished) {
         self->pending.emplace(item.piece, pending_piece{0, false, false, std::move(item.blocks)});
      }
   }
   // Re-read and hash every piece. A saved bitmap, unchanged timestamps and the cache
   // generation are not evidence of current file contents, even for a published seed.
   if (self->config.mode != open_mode::create) {
      for (auto file = 0U; file < self->config.manifest.files.size(); ++file) {
         const auto count = core::piece_count(self->config.manifest.files[file].size, self->config.manifest.piece_size);
         for (auto index = 0U; index < count; ++index) {
            self->check_stop(token);
            const auto piece = core::piece_id{file, index};
            const auto length = core::piece_length(self->config.manifest, piece);
            auto bytes = co_await self->config.disk.execute({"swarm.resume.read", token}, [self, piece, length] {
               return self->layout->read(piece.file, std::uint64_t{piece.index} * self->config.manifest.piece_size, length);
            });
            const auto valid = co_await self->config.cpu.execute({"swarm.resume.verify", token}, [self, piece, bytes = std::move(bytes)] {
               return self->matches(piece, bytes);
            });
            if (valid) { self->set_verified_locked(piece, true); self->pending.erase(piece); }
            else if (self->config.mode == open_mode::seed || self->layout->published()) {
               throw core::exceptions::integrity_failure{"existing published content does not match its descriptor"};
            }
         }
      }
   }
   self->published = self->layout->published();
   self->check_stop(token);
}

boost::asio::awaitable<void> file_store::impl::quiesce(std::shared_ptr<impl> self, bool closing, std::stop_token token) {
   {
      const auto lock = std::scoped_lock{self->mutex};
      if (self->closing && !closing) { throw core::exceptions::storage_closed{"file store is closing"}; }
      self->maintenance = true;
   }
   for (;;) {
      const auto epoch = self->changed.epoch();
      {
         const auto lock = std::scoped_lock{self->mutex};
         if (self->active == 0U && (!closing || self->leases == 0U)) { co_return; }
      }
      co_await self->changed.async_wait(epoch, token);
   }
}

boost::asio::awaitable<core::resume_state> file_store::impl::save(std::shared_ptr<impl> self, std::stop_token token) {
   auto state = core::resume_state{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      if (self->next_checkpoint >= std::numeric_limits<std::uint64_t>::max() - 1U) {
         throw core::exceptions::invalid_checkpoint{"checkpoint generation exhausted"};
      }
      state = self->checkpoint_locked(++self->next_checkpoint);
   }
   if (self->config.checkpoint) {
      state = co_await self->config.cpu.execute({"swarm.checkpoint.validate", token},
         [self, state = std::move(state)]() mutable {
            core::validate_checkpoint(state, self->config.manifest);
            return std::move(state);
         });
   }
   co_await self->config.disk.execute({"swarm.checkpoint.sync", token}, [self] {
      self->layout->sync_payload();
      if (self->config.checkpoint) { self->layout->sync_parent(); }
   });
   if (self->config.checkpoint) {
      co_await self->config.checkpoint->async_save(state, token);
   } else {
      auto bytes = co_await self->config.cpu.execute({"swarm.checkpoint.encode", token}, [self, state] {
         return core::encode_checkpoint(state, self->config.manifest);
      });
      co_await self->config.disk.execute({"swarm.checkpoint.replace", token}, [self, bytes = std::move(bytes)] {
         self->layout->write_checkpoint(bytes);
      });
   }
   {
      const auto lock = std::scoped_lock{self->mutex};
      self->durable_generation = state.generation;
      self->changed_locked();
   }
   co_return state;
}

boost::asio::awaitable<core::resume_state> file_store::impl::checkpoint(std::shared_ptr<impl> self, std::stop_token token) {
   self->check_stop(token);
   auto admitted = maintenance_request{self};
   auto cancel = cancellation{self, token};
   token = cancel.source.get_token();
   auto ticket = co_await self->maintenance_gate.acquire(token);
   const auto release = boost::scope::scope_exit{[self] {
      const auto lock = std::scoped_lock{self->mutex}; self->maintenance = false; self->changed.notify();
   }};
   co_await quiesce(self, false, token);
   self->check_stop(token);
   co_return co_await save(self, token);
}

boost::asio::awaitable<void> file_store::impl::publish(std::shared_ptr<impl> self, std::stop_token token) {
   self->check_stop(token);
   auto admitted = maintenance_request{self};
   auto cancel = cancellation{self, token};
   token = cancel.source.get_token();
   auto ticket = co_await self->maintenance_gate.acquire(token);
   const auto release = boost::scope::scope_exit{[self] {
      const auto lock = std::scoped_lock{self->mutex}; self->maintenance = false; self->changed.notify();
   }};
   co_await quiesce(self, false, token);
   {
      const auto lock = std::scoped_lock{self->mutex};
      if (!self->pending.empty()) { throw core::exceptions::unverified_piece{"unfinished pieces prevent publication"}; }
      for (auto i = 0U; i < self->offsets.back() + core::piece_count(self->config.manifest.files.back().size, self->config.manifest.piece_size); ++i) {
         if (!core::bitmap_test(self->verified, i)) { throw core::exceptions::unverified_piece{"publication requires every piece"}; }
      }
   }
   self->check_stop(token);
   // Persist the staged identity before rename. If cancellation crosses publication,
   // reconcile the actual layout; never manufacture a rollback by deleting data.
   static_cast<void>(co_await save(self, token));
   const auto reconcile = boost::scope::scope_exit{[self] {
      const auto lock = std::scoped_lock{self->mutex}; self->published = self->layout->published(); self->changed_locked();
   }};
   co_await self->config.disk.execute({"swarm.publish", token}, [self, token] {
      self->check_stop(token);
      if (!self->layout->published()) { self->layout->publish(); }
      self->layout->sync_parent();
   });
   {
      const auto lock = std::scoped_lock{self->mutex}; self->published = self->layout->published();
   }
   static_cast<void>(co_await save(self, token));
}

boost::asio::awaitable<void> file_store::impl::close(std::shared_ptr<impl> self) {
   self->request_stop();
   auto ticket = co_await self->maintenance_gate.acquire();
   {
      const auto lock = std::scoped_lock{self->mutex}; if (self->closed) { co_return; }
   }
   auto error = std::exception_ptr{};
   try {
      co_await quiesce(self, true, {});
      static_cast<void>(co_await save(self, {}));
   } catch (...) { error = std::current_exception(); }
   try {
      co_await self->config.disk.execute({"swarm.close", {}}, [self] { self->layout->close_checked(); });
   } catch (...) { if (!error) { error = std::current_exception(); } }
   {
      const auto lock = std::scoped_lock{self->mutex}; self->closed = true; self->changed_locked();
   }
   if (error) { std::rethrow_exception(error); }
}

boost::asio::awaitable<void> file_store::impl::close_owned(std::shared_ptr<impl> self) {
   const auto executor = co_await boost::asio::this_coro::executor;
   co_await boost::asio::co_spawn(executor, close(std::move(self)),
      boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
}

} // namespace forge::swarm::storage
