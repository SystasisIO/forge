module;

#include <boost/asio/awaitable.hpp>

#include <atomic>
#include <cstddef>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <utility>

module forge.plugins.net.p2p.node.plugin;

import forge.api.p2p.binding;
import forge.api.stream.session;
import forge.asio.notification;
import forge.net.p2p.node;
import forge.net.p2p.protocol;

#include "details/api_publication_slot.hxx"
#include "details/api_publication_generation.hxx"

namespace forge::plugins::net::p2p::node::detail {

api_publication_slot::api_publication_slot(forge::net::p2p::protocol_id protocol)
    : protocol_{std::move(protocol)} {}

api_publication_slot::~api_publication_slot() {
   detach();
}

const forge::net::p2p::protocol_id& api_publication_slot::protocol() const noexcept {
   return protocol_;
}

forge::net::p2p::node::protocol_handler api_publication_slot::handler() {
   return [self = shared_from_this()](forge::net::p2p::node::incoming_protocol_stream stream)
              -> boost::asio::awaitable<void> {
      co_await self->accept(std::move(stream));
      co_return;
   };
}

std::shared_ptr<api_publication_generation>
api_publication_slot::replace(std::shared_ptr<api_publication_generation> generation) noexcept {
   return std::atomic_exchange_explicit(&current_, std::move(generation), std::memory_order_acq_rel);
}

std::shared_ptr<api_publication_generation>
api_publication_slot::clear_if(const std::shared_ptr<api_publication_generation>& generation) noexcept {
   auto expected = generation;
   auto empty = std::shared_ptr<api_publication_generation>{};
   if (std::atomic_compare_exchange_strong_explicit(&current_, &expected, empty, std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
      return generation;
   }
   return {};
}

std::shared_ptr<api_publication_generation> api_publication_slot::clear() noexcept {
   return std::atomic_exchange_explicit(&current_, std::shared_ptr<api_publication_generation>{},
                                        std::memory_order_acq_rel);
}

void api_publication_slot::attach(const std::shared_ptr<forge::net::p2p::node>& node) {
   const auto lock = std::scoped_lock{mutex_};
   if (registered_) {
      return;
   }
   node->register_protocol_handler(protocol_, handler());
   node_ = node;
   registered_ = true;
}

void api_publication_slot::detach() noexcept {
   auto node = std::shared_ptr<forge::net::p2p::node>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      if (!registered_) {
         return;
      }
      node = std::move(node_);
      registered_ = false;
   }
   if (!node) {
      return;
   }
   try {
      static_cast<void>(node->unregister_protocol_handler(protocol_));
   } catch (...) {
      // Publication teardown must not prevent cancellation of its sessions.
   }
}

boost::asio::awaitable<void>
api_publication_slot::accept(forge::net::p2p::node::incoming_protocol_stream stream) {
   const auto generation = std::atomic_load_explicit(&current_, std::memory_order_acquire);
   if (!generation) {
      stream.stream.request_cancel();
      co_return;
   }
   co_await accept_api_publication_generation(generation, std::move(stream));
   co_return;
}

std::shared_ptr<api_publication_slot>
make_api_publication_slot(forge::net::p2p::protocol_id protocol) {
   return std::make_shared<api_publication_slot>(std::move(protocol));
}

const forge::net::p2p::protocol_id&
api_publication_slot_protocol(const std::shared_ptr<api_publication_slot>& slot) noexcept {
   return slot->protocol();
}

std::shared_ptr<api_publication_generation>
replace_api_publication_slot(const std::shared_ptr<api_publication_slot>& slot,
                             std::shared_ptr<api_publication_generation> generation) noexcept {
   return slot->replace(std::move(generation));
}

std::shared_ptr<api_publication_generation>
clear_api_publication_slot_if(const std::shared_ptr<api_publication_slot>& slot,
                              const std::shared_ptr<api_publication_generation>& generation) noexcept {
   return slot->clear_if(generation);
}

std::shared_ptr<api_publication_generation>
clear_api_publication_slot(const std::shared_ptr<api_publication_slot>& slot) noexcept {
   return slot->clear();
}

void attach_api_publication_slot(const std::shared_ptr<api_publication_slot>& slot,
                                 const std::shared_ptr<forge::net::p2p::node>& node) {
   slot->attach(node);
}

void detach_api_publication_slot(const std::shared_ptr<api_publication_slot>& slot) noexcept {
   slot->detach();
}

} // namespace forge::plugins::net::p2p::node::detail
