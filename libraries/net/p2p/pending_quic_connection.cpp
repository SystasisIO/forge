module;

#include <forge/exceptions/macros.hpp>

#include <mutex>
#include <optional>
#include <utility>

#include <boost/asio/awaitable.hpp>

module forge.net.p2p.node;

import forge.net.quic.connection;
import forge.asio.notification;
import forge.net.p2p.exceptions;

#include "details/pending_quic_connection.hxx"

namespace forge::net::p2p::direct::detail {

void pending_quic_connection::install(forge::net::quic::connection value) noexcept {
   {
      const auto lock = std::scoped_lock{mutex_};
      if (canceled_) {
         value.request_cancel();
      }
      value_.emplace(std::move(value));
      installed_ = true;
   }
   changed_.notify();
}

bool pending_quic_connection::try_install(forge::net::quic::connection& value) noexcept {
   {
      const auto lock = std::scoped_lock{mutex_};
      if (canceled_ || installed_) {
         return false;
      }
      value_.emplace(std::move(value));
      installed_ = true;
   }
   changed_.notify();
   return true;
}

boost::asio::awaitable<void> pending_quic_connection::async_wait() {
   for (;;) {
      const auto epoch = changed_.epoch();
      {
         const auto lock = std::scoped_lock{mutex_};
         if (canceled_) {
            FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P coordinated QUIC inbound wait canceled");
         }
         if (value_) {
            co_return;
         }
      }
      co_await changed_.async_wait(epoch);
   }
}

forge::net::quic::connection* pending_quic_connection::get() noexcept {
   const auto lock = std::scoped_lock{mutex_};
   return value_ ? &*value_ : nullptr;
}

forge::net::quic::connection pending_quic_connection::take() noexcept {
   const auto lock = std::scoped_lock{mutex_};
   if (!value_) {
      return {};
   }
   auto result = std::move(*value_);
   value_.reset();
   return result;
}

void pending_quic_connection::request_cancel() noexcept {
   {
      const auto lock = std::scoped_lock{mutex_};
      canceled_ = true;
      if (value_) {
         value_->request_cancel();
      }
   }
   changed_.notify();
}

} // namespace forge::net::p2p::direct::detail
