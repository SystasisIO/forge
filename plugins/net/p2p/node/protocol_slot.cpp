module;

#include <boost/asio/awaitable.hpp>
#include <atomic>
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
import forge.net.p2p.stream;

#include "details/protocol_slot.hxx"
#include "details/protocol_operations.hxx"
#include "details/api_publication_generation.hxx"

namespace forge::plugins::net::p2p::node::detail {
protocol_slot::protocol_slot(forge::net::p2p::protocol_id protocol) : protocol_{std::move(protocol)} {}
const forge::net::p2p::protocol_id& protocol_slot::protocol() const noexcept { return protocol_; }
std::shared_ptr<protocol_slot::generation> protocol_slot::current() const noexcept {
   return std::atomic_load_explicit(&current_, std::memory_order_seq_cst);
}
std::shared_ptr<protocol_slot::generation> protocol_slot::replace(std::shared_ptr<generation> next) noexcept {
   return std::atomic_exchange_explicit(&current_, std::move(next), std::memory_order_seq_cst);
}

void protocol_slot::attach(const std::shared_ptr<const protocol_operations>& operations) {
   auto admitted = std::shared_ptr<generation>{};
   const auto lock = std::scoped_lock{mutex_};
   if (operations_) { return; }
   admitted = current();
   if (!admitted) { return; }
   // The owner is retained before the fallible call: Identify Push may throw
   // after installing the handler. Only a normal unregister confirms absence.
   operations_ = operations;
   operations->install(protocol_, [weak = weak_from_this()](auto stream) {
      const auto slot = weak.lock();
      return accept(slot ? slot->current() : nullptr, std::move(stream));
   });
}

void protocol_slot::detach() {
   auto retired = std::shared_ptr<const protocol_operations>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      if (!operations_) { return; }
      static_cast<void>(operations_->remove(protocol_));
      retired = std::move(operations_);
   }
}

boost::asio::awaitable<void> protocol_slot::accept(std::shared_ptr<generation> owner,
    forge::net::p2p::node::incoming_protocol_stream stream) {
   if (!owner) {
      stream.stream.request_cancel();
      co_return;
   }
   if (owner->api) {
      co_await accept_api_publication_generation(owner->api, std::move(stream));
   } else {
      co_await owner->raw(std::move(stream));
   }
}
}
