module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <forge/exceptions/macros.hpp>

module forge.swarm.runtime.engine;

import forge.asio.gate;
import forge.asio.notification;

#include "details/resource_budget.hxx"
#include "details/operation_state.hxx"
#include "details/call_scope.hxx"
#include "details/peer_session.hxx"

namespace forge::swarm::runtime::detail {

boost::asio::awaitable<void> peer_session::connect(std::shared_ptr<operation_state> owner, std::string route) {
   auto self = std::make_shared<peer_session>(owner, owner->next_peer());
   self->_scope = std::make_shared<call_scope>(owner,
      "connection:" + std::to_string(owner->number) + ":" + std::to_string(self->_number));
   self->_scope->bind({});
   return connect_owned(std::move(self), std::move(route));
}
boost::asio::awaitable<void> peer_session::connect_owned(std::shared_ptr<peer_session> self, std::string route) {
   const auto owner = self->_owner;
   self->_scope->start();
   std::exception_ptr failure;
   bool terminal_waited = false;
   try {
      self->_connection = co_await owner->providers->async_connect(std::move(route), self->_scope->stop.get_token(),
         std::chrono::steady_clock::now() + owner->config.callback_timeout);
      self->_scope->check();
      if (!self->_connection) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "Swarm connection capability returned empty hold");
      }
      self->_scope->rebind(self->_connection->remote_identity());
      self->_scope->check();
      self->_scope->begin_transfer();
      auto remote = self->_connection->remote();
      auto owned_call = std::make_shared<client_call>();
      auto call = co_await remote.async_open<&api::peer::exchange>(forge::api::core::call_options{
         .max_item_bytes = protocol::max_wire_message_size, .max_buffered_items = 1,
         .max_buffered_bytes = protocol::max_wire_message_size, .deadline = owner->total_deadline},
         protocol::exchange_request{owner->content, owner->realm}, forge::api::auth::authenticated_caller{});
      *owned_call = std::move(call);
      self->_call = std::move(owned_call);
      // Immutable after publication, and the callback is destroyed BEFORE reset.
      std::stop_callback cancel{self->_scope->stop.get_token(), [self] {
         self->_call->cancel(); self->_connection->cancel();
      }};
      co_await paired(self, false);
      terminal_waited = true;
      co_await self->_call->async_finish();
   } catch (...) { failure = std::current_exception(); }
   if (failure) self->interrupt();
   if (self->_call && !terminal_waited) {
      try { co_await self->_call->async_finish(); } catch (...) { if (!failure) failure = std::current_exception(); }
   }
   bool cleanup_failed = false;
   if (self->_remote) {
      try {
         auto lock = std::scoped_lock{owner->mutex};
         static_cast<void>(owner->picker->remove_peer(*self->_remote,
            std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now())));
         owner->sessions.erase(*self->_remote);
      } catch (...) {
         const auto cleanup_failure = std::current_exception();
         if (!failure) failure = cleanup_failure;
         { auto lock = std::scoped_lock{owner->mutex};
           owner->sessions.erase(*self->_remote);
           if (!owner->cleanup_failure) owner->cleanup_failure = cleanup_failure; }
         cleanup_failed = true;
      }
   }
   if (cleanup_failed) { owner->cancel(); self->interrupt(); }
   self->_requests.clear();
   self->_commands.clear();
   if (self->_connection) {
      if (failure) self->_connection->cancel();
      try { co_await self->_connection->async_close(); } catch (...) { if (!failure) failure = std::current_exception(); }
   }
   self->_call.reset(); self->_connection.reset();
   co_await self->_scope->finish();
   owner->changed.notify();
   if (failure) std::rethrow_exception(failure);
}

void peer_session::retract(std::uint64_t id) {
   auto lock = std::scoped_lock{_mutex};
   if (auto found = _requests.find(id); found != _requests.end()) {
      // The already admitted request ticket pays for its cancellation command;
      // cancellation never needs a new data-work admission slot.
      _commands.push_back({{protocol::cancel{id}}, std::move(found->second.capacity)});
      _requests.erase(found);
   }
   _owner->changed.notify();
}

boost::asio::awaitable<void> peer_session::download_read(std::shared_ptr<peer_session> self) {
   try {
      while (auto message = co_await self->_call->async_read()) {
         self->_scope->check();
         protocol::validate(*message, self->_owner->manifest);
         if (!self->_hello) {
            const auto* hello = std::get_if<protocol::handshake>(&message->body);
            if (!hello || hello->swarm != self->_owner->content || hello->realm != self->_owner->realm) {
               FORGE_THROW_EXCEPTION(protocol::exceptions::invalid_message, "Swarm download bound handshake required");
            }
            self->_hello = true; continue;
         }
         if (auto* inventory = std::get_if<protocol::inventory>(&message->body)) {
            auto lock = std::scoped_lock{self->_owner->mutex};
            self->_inventory = inventory->pieces;
            if (!self->_remote) {
               self->_remote = self->_owner->picker->add_peer(self->_inventory);
               self->_owner->sessions.emplace(*self->_remote, self);
            }
            else self->_owner->picker->set_availability(*self->_remote, self->_inventory);
         } else if (auto* have = std::get_if<protocol::have>(&message->body)) {
            auto lock = std::scoped_lock{self->_owner->mutex};
            if (!self->_remote) {
               FORGE_THROW_EXCEPTION(protocol::exceptions::invalid_message, "Swarm HAVE before inventory");
            }
            const auto index = core::piece_ordinal(self->_owner->manifest, have->piece);
            self->_inventory.at(index / 8) |= static_cast<std::uint8_t>(1U << (index % 8));
            self->_owner->picker->set_availability(*self->_remote, self->_inventory);
         } else if (auto* choke = std::get_if<protocol::choke>(&message->body)) {
            auto lock = std::scoped_lock{self->_owner->mutex};
            if (!self->_remote) {
               FORGE_THROW_EXCEPTION(protocol::exceptions::invalid_message, "Swarm choke before inventory");
            }
            auto canceled = self->_owner->picker->set_choked(*self->_remote, choke->choked,
               std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()));
            self->_choked = choke->choked;
            for (const auto& request : canceled) self->retract(request.id);
         } else if (auto* reject = std::get_if<protocol::reject>(&message->body)) {
            auto lock = std::scoped_lock{self->_owner->mutex};
            if (self->_remote) static_cast<void>(self->_owner->picker->cancel(*self->_remote, reject->id,
               std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now())));
            auto request_lock = std::scoped_lock{self->_mutex};
            self->_requests.erase(reject->id);
         } else if (auto* block = std::get_if<protocol::block>(&message->body)) {
            co_await receive(self, std::move(*block));
         } else {
            FORGE_THROW_EXCEPTION(protocol::exceptions::invalid_message, "Swarm unexpected download message");
         }
         self->_changed.notify(); self->_owner->changed.notify();
      }
      { auto lock = std::scoped_lock{self->_mutex}; self->_output_closed = true; }
      self->_owner->changed.notify();
   } catch (...) { self->interrupt(); throw; }
}

boost::asio::awaitable<void> peer_session::download_write(std::shared_ptr<peer_session> self) {
   try {
      auto control = self->_owner->budget->reserve(self->_owner->number, self->_scope->peer, {0, 1, 256, 0});
      co_await self->_call->async_write({protocol::handshake{1, self->_owner->content, self->_owner->realm, 0}});
      co_await self->_call->async_write({protocol::interest{true}});
      std::optional<std::chrono::steady_clock::time_point> capacity_deadline;
      std::uint64_t capacity_progress = 0;
      while (true) {
         const auto epoch = self->_owner->changed.epoch();
         self->_scope->check();
         bool output_closed;
         { auto lock = std::scoped_lock{self->_mutex}; output_closed = self->_output_closed; }
         if (output_closed) {
            co_await self->_call->async_close();
            co_return;
         }
         std::optional<core::transfer::request> request;
         std::optional<command> cancel;
         std::shared_ptr<operation_state::piece_work> piece;
         resource_budget::ticket capacity;
         auto waiting_for = resource_limits{0, 1, 3ULL * core::block_size, 0};
         bool blocked = false;
         bool complete;
         std::uint64_t progress;
         {
            auto lock = std::scoped_lock{self->_owner->mutex};
            complete = self->_owner->picker->inspect().complete;
            progress = self->_owner->useful_progress;
            const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now());
            {
               auto request_lock = std::scoped_lock{self->_mutex};
               if (!self->_commands.empty()) { cancel.emplace(std::move(self->_commands.front())); self->_commands.pop_front(); }
            }
            if (!complete && !cancel) {
               auto expired = self->_owner->picker->expire(now);
               // Ownership checks also discard late frames after expiration.
               for (const auto& item : expired) {
                  if (auto session = self->_owner->sessions[item.peer].lock()) session->retract(item.id);
               }
               auto request_lock = std::scoped_lock{self->_mutex};
               if (!self->_commands.empty()) { cancel.emplace(std::move(self->_commands.front())); self->_commands.pop_front(); }
            }
            if (!complete && !cancel && self->_remote && !self->_choked) {
               auto request_lock = std::scoped_lock{self->_mutex};
               if (self->_requests.empty()) {
                  auto admitted = self->_owner->budget->try_reserve(self->_owner->number, self->_scope->peer, waiting_for);
                  if (!admitted) blocked = true;
                  else {
                     capacity = std::move(*admitted);
                     auto picked = self->_owner->picker->pick(*self->_remote, now, 1);
                     if (!picked.empty()) {
                        request = picked.front();
                        const auto new_piece = !self->_owner->pieces.contains(request->piece);
                        try {
                           piece = self->_owner->piece(request->piece, self->_scope->peer);
                           if (piece) {
                              self->_requests.emplace(request->id, owned_request{*request, std::move(capacity), piece, piece->generation});
                           } else {
                              static_cast<void>(self->_owner->picker->cancel(*self->_remote, request->id, now));
                              waiting_for.items += 1;
                              waiting_for.bytes += 2ULL * core::piece_length(self->_owner->manifest, request->piece);
                              waiting_for.pieces = 1;
                              request.reset(); blocked = true;
                           }
                        } catch (...) {
                           static_cast<void>(self->_owner->picker->cancel(*self->_remote, request->id, now));
                           if (new_piece) self->_owner->pieces.erase(request->piece);
                           throw;
                        }
                     }
                  }
               }
            }
         }
         if (cancel) { co_await self->_call->async_write(std::move(cancel->message)); continue; }
         if (complete) {
            co_await self->_call->async_close();
            co_return;
         }
         if (request) {
            capacity_deadline.reset();
            self->_owner->add_fact(&facts::requested, request->length);
            co_await self->_call->async_write({protocol::request{request->id, request->piece, request->offset, request->length}});
            continue;
         }
         capacity.release();
         if (blocked) {
            const auto now = std::chrono::steady_clock::now();
            if (!capacity_deadline || capacity_progress != progress) {
               capacity_deadline = std::min(now + self->_owner->config.transfer.request_timeout,
                  self->_owner->total_deadline.value_or(std::chrono::steady_clock::time_point::max()));
               capacity_progress = progress;
            }
            if (now >= *capacity_deadline) {
               FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted, "Swarm resource admission stalled without useful progress");
            }
            // Recheck AFTER releasing our temporary request ticket. Waiting
            // on its own release epoch would otherwise spin, not backpressure.
            const auto budget_epoch = self->_owner->budget->changed.epoch();
            if (self->_owner->budget->available(self->_owner->number, self->_scope->peer, waiting_for)) {
               co_await boost::asio::post(co_await boost::asio::this_coro::executor, boost::asio::use_awaitable);
               continue;
            }
            auto local_work = self->_scope->pause_idle();
            using namespace boost::asio::experimental::awaitable_operators;
            std::exception_ptr wait_failure;
            { auto lock = std::scoped_lock{self->_owner->mutex}; ++self->_owner->state.capacity_waiters; }
            try {
               co_await (self->_owner->budget->changed.async_wait_until(budget_epoch, *capacity_deadline, self->_scope->stop.get_token()) ||
                  self->_owner->changed.async_wait_until(epoch, *capacity_deadline, self->_scope->stop.get_token()));
            } catch (...) { wait_failure = std::current_exception(); }
            { auto lock = std::scoped_lock{self->_owner->mutex}; --self->_owner->state.capacity_waiters; }
            if (wait_failure) std::rethrow_exception(wait_failure);
            continue;
         }
         co_await self->_owner->changed.async_wait_until(epoch,
            std::chrono::steady_clock::now() + self->_owner->config.transfer.request_timeout, self->_scope->stop.get_token());
      }
   } catch (...) { self->interrupt(); throw; }
}

boost::asio::awaitable<void> peer_session::receive(std::shared_ptr<peer_session> self, protocol::block block) {
   self->_owner->add_fact(&facts::received, block.data.size());
   std::optional<owned_request> request;
   {
      auto lock = std::scoped_lock{self->_mutex};
      auto found = self->_requests.find(block.id);
      if (found == self->_requests.end()) co_return;
      const auto& owned = found->second.request;
      if (block.piece != owned.piece || block.offset != owned.offset || block.data.size() != owned.length) {
         FORGE_THROW_EXCEPTION(protocol::exceptions::invalid_message, "Swarm block does not match owned request geometry");
      }
      request.emplace(std::move(found->second));
      self->_requests.erase(found);
   }
   // Taking the request out of the map prevents duplicate delivery, but no new
   // pick can overwrite its piece while the writer gate is held.
   auto local_work = self->_scope->pause_idle();
   auto gate = co_await request->piece->gate.acquire(self->_scope->stop.get_token());
   std::optional<core::transfer::verification> verification;
   {
      auto lock = std::scoped_lock{self->_owner->mutex};
      if (self->_owner->stop.stop_requested() || !self->_remote || request->generation != request->piece->generation) co_return;
      const auto result = self->_owner->picker->receive(*self->_remote, block.id, block.data.size(),
         std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()));
      if (!result.accepted) co_return;
      verification = result.ready;
      for (const auto& canceled : result.canceled) {
         if (auto session = self->_owner->sessions[canceled.peer].lock()) session->retract(canceled.id);
      }
   }
   std::exception_ptr write_failure;
   bool doing_storage = false;
   bool storage_failed = false;
   try {
      self->_scope->check();
      doing_storage = true;
      if (!request->piece->writer) request->piece->writer = self->_owner->store->begin(block.piece);
      co_await request->piece->writer->async_write(block.offset, std::move(block.data), self->_scope->stop.get_token());
      doing_storage = false;
      self->_scope->check();
      self->_scope->progress();
      { auto lock = std::scoped_lock{self->_owner->mutex}; ++self->_owner->useful_progress; }
   } catch (...) {
      write_failure = std::current_exception();
      storage_failed = doing_storage && !self->_scope->stop.stop_requested() && !self->_owner->stop.stop_requested();
   }
   if (write_failure) {
      if (storage_failed) {
         { auto lock = std::scoped_lock{self->_owner->mutex};
           if (!self->_owner->storage_failure) self->_owner->storage_failure = write_failure; }
         // Preserve the terminal storage error even if rollback itself cannot
         // allocate. Stop siblings outside the picker lock before discarding.
         self->_owner->cancel();
      }
      if (request->piece->writer) {
         try { co_await request->piece->writer->async_discard(); } catch (...) {}
         request->piece->writer.reset();
      }
      {
         auto lock = std::scoped_lock{self->_owner->mutex};
         ++request->piece->generation;
         const auto canceled = self->_owner->picker->discard(block.piece,
            std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()));
         for (const auto& item : canceled) {
            if (auto session = self->_owner->sessions[item.peer].lock()) session->retract(item.id);
         }
         self->_owner->pieces.erase(block.piece);
      }
      self->_owner->changed.notify();
      std::rethrow_exception(write_failure);
   }
   if (verification) {
      bool verified = true;
      std::exception_ptr commit_failure;
      doing_storage = true;
      try {
         co_await request->piece->writer->async_commit(self->_scope->stop.get_token());
         doing_storage = false;
         self->_scope->check();
      }
      catch (const core::exceptions::integrity_failure&) { verified = false; }
      catch (...) {
         verified = false; commit_failure = std::current_exception();
         storage_failed = doing_storage && !self->_scope->stop.stop_requested() && !self->_owner->stop.stop_requested();
      }
      if (storage_failed) {
         { auto lock = std::scoped_lock{self->_owner->mutex};
           if (!self->_owner->storage_failure) self->_owner->storage_failure = commit_failure; }
         self->_owner->cancel();
      }
      request->piece->writer.reset();
      {
         auto lock = std::scoped_lock{self->_owner->mutex};
         const auto current = self->_owner->picker->verification_for(block.piece);
         if (!current || *current != *verification) co_return;
         const auto result = self->_owner->picker->finish(*verification, verified);
         if (result == core::transfer::verification_result::verified)
            self->_owner->state.traffic.verified += core::piece_length(self->_owner->manifest, block.piece);
         self->_owner->pieces.erase(block.piece);
      }
      if (commit_failure) std::rethrow_exception(commit_failure);
      bool exhausted = false;
      if (!verified) {
         auto lock = std::scoped_lock{self->_owner->mutex};
         exhausted = ++self->_owner->integrity_failures >= self->_owner->config.integrity_failures;
      }
      if (exhausted) {
         self->_owner->cancel();
         FORGE_THROW_EXCEPTION(core::exceptions::integrity_failure, "Swarm download exceeded integrity failure bound");
      }
   }
}

} // namespace forge::swarm::runtime::detail
