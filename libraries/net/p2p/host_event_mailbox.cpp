module;

#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <boost/asio/awaitable.hpp>
#include <boost/scope/scope_exit.hpp>
#include <forge/exceptions/macros.hpp>

module forge.net.p2p.node;

import forge.asio.notification;
import forge.net.p2p.exceptions;
import forge.net.p2p.host_event;

#include "details/host_event_mailbox.hxx"

namespace forge::net::p2p::detail {

void host_event_mailbox::publish(std::shared_ptr<const host_event> value) noexcept {
   {
      const auto lock = std::scoped_lock{mutex};
      if (closed) {
         return;
      }
      resync = resync || static_cast<bool>(pending);
      pending = std::move(value);
   }
   changed.notify();
}

void host_event_mailbox::close() noexcept {
   {
      const auto lock = std::scoped_lock{mutex};
      closed = true;
      pending.reset();
   }
   changed.notify();
}

bool host_event_mailbox::active() const noexcept {
   const auto lock = std::scoped_lock{mutex};
   return !closed;
}

boost::asio::awaitable<std::optional<host_event>>
host_event_mailbox::read(std::shared_ptr<host_event_mailbox> self) {
   if (!self) {
      co_return std::nullopt;
   }
   {
      const auto lock = std::scoped_lock{self->mutex};
      if (self->reading) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P host events permit one reader per subscription");
      }
      self->reading = true;
   }
   const auto reading = boost::scope::scope_exit{[&] {
      const auto lock = std::scoped_lock{self->mutex};
      self->reading = false;
   }};
   for (;;) {
      const auto epoch = self->changed.epoch();
      {
         const auto lock = std::scoped_lock{self->mutex};
         if (self->closed) {
            co_return std::nullopt;
         }
         if (self->pending) {
            auto result = *self->pending;
            result.resync_required = self->resync;
            self->pending.reset();
            self->resync = false;
            co_return result;
         }
      }
      static_cast<void>(co_await self->changed.async_wait(epoch));
   }
}

} // namespace forge::net::p2p::detail
