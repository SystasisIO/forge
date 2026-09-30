module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <exception>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <utility>

module forge.plugins.net.p2p.node.plugin;

import forge.api.p2p.binding;
import forge.api.p2p.publication;
import forge.api.stream.session;
import forge.asio.notification;
import forge.net.p2p.node;
import forge.net.p2p.protocol;
import forge.plugins.net.p2p.node.exceptions;

#include "details/api_publication_registry.hxx"
#include "details/api_publication_generation.hxx"
#include "details/api_publication_slot.hxx"

namespace forge::plugins::net::p2p::node::detail {

api_publication_registry::api_publication_registry() = default;

api_publication_registry::~api_publication_registry() {
   request_close();
}

forge::api::p2p::publication
api_publication_registry::publish(forge::api::p2p::api_binding binding) {
   const auto protocol = binding.protocol();
   auto generation = make_api_publication_generation(std::move(binding));
   auto slot = std::shared_ptr<api_publication_slot>{};
   auto previous = std::shared_ptr<api_publication_generation>{};
   auto publication = forge::api::p2p::publication{};
   auto owner_executor = boost::asio::any_io_executor{};
   auto attach_failure = std::exception_ptr{};
   auto failed_slot = std::shared_ptr<api_publication_slot>{};

   {
      const auto lock = std::scoped_lock{mutex_};
      if (closed_) {
         FORGE_THROW_EXCEPTION(exceptions::route_conflict, "P2P API publications are closed");
      }
      if (!owner_executor_) {
         FORGE_THROW_EXCEPTION(exceptions::route_conflict,
                               "P2P API publication owner executor is unavailable");
      }
      owner_executor = owner_executor_;

      const auto existing = find_active(protocol);
      if (existing != active_slots_.end()) {
         slot = existing->slot;
      } else {
         if (contains_locked(protocol)) {
            FORGE_THROW_EXCEPTION(exceptions::route_conflict, "P2P API publication is closing");
         }
         slot = make_api_publication_slot(protocol);
      }

      const auto owner = std::weak_ptr<api_publication_registry>{shared_from_this()};
      const auto published_slot = std::weak_ptr<api_publication_slot>{slot};
      const auto published_generation = std::weak_ptr<api_publication_generation>{generation};
      publication = forge::api::p2p::detail::publication_access::make(
         std::move(owner_executor),
         [owner, published_slot, published_generation] {
            const auto current = published_generation.lock();
            if (!current) {
               return;
            }
            if (const auto registry = owner.lock()) {
               if (const auto active_slot = published_slot.lock()) {
                  registry->close_generation(active_slot, current);
                  return;
               }
            }
            request_close_api_publication_generation(current);
         },
         [owner, published_generation]() -> boost::asio::awaitable<void> {
            const auto current = published_generation.lock();
            if (!current) {
               co_return;
            }
            if (const auto registry = owner.lock()) {
               co_await registry->async_close_generation(current);
               co_return;
            }
            co_await async_close_api_publication_generation(current);
         },
         [published_generation] {
            const auto current = published_generation.lock();
            return current && api_publication_generation_active(current);
         });
      set_api_publication_generation_retirement(
         generation, [owner](const api_publication_generation* retired) {
            if (const auto registry = owner.lock()) {
               registry->retire_generation(retired);
            }
         });

      if (existing != active_slots_.end()) {
         generations_.emplace_back(generation_record{.identity = generation.get(), .value = generation});
         previous = replace_api_publication_slot(slot, generation);
      } else {
         const auto slot_position = active_slots_.emplace(active_slots_.end(), slot_record{.slot = slot});
         auto generation_position = generations_.end();
         auto generation_registered = false;
         try {
            generation_position = generations_.emplace(
               generations_.end(), generation_record{.identity = generation.get(), .value = generation});
            generation_registered = true;
            static_cast<void>(replace_api_publication_slot(slot, generation));
            if (node_) {
               attach_api_publication_slot(slot, node_);
            }
         } catch (...) {
            static_cast<void>(clear_api_publication_slot(slot));
            active_slots_.erase(slot_position);
            if (generation_registered) {
               generations_.erase(generation_position);
            }
            failed_slot = slot;
            attach_failure = std::current_exception();
         }
      }
   }

   if (attach_failure) {
      request_close_api_publication_generation(generation);
      detach_api_publication_slot(failed_slot);
      std::rethrow_exception(attach_failure);
   }
   if (previous) {
      request_close_api_publication_generation(previous);
   }
   return publication;
}

void api_publication_registry::bind_executor(boost::asio::any_io_executor owner_executor) {
   if (!owner_executor) {
      FORGE_THROW_EXCEPTION(exceptions::route_conflict,
                            "P2P API publication owner executor is unavailable");
   }

   const auto lock = std::scoped_lock{mutex_};
   if (closed_) {
      FORGE_THROW_EXCEPTION(exceptions::route_conflict, "P2P API publications are closed");
   }
   if (!owner_executor_) {
      owner_executor_ = std::move(owner_executor);
   }
}

void api_publication_registry::attach(std::shared_ptr<forge::net::p2p::node> node) {
   const auto lock = std::scoped_lock{mutex_};
   if (closed_) {
      return;
   }
   if (node_) {
      if (node_ == node) {
         return;
      }
      FORGE_THROW_EXCEPTION(exceptions::route_conflict, "P2P API publications already have a node");
   }

   auto current = active_slots_.begin();
   try {
      for (; current != active_slots_.end(); ++current) {
         attach_api_publication_slot(current->slot, node);
      }
   } catch (...) {
      for (auto rollback = active_slots_.begin(); rollback != current; ++rollback) {
         detach_api_publication_slot(rollback->slot);
      }
      throw;
   }
   node_ = std::move(node);
}

bool api_publication_registry::contains(const forge::net::p2p::protocol_id& protocol) const noexcept {
   const auto lock = std::scoped_lock{mutex_};
   return contains_locked(protocol);
}

void api_publication_registry::close_generation(std::shared_ptr<api_publication_slot> slot,
                                                std::shared_ptr<api_publication_generation> generation) noexcept {
   auto closing = std::shared_ptr<api_publication_slot>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      for (auto current = active_slots_.begin(); current != active_slots_.end(); ++current) {
         if (current->slot != slot) {
            continue;
         }
         if (clear_api_publication_slot_if(current->slot, generation)) {
            closing = current->slot;
            closing_slots_.splice(closing_slots_.end(), active_slots_, current);
         }
         break;
      }
   }

   request_close_api_publication_generation(generation);
   if (!closing) {
      return;
   }
   detach_api_publication_slot(closing);

   const auto lock = std::scoped_lock{mutex_};
   for (auto current = closing_slots_.begin(); current != closing_slots_.end(); ++current) {
      if (current->slot == closing) {
         closing_slots_.erase(current);
         break;
      }
   }
}

boost::asio::awaitable<void>
api_publication_registry::async_close_generation(std::shared_ptr<api_publication_generation> generation) {
   request_close_api_publication_generation(generation);
   co_await async_close_api_publication_generation(std::move(generation));
   {
      const auto lock = std::scoped_lock{mutex_};
      prune_generations_locked();
   }
}

void api_publication_registry::request_close() noexcept {
   auto closing = std::list<slot_record>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      if (closed_) {
         return;
      }
      closed_ = true;
      closing.splice(closing.end(), active_slots_);
      closing.splice(closing.end(), closing_slots_);
   }

   for (auto& record : closing) {
      if (const auto generation = clear_api_publication_slot(record.slot)) {
         request_close_api_publication_generation(generation);
      }
      detach_api_publication_slot(record.slot);
   }

   {
      const auto lock = std::scoped_lock{mutex_};
      node_.reset();
      prune_generations_locked();
      close_complete_ = true;
   }
   close_ready_.notify();
}

boost::asio::awaitable<void> api_publication_registry::async_close() {
   request_close();
   co_await wait_for_close();
   auto generations = std::list<std::shared_ptr<api_publication_generation>>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      prune_generations_locked();
      for (const auto& record : generations_) {
         if (const auto generation = record.value.lock()) {
            generations.push_back(generation);
         }
      }
   }
   for (auto& generation : generations) {
      co_await async_close_api_publication_generation(std::move(generation));
   }
}

boost::asio::awaitable<void> api_publication_registry::wait_for_close() {
   for (;;) {
      const auto observed = close_ready_.epoch();
      {
         const auto lock = std::scoped_lock{mutex_};
         if (close_complete_) {
            co_return;
         }
      }
      co_await close_ready_.async_wait(observed);
   }
}

std::list<api_publication_registry::slot_record>::iterator
api_publication_registry::find_active(const forge::net::p2p::protocol_id& protocol) noexcept {
   for (auto current = active_slots_.begin(); current != active_slots_.end(); ++current) {
      if (api_publication_slot_protocol(current->slot) == protocol) {
         return current;
      }
   }
   return active_slots_.end();
}

std::list<api_publication_registry::slot_record>::const_iterator
api_publication_registry::find_active(const forge::net::p2p::protocol_id& protocol) const noexcept {
   for (auto current = active_slots_.cbegin(); current != active_slots_.cend(); ++current) {
      if (api_publication_slot_protocol(current->slot) == protocol) {
         return current;
      }
   }
   return active_slots_.cend();
}

bool api_publication_registry::contains_locked(const forge::net::p2p::protocol_id& protocol) const noexcept {
   if (find_active(protocol) != active_slots_.cend()) {
      return true;
   }
   for (const auto& record : closing_slots_) {
      if (api_publication_slot_protocol(record.slot) == protocol) {
         return true;
      }
   }
   return false;
}

void api_publication_registry::retire_generation(const api_publication_generation* generation) noexcept {
   const auto lock = std::scoped_lock{mutex_};
   for (auto current = generations_.begin(); current != generations_.end(); ++current) {
      if (current->identity == generation) {
         generations_.erase(current);
         return;
      }
   }
}

void api_publication_registry::prune_generations_locked() noexcept {
   for (auto current = generations_.begin(); current != generations_.end();) {
      if (current->value.expired()) {
         current = generations_.erase(current);
      } else {
         ++current;
      }
   }
}

std::shared_ptr<api_publication_registry> make_api_publication_registry() {
   return std::make_shared<api_publication_registry>();
}

forge::api::p2p::publication
publish_api_publication(const std::shared_ptr<api_publication_registry>& registry,
                        forge::api::p2p::api_binding binding) {
   if (!registry) {
      FORGE_THROW_EXCEPTION(exceptions::route_conflict, "P2P API publications are unavailable");
   }
   return registry->publish(std::move(binding));
}

void bind_api_publication_registry_executor(
   const std::shared_ptr<api_publication_registry>& registry,
   boost::asio::any_io_executor owner_executor) {
   if (!registry) {
      FORGE_THROW_EXCEPTION(exceptions::route_conflict, "P2P API publications are unavailable");
   }
   registry->bind_executor(std::move(owner_executor));
}

void attach_api_publication_registry(const std::shared_ptr<api_publication_registry>& registry,
                                     const std::shared_ptr<forge::net::p2p::node>& node) {
   if (!registry) {
      FORGE_THROW_EXCEPTION(exceptions::route_conflict, "P2P API publications are unavailable");
   }
   registry->attach(node);
}

bool api_publication_registry_contains(const std::shared_ptr<api_publication_registry>& registry,
                                       const forge::net::p2p::protocol_id& protocol) noexcept {
   return registry && registry->contains(protocol);
}

void request_close_api_publication_registry(
   const std::shared_ptr<api_publication_registry>& registry) noexcept {
   if (registry) {
      registry->request_close();
   }
}

boost::asio::awaitable<void> async_close_api_publication_registry(
   const std::shared_ptr<api_publication_registry>& registry) {
   if (registry) {
      co_await registry->async_close();
   }
}

} // namespace forge::plugins::net::p2p::node::detail
