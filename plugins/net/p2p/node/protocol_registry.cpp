module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <stop_token>
#include <mutex>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.api.p2p.binding;
import forge.api.p2p.publication;
import forge.api.stream.session;
import forge.asio.notification;
import forge.net.p2p.node;
import forge.net.p2p.protocol;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.protocol_registration;

#include "details/protocol_registry.hxx"
#include "details/protocol_operations.hxx"
#include "details/api_publication_generation.hxx"

namespace forge::plugins::net::p2p::node::detail {

protocol_registry::protocol_registry(std::size_t max_slots, forge::app::plugin_lifecycle lifecycle)
    : lifecycle_{std::move(lifecycle)}, max_slots_{max_slots} {
   configure({}, max_slots);
}
protocol_registry::~protocol_registry() { request_close(); }

void protocol_registry::configure(std::vector<forge::net::p2p::protocol_id> protocols, std::size_t max_slots) {
   using namespace forge::net::p2p;
   auto reserved = std::vector<protocol_id>{builtins::echo, builtins::ping, builtins::identify, builtins::identify_push,
       builtins::peer_exchange, builtins::autonat_v1, builtins::autonat_v2_dial_request,
       builtins::autonat_v2_dial_back, builtins::relay_hop, builtins::relay_stop, builtins::dcutr,
       builtins::kad_dht, builtins::rendezvous, builtins::meshsub_v10, builtins::meshsub_v11,
       builtins::meshsub_v12, builtins::meshsub_v13};
   reserved.insert(reserved.end(), protocols.begin(), protocols.end());
   std::ranges::sort(reserved);
   reserved.erase(std::unique(reserved.begin(), reserved.end()), reserved.end());
   const auto lock = std::scoped_lock{mutex_};
   if (closed_ || operations_ || max_slots == 0 || slots_.size() > max_slots) {
      FORGE_THROW_EXCEPTION(exceptions::route_conflict, "protocol registry configuration is closed or over limit");
   }
   for (const auto& row : slots_) {
      if (std::ranges::find(reserved, row.slot->protocol()) != reserved.end()) {
         FORGE_THROW_EXCEPTION(exceptions::route_conflict, "configured DHT protocol is already claimed");
      }
   }
   reserved_.swap(reserved);
   max_slots_ = max_slots;
}

protocol_registry::position protocol_registry::find(const forge::net::p2p::protocol_id& protocol) noexcept {
   return std::find_if(slots_.begin(), slots_.end(), [&](const auto& row) { return row.slot->protocol() == protocol; });
}

void protocol_registry::require_available(const forge::net::p2p::protocol_id& protocol) const {
   if (closed_ || protocol.value.empty() || protocol.value.front() != '/' ||
       std::ranges::find(reserved_, protocol) != reserved_.end()) {
      FORGE_THROW_EXCEPTION(exceptions::route_conflict, "P2P protocol name is invalid, reserved or closed");
   }
}

void protocol_registry::bind_executor(boost::asio::any_io_executor executor) {
   const auto lock = std::scoped_lock{mutex_};
   if (closed_ || !executor) {
      FORGE_THROW_EXCEPTION(exceptions::route_conflict, "P2P protocol owner executor is unavailable");
   }
   if (!executor_) { executor_ = std::move(executor); }
}

void protocol_registry::detach(const std::shared_ptr<protocol_slot>& slot) {
   slot->detach();
   // The closing claim is kept throughout the native call, including its
   // exception path. false is a successful confirmation of native absence.
   const auto lock = std::scoped_lock{mutex_};
   const auto row = find(slot->protocol());
   if (row != slots_.end() && row->slot == slot && row->closing) { slots_.erase(row); }
}

void protocol_registry::retry_closing(const forge::net::p2p::protocol_id& protocol) {
   auto slot = std::shared_ptr<protocol_slot>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      const auto row = find(protocol);
      if (row != slots_.end() && row->closing && !row->attaching) { slot = row->slot; }
   }
   if (slot) { detach(slot); }
}

void protocol_registry::finish_attach(const std::shared_ptr<protocol_slot>& slot,
    const std::shared_ptr<const protocol_operations>& operations) {
   auto failure = std::exception_ptr{};
   try { if (operations) { slot->attach(operations); } } catch (...) { failure = std::current_exception(); }
   auto retired = std::shared_ptr<protocol_slot::generation>{};
   auto pending = std::shared_ptr<protocol_slot::generation>{};
   auto closing = false;
   {
      const auto lock = std::scoped_lock{mutex_};
      const auto row = find(slot->protocol());
      if (row != slots_.end() && row->slot == slot) {
         row->attaching = false;
         if (failure) {
            row->closing = true;
            retired = slot->replace({});
            pending = std::move(row->pending);
         }
         closing = row->closing;
      }
   }
   if (retired && retired->api) { request_close_api_publication_generation(retired->api); }
   if (pending && pending->api) { request_close_api_publication_generation(pending->api); }
   if (closing) {
      try { detach(slot); } catch (...) { if (!failure) { failure = std::current_exception(); } }
   }
   if (failure) { std::rethrow_exception(failure); }
}

void protocol_registry::attach(std::shared_ptr<const protocol_operations> operations) {
   auto slots = std::vector<std::shared_ptr<protocol_slot>>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      if (closed_) { return; }
      if (operations_) {
         if (operations_ == operations) { return; }
         FORGE_THROW_EXCEPTION(exceptions::route_conflict, "protocol registry already has native operations");
      }
      slots.reserve(slots_.size());
      for (auto& row : slots_) { if (!row.closing) { slots.push_back(row.slot); } }
      operations_ = operations;
      for (auto& row : slots_) { if (!row.closing) { row.attaching = true; } }
   }
   auto failure = std::exception_ptr{};
   for (const auto& slot : slots) {
      try { finish_attach(slot, operations); } catch (...) { if (!failure) { failure = std::current_exception(); } }
   }
   if (failure) { request_close(); std::rethrow_exception(failure); }
}

protocol_registration protocol_registry::register_raw(forge::net::p2p::protocol_id protocol,
    forge::net::p2p::node::protocol_handler handler, const protocol_registration* previous) {
   const auto state = lifecycle_.snapshot();
   if (!state.admission_open) { FORGE_THROW_EXCEPTION(exceptions::route_conflict, "protocol admission is closed"); }
   if (!handler) { FORGE_THROW_EXCEPTION(exceptions::route_conflict, "empty raw protocol handler"); }
   if (!previous) { retry_closing(protocol); }
   auto generation = std::make_shared<protocol_slot::generation>();
   generation->raw = std::move(handler);
   auto slot = std::shared_ptr<protocol_slot>{};
   auto retired = std::shared_ptr<protocol_slot::generation>{};
   auto operations = std::shared_ptr<const protocol_operations>{};
   auto result = protocol_registration{};
   auto fresh = false;
   {
      const auto lock = std::scoped_lock{mutex_};
      require_available(protocol);
      auto row = find(protocol);
      if (previous) {
         if (row == slots_.end() || row->closing || row->attaching || row->pending || !row->slot->current() ||
             row->slot->current()->api || !protocol_registration_access::matches(
                 *previous, shared_from_this(), row->slot->current())) {
            FORGE_THROW_EXCEPTION(exceptions::route_conflict, "raw replacement requires its active owner");
         }
         slot = row->slot;
      } else {
         if (row != slots_.end()) { FORGE_THROW_EXCEPTION(exceptions::route_conflict, "protocol is already claimed"); }
         if (slots_.size() >= max_slots_) { FORGE_THROW_EXCEPTION(exceptions::route_conflict, "protocol claim limit reached"); }
         slot = std::make_shared<protocol_slot>(std::move(protocol), lifecycle_);
         fresh = true;
      }
      const auto weak = weak_from_this();
      const auto weak_slot = std::weak_ptr{slot};
      const auto weak_generation = std::weak_ptr{generation};
      result = protocol_registration_access::make(weak, weak_generation,
          [weak, weak_slot, weak_generation] {
             if (const auto owner = weak.lock()) {
                if (const auto slot = weak_slot.lock()) {
                   owner->close_generation(slot, weak_generation.lock());
                }
             }
          }, [weak_slot, weak_generation] {
             const auto slot = weak_slot.lock();
             const auto generation = weak_generation.lock();
             return slot && generation && slot->current() == generation;
          });
      if (fresh) {
         slots_.push_back({.slot = slot, .pending = generation, .attaching = true});
         operations = operations_;
      } else { find(slot->protocol())->pending = generation; }
   }
   try {
      if (fresh) { finish_attach(slot, operations); }
      if (!lifecycle_.try_admit(state.generation, [&] {
         const auto lock = std::scoped_lock{mutex_};
         require_available(slot->protocol());
         const auto row = find(slot->protocol());
         if (row == slots_.end() || row->slot != slot || row->closing || row->attaching || row->pending != generation) {
            FORGE_THROW_EXCEPTION(exceptions::route_conflict, "protocol preparation was retired");
         }
         // Only the live pointer exchange is guarded. Both generations remain
         // pinned until native/user retirement can run outside these locks.
         retired = slot->replace(generation);
         row->pending.reset();
      })) {
         FORGE_THROW_EXCEPTION(exceptions::route_conflict, "protocol admission is closed");
      }
   } catch (...) {
      result.close();
      throw;
   }
   return result;
}

forge::api::p2p::publication protocol_registry::publish(forge::api::p2p::api_binding binding) {
   const auto state = lifecycle_.snapshot();
   if (!state.admission_open) { FORGE_THROW_EXCEPTION(exceptions::route_conflict, "API publication admission is closed"); }
   const auto protocol = binding.protocol();
   retry_closing(protocol);
   auto api = make_api_publication_generation(std::move(binding));
   auto generation = std::make_shared<protocol_slot::generation>();
   generation->api = api;
   auto slot = std::shared_ptr<protocol_slot>{};
   auto retired = std::shared_ptr<protocol_slot::generation>{};
   auto result = forge::api::p2p::publication{};
   auto operations = std::shared_ptr<const protocol_operations>{};
   auto fresh = false;
   {
      const auto lock = std::scoped_lock{mutex_};
      require_available(protocol);
      if (!executor_) { FORGE_THROW_EXCEPTION(exceptions::route_conflict, "API owner executor is unavailable"); }
      const auto row = find(protocol);
      if (row != slots_.end()) {
         if (row->closing || row->attaching || row->pending || !row->slot->current() || !row->slot->current()->api) {
            FORGE_THROW_EXCEPTION(exceptions::route_conflict, "API protocol is already claimed or closing");
         }
         slot = row->slot;
      } else {
         if (slots_.size() >= max_slots_) { FORGE_THROW_EXCEPTION(exceptions::route_conflict, "protocol claim limit reached"); }
         slot = std::make_shared<protocol_slot>(protocol, lifecycle_);
         fresh = true;
      }
      const auto weak = weak_from_this();
      const auto weak_slot = std::weak_ptr{slot};
      const auto weak_generation = std::weak_ptr{generation};
      const auto weak_api = std::weak_ptr{api};
      result = forge::api::p2p::detail::publication_access::make(executor_,
          [weak, weak_slot, weak_generation, weak_api] {
             if (const auto owner = weak.lock()) {
                if (const auto slot = weak_slot.lock()) {
                   owner->close_generation(slot, weak_generation.lock());
                }
             }
             if (const auto api = weak_api.lock()) { request_close_api_publication_generation(api); }
          }, [weak, weak_slot, weak_api]() -> boost::asio::awaitable<void> {
             const auto api = weak_api.lock();
             if (const auto owner = weak.lock()) {
                co_await owner->async_close_generation(weak_slot.lock(), api);
             } else if (api) { co_await async_close_api_publication_generation(api); }
          }, [weak_api] { const auto api = weak_api.lock(); return api && api_publication_generation_active(api); },
          api->stop_token());
      set_api_publication_generation_retirement(api, [weak](const auto* retired) {
         if (const auto owner = weak.lock()) { owner->retire_generation(retired); }
      });
      generations_.push_back({.identity = api.get(), .value = api});
      if (fresh) {
         slots_.push_back({.slot = slot, .pending = generation, .attaching = true});
         operations = operations_;
      } else { row->pending = generation; }
   }
   try {
      if (fresh) { finish_attach(slot, operations); }
      if (!lifecycle_.try_admit(state.generation, [&] {
         const auto lock = std::scoped_lock{mutex_};
         require_available(slot->protocol());
         const auto row = find(slot->protocol());
         if (row == slots_.end() || row->slot != slot || row->closing || row->attaching || row->pending != generation) {
            FORGE_THROW_EXCEPTION(exceptions::route_conflict, "API publication preparation was retired");
         }
         retired = slot->replace(generation);
         row->pending.reset();
      })) {
         FORGE_THROW_EXCEPTION(exceptions::route_conflict, "API publication admission is closed");
      }
   } catch (...) {
      result.close();
      throw;
   }
   if (retired && retired->api) { request_close_api_publication_generation(retired->api); }
   return result;
}

void protocol_registry::close_generation(std::shared_ptr<protocol_slot> slot,
    std::shared_ptr<protocol_slot::generation> generation) noexcept {
   auto retired = std::shared_ptr<protocol_slot::generation>{};
   auto cleanup = false;
   {
      const auto lock = std::scoped_lock{mutex_};
      const auto row = find(slot->protocol());
      if (row != slots_.end() && row->slot == slot) {
         if (generation && row->pending == generation) {
            row->pending.reset(); // generation pins its captures through this lock.
            if (!slot->current()) { row->closing = true; }
         }
         if (generation && slot->current() == generation) {
            retired = slot->replace({});
            row->closing = true;
         }
         cleanup = row->closing && !row->attaching;
      }
   }
   if (generation && generation->api) { request_close_api_publication_generation(generation->api); }
   if (cleanup) { try { detach(slot); } catch (...) { /* Retain the closing claim for an explicit retry. */ } }
}

boost::asio::awaitable<void> protocol_registry::async_close_generation(std::shared_ptr<protocol_slot> slot,
    std::shared_ptr<api_publication_generation> generation) {
   if (generation) {
      request_close_api_publication_generation(generation);
      co_await async_close_api_publication_generation(generation);
   }
   if (slot) { retry_closing(slot->protocol()); }
}

void protocol_registry::request_close() noexcept {
   auto closing = std::list<slot_record>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      closed_ = true;
      if (closing_) { return; }
      closing_ = true;
      closing.splice(closing.end(), slots_);
   }
   auto failure = std::exception_ptr{};
   auto failed = std::list<slot_record>{};
   while (!closing.empty()) {
      auto row = closing.begin();
      auto retired = row->slot->replace({});
      auto pending = std::move(row->pending);
      if (pending && pending->api) { request_close_api_publication_generation(pending->api); }
      if (retired && retired->api) { request_close_api_publication_generation(retired->api); }
      try { row->slot->detach(); closing.erase(row); }
      catch (...) {
         if (!failure) { failure = std::current_exception(); }
         row->closing = true;
         failed.splice(failed.end(), closing, row);
      }
   }
   auto operations = std::shared_ptr<const protocol_operations>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      slots_.splice(slots_.end(), failed);
      close_failure_ = failure;
      closing_ = false;
      operations = std::move(operations_);
      prune_generations_locked();
   }
   close_ready_.notify();
}

boost::asio::awaitable<void> protocol_registry::async_close() {
   request_close();
   for (;;) {
      const auto observed = close_ready_.epoch();
      auto pending = false;
      { const auto lock = std::scoped_lock{mutex_}; pending = closing_; }
      if (!pending) { break; }
      co_await close_ready_.async_wait(observed);
   }
   auto generations = std::vector<std::shared_ptr<api_publication_generation>>{};
   auto failure = std::exception_ptr{};
   {
      const auto lock = std::scoped_lock{mutex_};
      failure = close_failure_;
      for (const auto& row : generations_) { if (auto value = row.value.lock()) { generations.push_back(std::move(value)); } }
   }
   for (auto& generation : generations) { co_await async_close_api_publication_generation(generation); }
   if (failure) { std::rethrow_exception(failure); }
}

void protocol_registry::retire_generation(const api_publication_generation* generation) noexcept {
   const auto lock = std::scoped_lock{mutex_};
   generations_.remove_if([&](const auto& row) { return row.identity == generation; });
}
void protocol_registry::prune_generations_locked() noexcept {
   generations_.remove_if([](const auto& row) { return row.value.expired(); });
}

std::shared_ptr<protocol_registry> make_protocol_registry(std::size_t max_slots, forge::app::plugin_lifecycle lifecycle) {
   return std::make_shared<protocol_registry>(max_slots, std::move(lifecycle));
}
forge::api::p2p::publication publish_api_publication(const std::shared_ptr<protocol_registry>& registry,
    forge::api::p2p::api_binding binding) { return registry->publish(std::move(binding)); }
void bind_protocol_registry_executor(const std::shared_ptr<protocol_registry>& registry,
    boost::asio::any_io_executor executor) { registry->bind_executor(std::move(executor)); }
void attach_protocol_registry(const std::shared_ptr<protocol_registry>& registry,
    const std::shared_ptr<forge::net::p2p::node>& node) { registry->attach(make_protocol_operations(node)); }
void configure_protocol_registry(const std::shared_ptr<protocol_registry>& registry,
    std::vector<forge::net::p2p::protocol_id> protocols, std::size_t max_slots) {
   registry->configure(std::move(protocols), max_slots);
}
void request_close_protocol_registry(const std::shared_ptr<protocol_registry>& registry) noexcept { registry->request_close(); }
boost::asio::awaitable<void> async_close_protocol_registry(const std::shared_ptr<protocol_registry>& registry) {
   co_await registry->async_close();
}
}
