module;

#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>
#include "details/connection_test_hooks.hxx"

module forge.net.tcp.connection;

import forge.asio.notification;
import forge.net.transport.stream;

#include "details/connection_impl.hxx"

namespace forge::net::tcp {
using asio_tcp = boost::asio::ip::tcp;
using detail::cancel_socket;

connection::connection() = default;
connection::connection(boost::asio::ip::tcp::socket socket, options tcp_options, std::shared_ptr<void> lifetime)
    : connection(std::move(socket), tcp_options, std::move(lifetime), {}) {}

connection::connection(boost::asio::ip::tcp::socket socket, options tcp_options, std::shared_ptr<void> lifetime,
                       std::shared_ptr<detail::connection_test_hooks> hooks) {
   auto owned = std::shared_ptr<asio_tcp::socket>{};
   try {
      owned = std::make_shared<asio_tcp::socket>(std::move(socket));
      if (hooks && hooks->startup) {
         hooks->startup(hooks->state.get(), detail::startup_stage::owner_allocation);
      }
      // Keep both the native socket and token here until all fallible startup
      // steps finish. Constructor unwinding must not acknowledge an open socket.
      impl_ = std::make_shared<impl>(owned, tcp_options, lifetime, hooks);
      impl_->start_terminal_worker();
   } catch (...) {
      if (impl_) {
         impl_->request_cancel();
         impl::close_on_owner(*owned, *impl_->terminal_state, hooks);
      } else {
         cancel_socket(owned ? *owned : socket, hooks);
      }
      throw;
   }
}
connection::~connection() = default;
connection::connection(connection&&) noexcept = default;
connection& connection::operator=(connection&&) noexcept = default;

bool connection::valid() const noexcept {
   return impl_ && impl_->valid();
}

transport::endpoint connection::local_endpoint() const {
   if (!valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
   }
   return impl_->local_endpoint();
}

transport::endpoint connection::remote_endpoint() const {
   if (!valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
   }
   return impl_->remote_endpoint();
}

boost::asio::awaitable<void> connection::async_write(std::span<const std::uint8_t> bytes) {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
   }
   auto state = impl_;
   co_await state->async_write(bytes);
}

boost::asio::awaitable<std::size_t> connection::async_read_some(std::span<std::uint8_t> bytes) {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
   }
   auto state = impl_;
   co_return co_await state->async_read_some(bytes);
}

boost::asio::awaitable<std::vector<std::uint8_t>> connection::async_read() {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
   }
   auto state = impl_;
   co_return co_await state->async_read();
}

boost::asio::awaitable<void> connection::async_close() {
   if (!impl_) {
      co_return;
   }
   auto state = impl_;
   co_await state->async_close();
}

void connection::cancel() {
   request_cancel();
}

void connection::request_cancel() noexcept {
   if (impl_) {
      impl_->request_cancel();
   }
}

transport::stream_connection connection::into_transport_stream() && {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
   }
   return impl_->into_transport_stream();
}

boost::asio::ip::tcp::socket connection::release_socket() && {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
   }
   return impl_->release_socket(nullptr);
}

boost::asio::ip::tcp::socket connection::release_socket(std::shared_ptr<void>& lifetime) && {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
   }
   return impl_->release_socket(&lifetime);
}

} // namespace forge::net::tcp
