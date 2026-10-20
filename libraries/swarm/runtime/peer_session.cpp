module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <algorithm>
#include <atomic>
#include <bit>
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

import forge.swarm.protocol.metadata_reader;
import forge.swarm.core.choker;
import forge.asio.gate;
import forge.asio.notification;

#include "details/resource_budget.hxx"
#include "details/operation_state.hxx"
#include "details/call_scope.hxx"
#include "details/peer_session.hxx"

namespace forge::swarm::runtime::detail {

peer_session::peer_session(std::shared_ptr<operation_state> owner, std::uint64_t number)
   : _owner{std::move(owner)}, _number{number} {}

boost::asio::awaitable<void> peer_session::authorize(std::shared_ptr<call_scope> scope,
   forge::api::auth::authenticated_caller caller, access_kind kind) {
   if (!caller.transport_authenticated() || !scope->owner->policy) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::invocation_denied, "Swarm requires explicit policy and trusted caller");
   }
   std::shared_ptr<quota> grant;
   std::exception_ptr failure;
   try {
      grant = co_await scope->owner->policy->async_authorize(scope->owner->content, caller, kind,
         scope->stop.get_token(), scope->deadline);
   } catch (...) { failure = std::current_exception(); }
   if (failure || !grant || !grant->permits(scope->owner->content, caller)) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::invocation_denied, "Swarm product policy denied invocation");
   }
   scope->check();
   scope->permit(std::move(grant));
   scope->check();
}

boost::asio::awaitable<void> peer_session::metadata(std::shared_ptr<operation_state> owner,
   forge::api::auth::authenticated_caller caller, forge::api::core::stream_writer<protocol::metadata_chunk> output) {
   if (!caller.transport_authenticated()) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::invocation_denied, "Swarm local calls also require trusted caller");
   }
   auto scope = std::make_shared<call_scope>(owner,
      "authenticated:" + std::to_string(static_cast<unsigned>(caller.source)) + ":" + caller.fingerprint.str());
   scope->bind({}, forge::api::core::detail::writer_access::endpoint(output));
   scope->start();
   std::exception_ptr failure;
   try {
      co_await authorize(scope, caller, access_kind::metadata);
      const auto first_size = std::min(owner->metadata_size, protocol::metadata_chunk_size);
      owner->add_fact(&facts::requested, first_size);
      auto first_allowance = std::optional<quota::reservation>{scope->grant->reserve(first_size)};
      auto capacity = owner->budget->reserve(owner->number, scope->peer,
         {0, 1, 3ULL * owner->metadata_size + protocol::metadata_chunk_size, 0});
      auto reader = std::optional<protocol::metadata_reader>{};
      {
         auto local_work = scope->pause_idle();
         reader.emplace(co_await owner->cpu.execute({.name = "swarm-metadata", .parent_stop_token = scope->stop.get_token()},
            [owner](forge::asio::compute::context& context) {
               context.throw_if_stop_requested();
               return protocol::metadata_reader{protocol::metadata{owner->content, owner->manifest}};
            }));
      }
      while (auto chunk = reader->next()) {
         scope->check();
         const auto length = chunk->data.size();
         if (!first_allowance) owner->add_fact(&facts::requested, length);
         auto allowance = first_allowance ? std::move(*first_allowance) : scope->grant->reserve(length);
         first_allowance.reset();
         allowance.queue();
         owner->add_fact(&facts::attempted, length);
         try {
            co_await output.async_write(std::move(*chunk));
            allowance.queued(); owner->add_fact(&facts::queued, length);
            scope->progress();
         } catch (...) { owner->add_fact(&facts::uncertain, length); throw; }
      }
      scope->check();
      co_await output.async_close();
   } catch (...) { failure = std::current_exception(); scope->interrupt(); }
   co_await scope->finish();
   if (failure) std::rethrow_exception(failure);
}

boost::asio::awaitable<void> peer_session::serve(std::shared_ptr<operation_state> owner,
   forge::api::auth::authenticated_caller caller,
   forge::api::core::duplex_stream<protocol::peer_message, protocol::peer_message> stream) {
   if (!caller.transport_authenticated()) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::invocation_denied, "Swarm local calls also require trusted caller");
   }
   auto self = std::make_shared<peer_session>(owner, owner->next_peer());
   self->_server.emplace(std::move(stream));
   self->_scope = std::make_shared<call_scope>(owner,
      "authenticated:" + std::to_string(static_cast<unsigned>(caller.source)) + ":" + caller.fingerprint.str());
   self->_scope->bind(forge::api::core::detail::reader_access::endpoint(self->_server->input()),
      forge::api::core::detail::writer_access::endpoint(self->_server->output()));
   self->_scope->start();
   std::exception_ptr failure;
   bool registered = false;
   try {
      co_await authorize(self->_scope, caller, access_kind::content);
      owner->upload_enter(self->_number); registered = true;
      co_await paired(self, true);
   } catch (...) { failure = std::current_exception(); self->interrupt(); }
   if (registered) owner->upload_leave(self->_number);
   self->_commands.clear(); self->_server.reset();
   co_await self->_scope->finish();
   if (failure) std::rethrow_exception(failure);
}

void peer_session::interrupt() noexcept {
   _scope->interrupt();
   if (_call) _call->cancel();
   _changed.notify();
}
boost::asio::awaitable<void> peer_session::paired(std::shared_ptr<peer_session> self, bool serving) {
   using namespace boost::asio::experimental::awaitable_operators;
   if (serving) co_await (serve_read(self) && serve_write(self) && watch_availability(self));
   else co_await (download_read(self) && download_write(self));
}

boost::asio::awaitable<void> peer_session::watch_availability(std::shared_ptr<peer_session> self) {
   std::stop_callback cancel{self->_scope->stop.get_token(), [self] { self->_availability_stop.request_stop(); }};
   auto generation = self->_owner->store->snapshot().generation;
   self->_owner->changed.notify();
   try {
      while (!self->_availability_stop.stop_requested()) {
         const auto state = co_await self->_owner->store->async_wait_change(generation, self->_availability_stop.get_token());
         generation = state.generation;
         // Coalesce in the existing writer; no payload queue or topology worker.
         self->_owner->changed.notify();
      }
   } catch (...) {
      if (self->_availability_stop.stop_requested()) co_return;
      self->interrupt(); throw;
   }
}

boost::asio::awaitable<void> peer_session::send(std::shared_ptr<peer_session> self,
   protocol::peer_message message, std::uint64_t units, std::optional<quota::reservation> reserved) {
   self->_scope->check();
   auto capacity = self->_owner->budget->reserve(self->_owner->number, self->_scope->peer,
      {0, 1, 3ULL * (units + 128), 0});
   if (!reserved) self->_owner->add_fact(&facts::requested, units);
   auto allowance = reserved ? std::move(*reserved) : self->_scope->grant->reserve(units);
   allowance.queue();
   self->_owner->add_fact(&facts::attempted, units);
   try {
      co_await self->_server->async_write(std::move(message));
      allowance.queued();
      self->_owner->add_fact(&facts::queued, units);
      self->_owner->upload_served(self->_number, units);
      if (units) self->_scope->progress();
   } catch (...) { self->_owner->add_fact(&facts::uncertain, units); throw; }
}

boost::asio::awaitable<void> peer_session::serve_read(std::shared_ptr<peer_session> self) {
   try {
      while (auto message = co_await self->_server->async_read()) {
         self->_scope->check();
         protocol::validate(*message, self->_owner->manifest);
         if (!self->_hello) {
            const auto* hello = std::get_if<protocol::handshake>(&message->body);
            if (!hello || hello->swarm != self->_owner->content || hello->realm != self->_owner->realm) {
               FORGE_THROW_EXCEPTION(protocol::exceptions::invalid_message, "Swarm bound handshake required");
            }
            { auto lock = std::scoped_lock{self->_mutex}; self->_hello = true; }
            self->_changed.notify();
            continue;
         }
         if (std::holds_alternative<protocol::handshake>(message->body)) {
            FORGE_THROW_EXCEPTION(protocol::exceptions::invalid_message, "Swarm duplicate handshake");
         }
         if (auto* interest = std::get_if<protocol::interest>(&message->body)) {
            self->_owner->upload_interest(self->_number, interest->interested);
            continue;
         }
         if (auto* cancel = std::get_if<protocol::cancel>(&message->body)) {
            auto lock = std::scoped_lock{self->_mutex};
            if (self->_serving.contains(cancel->id)) self->_canceled.insert(cancel->id);
            continue;
         }
         std::optional<std::uint64_t> id;
         if (auto* request = std::get_if<protocol::request>(&message->body)) id = request->id;
         if (auto* request = std::get_if<protocol::hash_request>(&message->body)) id = request->id;
         if (!id) {
            if (std::holds_alternative<protocol::inventory>(message->body) ||
                std::holds_alternative<protocol::have>(message->body) ||
                std::holds_alternative<protocol::choke>(message->body)) continue;
            FORGE_THROW_EXCEPTION(protocol::exceptions::invalid_message, "Swarm unexpected serving message");
         }
         auto capacity = self->_owner->budget->reserve(self->_owner->number, self->_scope->peer, {0, 1, 256, 0});
         {
            auto lock = std::scoped_lock{self->_mutex};
            if (self->_commands.size() >= self->_owner->config.per_peer.items / 2 || !self->_serving.insert(*id).second) {
               FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted, "Swarm command queue full or duplicate request");
            }
            self->_commands.push_back({std::move(*message), std::move(capacity)});
         }
         self->_changed.notify();
         self->_owner->changed.notify();
      }
      {
         auto lock = std::scoped_lock{self->_mutex};
         if (!self->_hello) {
            FORGE_THROW_EXCEPTION(protocol::exceptions::invalid_message, "Swarm handshake missing at input EOF");
         }
         self->_input_closed = true;
      }
      self->_changed.notify(); self->_owner->changed.notify();
   } catch (...) { self->interrupt(); throw; }
}

boost::asio::awaitable<void> peer_session::serve_write(std::shared_ptr<peer_session> self) {
   try {
      while (true) {
         const auto epoch = self->_changed.epoch();
         bool hello;
         { auto lock = std::scoped_lock{self->_mutex}; hello = self->_hello; }
         if (hello) break;
         co_await self->_changed.async_wait(epoch, self->_scope->stop.get_token());
         self->_scope->check();
      }
      co_await send(self, {protocol::handshake{1, self->_owner->content, self->_owner->realm, 0}}, 0);
      auto snapshot = self->_owner->store->snapshot();
      self->_inventory = snapshot.verified;
      co_await send(self, {protocol::inventory{std::move(snapshot.verified)}}, 0);
      co_await send(self, {protocol::choke{true}}, 0);
      while (true) {
         const auto epoch = self->_owner->changed.epoch();
         self->_scope->check();
         bool drained;
         { auto lock = std::scoped_lock{self->_mutex}; drained = self->_input_closed && self->_commands.empty(); }
         if (drained) {
            co_await self->_server->async_close();
            self->_availability_stop.request_stop();
            co_return;
         }
         const auto allowed = self->_owner->upload_allowed(self->_number);
         if (self->_choked == allowed) {
            self->_choked = !allowed;
            co_await send(self, {protocol::choke{self->_choked}}, 0);
         }
         const auto current = self->_owner->store->snapshot();
         if (current.verified != self->_inventory) {
            bool lost = false;
            for (auto index = std::size_t{}; index < self->_inventory.size(); ++index)
               lost |= (self->_inventory[index] & ~current.verified.at(index)) != 0;
            if (lost) {
               co_await send(self, {protocol::inventory{current.verified}}, 0);
               self->_inventory = current.verified;
            } else {
               // Shallow, bounded announcements interleave with request service.
               auto ordinal = 0U;
               auto announced = 0U;
               for (auto file = 0U; file < self->_owner->manifest.files.size() && announced < 16; ++file) {
                  const auto count = core::piece_count(self->_owner->manifest.files[file].size, self->_owner->manifest.piece_size);
                  for (auto piece = 0U; piece < count && announced < 16; ++piece, ++ordinal) {
                     if (core::bitmap_test(current.verified, ordinal) && !core::bitmap_test(self->_inventory, ordinal)) {
                        co_await send(self, {protocol::have{{file, piece}}}, 0);
                        self->_inventory.at(ordinal / 8) |= static_cast<std::uint8_t>(1U << (ordinal % 8));
                        ++announced;
                     }
                  }
               }
               if (announced == 16) self->_owner->changed.notify();
            }
         }
         std::optional<command> command;
         {
            auto lock = std::scoped_lock{self->_mutex};
            if (!self->_commands.empty()) { command.emplace(std::move(self->_commands.front())); self->_commands.pop_front(); }
         }
         if (!command) {
            std::chrono::steady_clock::time_point next;
            { auto lock = std::scoped_lock{self->_owner->mutex}; next = self->_owner->upload_next; }
            co_await self->_owner->changed.async_wait_until(epoch, next, self->_scope->stop.get_token()); continue;
         }
         const auto* request = std::get_if<protocol::request>(&command->message.body);
         const auto* hash = std::get_if<protocol::hash_request>(&command->message.body);
         const auto id = request ? request->id : hash->id;
         const auto piece = request ? request->piece : hash->piece;
         bool canceled;
         { auto lock = std::scoped_lock{self->_mutex}; canceled = self->_canceled.contains(id); }
         const auto available = self->_owner->store->snapshot();
         if (canceled || !self->_owner->upload_allowed(self->_number) ||
             !core::bitmap_test(available.verified, core::piece_ordinal(self->_owner->manifest, piece))) {
            co_await send(self, {protocol::reject{id, canceled ? protocol::rejection::canceled : protocol::rejection::unavailable}}, 0);
         } else {
            const auto& file = self->_owner->manifest.files.at(piece.file);
            const auto siblings = file.pieces.empty() ? 0U : std::bit_width(file.pieces.size() - 1U);
            const auto allowed_bytes = request ? std::uint64_t{request->length} : 32ULL * (1 + siblings);
            self->_owner->add_fact(&facts::requested, allowed_bytes);
            auto allowance = self->_scope->grant->reserve(allowed_bytes);
            auto capacity = self->_owner->budget->reserve(self->_owner->number, self->_scope->peer,
               {0, 1, 2ULL * core::piece_length(self->_owner->manifest, piece) + 64ULL * file.pieces.size(), 0});
            protocol::peer_message response;
            std::uint64_t units;
            if (request) {
               auto local_work = self->_scope->pause_idle();
               auto data = co_await self->_owner->store->async_read(piece, request->offset, request->length,
                  self->_scope->stop.get_token());
               units = data.size(); response.body = protocol::block{id, piece, request->offset, std::move(data)};
            } else {
               auto local_work = self->_scope->pause_idle();
               auto proof = co_await self->_owner->cpu.execute({.name = "swarm-proof", .parent_stop_token = self->_scope->stop.get_token()},
                  [owner = self->_owner, piece](forge::asio::compute::context& context) {
                     context.throw_if_stop_requested();
                     const auto& file = owner->manifest.files.at(piece.file);
                     const auto hash = file.pieces.empty() ? file.root : file.pieces.at(piece.index);
                     return protocol::hash_response{0, piece, hash,
                        core::make_piece_proof(file.pieces, file.size, owner->manifest.piece_size, piece.index)};
                  });
               proof.id = id; units = 32ULL * (1 + proof.proof.siblings.size()); response.body = std::move(proof);
            }
            self->_scope->check();
            { auto lock = std::scoped_lock{self->_mutex}; canceled = self->_canceled.contains(id); }
            if (!canceled && self->_owner->upload_allowed(self->_number))
               co_await send(self, std::move(response), units, std::move(allowance));
            else if (!canceled)
               co_await send(self, {protocol::reject{id, protocol::rejection::choked}}, 0);
         }
         { auto lock = std::scoped_lock{self->_mutex}; self->_serving.erase(id); self->_canceled.erase(id); }
      }
   } catch (...) { self->interrupt(); throw; }
}

} // namespace forge::swarm::runtime::detail
