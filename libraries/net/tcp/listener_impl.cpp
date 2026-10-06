module;

#include <forge/exceptions/macros.hpp>
#include "details/socket_reuse.hxx"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

module forge.net.tcp.listener;

import forge.asio.notification;

#include "details/listener_impl.hxx"

namespace forge::net::tcp {
namespace asio = boost::asio;
using asio_tcp = boost::asio::ip::tcp;

[[noreturn]] void listener::impl::throw_invalid_endpoint(const transport::endpoint& endpoint, std::string message) {
   FORGE_THROW_EXCEPTION(exceptions::invalid_endpoint, std::move(message),
                         forge::exceptions::ctx("host", endpoint.host), forge::exceptions::ctx("port", endpoint.port),
                         forge::exceptions::ctx("protocol", static_cast<int>(endpoint.protocol)));
}

[[noreturn]] void listener::impl::throw_invalid_options(std::string message) {
   FORGE_THROW_EXCEPTION(exceptions::invalid_options, std::move(message));
}

[[noreturn]] void listener::impl::throw_listen_failed(const transport::endpoint& endpoint,
                                                      const boost::system::error_code& error) {
   FORGE_THROW_EXCEPTION(exceptions::listen_failed, "tcp listen failed", forge::exceptions::ctx("host", endpoint.host),
                         forge::exceptions::ctx("port", endpoint.port),
                         forge::exceptions::ctx("reason", error.message()));
}

void listener::impl::validate_options(const options& value) {
   if (value.read_chunk_size == 0) {
      throw_invalid_options("tcp read_chunk_size must be greater than zero");
   }
}

asio_tcp::endpoint listener::impl::to_bind_endpoint(const transport::endpoint& endpoint) {
   if (endpoint.protocol != transport::endpoint::protocol_kind::tcp) {
      throw_invalid_endpoint(endpoint, "tcp listener requires tcp endpoint protocol");
   }
   if (endpoint.host.empty()) {
      throw_invalid_endpoint(endpoint, "tcp listener requires non-empty host");
   }
   if (endpoint.host.find('\0') != std::string::npos) {
      throw_invalid_endpoint(endpoint, "tcp listener host must not contain NUL");
   }

   switch (endpoint.host_type) {
   case transport::endpoint::host_kind::ip4:
   case transport::endpoint::host_kind::ip6:
      try {
         return asio_tcp::endpoint{endpoint.literal_address(), endpoint.port};
      } catch (const boost::system::system_error&) {
         throw_invalid_endpoint(endpoint, "tcp listener requires valid scoped literal host");
      }
   case transport::endpoint::host_kind::dns:
   case transport::endpoint::host_kind::dns4:
   case transport::endpoint::host_kind::dns6:
      throw_invalid_endpoint(endpoint, "tcp listener cannot bind DNS host kind");
   }
   throw_invalid_endpoint(endpoint, "tcp listener received unsupported host kind");
}

[[nodiscard]] transport::endpoint listener::impl::from_asio_endpoint(const asio_tcp::endpoint& endpoint) {
   return transport::endpoint::from_address(endpoint.address(), endpoint.port(),
                                            transport::endpoint::protocol_kind::tcp);
}

void listener::impl::configure_socket(asio_tcp::socket& socket, const options& tcp_options) {
   auto error = boost::system::error_code{};
   socket.set_option(asio_tcp::no_delay{tcp_options.no_delay}, error);
   if (error) {
      FORGE_THROW_EXCEPTION(exceptions::io_error, "failed to configure tcp no_delay",
                            forge::exceptions::ctx("reason", error.message()));
   }
   socket.set_option(boost::asio::socket_base::keep_alive{tcp_options.keep_alive}, error);
   if (error) {
      FORGE_THROW_EXCEPTION(exceptions::io_error, "failed to configure tcp keep_alive",
                            forge::exceptions::ctx("reason", error.message()));
   }
}

listener::impl::impl(boost::asio::any_io_executor executor, transport::endpoint requested,
                     transport::listen_options listen_options, options tcp_options_value)
    : strand(asio::make_strand(std::move(executor))), acceptor(strand), tcp_options(tcp_options_value) {
   validate_options(tcp_options);
   if (listen_options.limits.max_connections == 0) {
      throw_invalid_options("tcp listener max_connections must be greater than zero");
   }

   const auto bind_endpoint = to_bind_endpoint(requested);
   auto error = boost::system::error_code{};
   acceptor.open(bind_endpoint.protocol(), error);
   if (error) {
      throw_listen_failed(requested, error);
   }
   acceptor.set_option(boost::asio::socket_base::reuse_address{tcp_options.reuse_address}, error);
   if (error) {
      throw_listen_failed(requested, error);
   }
   if (tcp_options.reuse_port) {
      if (!tcp_options.reuse_address) {
         throw_invalid_options("coordinated tcp reuse requires reuse_address");
      }
      error = detail::enable_port_reuse(acceptor.native_handle());
      if (error) {
         throw_listen_failed(requested, error);
      }
   }
   acceptor.bind(bind_endpoint, error);
   if (error) {
      throw_listen_failed(requested, error);
   }
   const auto backlog = static_cast<int>(
       std::min<std::size_t>(listen_options.limits.max_connections,
                             static_cast<std::size_t>(boost::asio::socket_base::max_listen_connections)));
   acceptor.listen(backlog, error);
   if (error) {
      throw_listen_failed(requested, error);
   }
   local = from_asio_endpoint(acceptor.local_endpoint(error));
   if (error) {
      throw_listen_failed(requested, error);
   }
}

[[nodiscard]] bool listener::impl::valid() const noexcept {
   return state.load(std::memory_order_acquire) == state_value::open;
}

listener::impl::~impl() {
   reuse_closed->notify();
}

[[nodiscard]] transport::endpoint listener::impl::local_endpoint() const {
   if (!valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp listener");
   }
   return local;
}

boost::asio::awaitable<connection> listener::impl::async_accept_connection(std::shared_ptr<void> lifetime) {
   auto self = shared_from_this();
   co_return co_await asio::co_spawn(
       strand,
       [self = std::move(self), lifetime = std::move(lifetime)]() mutable -> asio::awaitable<connection> {
          if (!self->valid()) {
             FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp listener");
          }
          ++self->active_accepts;
          const auto finish = [self, &lifetime](impl*) noexcept {
             lifetime.reset();
             --self->active_accepts;
             self->accepts_changed.notify();
          };
          auto operation = std::unique_ptr<impl, decltype(finish)>{self.get(), finish};
          auto socket = asio_tcp::socket{self->acceptor.get_executor()};
          auto error = boost::system::error_code{};
          co_await self->acceptor.async_accept(socket, asio::redirect_error(asio::use_awaitable, error));
          if (error) {
             if (error == asio::error::operation_aborted) {
                if (self->state.load(std::memory_order_acquire) != state_value::open) {
                   FORGE_THROW_EXCEPTION(exceptions::closed, "tcp listener closed during accept");
                }
                FORGE_THROW_EXCEPTION(exceptions::canceled, "tcp listener accept canceled");
             }
             FORGE_THROW_EXCEPTION(exceptions::accept_failed, "tcp accept failed",
                                   forge::exceptions::ctx("reason", error.message()));
          }

          configure_socket(socket, self->tcp_options);
          co_return connection{std::move(socket), self->tcp_options, std::move(lifetime)};
       },
       asio::use_awaitable);
}

boost::asio::awaitable<transport::stream_connection> listener::impl::async_accept() {
   auto tcp_connection = co_await async_accept_connection({});
   co_return std::move(tcp_connection).into_transport_stream();
}

boost::asio::awaitable<void> listener::impl::async_close() {
   static_cast<void>(request_close());
   auto self = shared_from_this();
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
   co_await asio::co_spawn(
       strand,
       [self = std::move(self)]() -> asio::awaitable<void> {
          self->close_on_owner();
          auto observed = self->accepts_changed.epoch();
          while (self->active_accepts != 0) {
             observed = co_await self->accepts_changed.async_wait(observed);
          }
          auto drains = std::vector<std::shared_ptr<forge::asio::notification>>{};
          {
             auto lock = std::scoped_lock{self->reuse_mutex};
             for (const auto& weak : self->connector_drains) {
                if (auto signal = weak.lock()) {
                   drains.push_back(std::move(signal));
                }
             }
          }
          for (const auto& signal : drains) {
             co_await signal->async_wait(0);
          }
          co_return;
       },
       asio::use_awaitable);
}

void listener::impl::close() {
   if (request_close()) {
      auto self = shared_from_this();
      asio::post(strand, [self = std::move(self)] { self->close_on_owner(); });
   }
}

void listener::impl::cancel() {
   auto self = shared_from_this();
   asio::post(strand, [self = std::move(self)] {
      if (self->state.load(std::memory_order_acquire) != state_value::open) {
         return;
      }
      auto ignored = boost::system::error_code{};
      self->acceptor.cancel(ignored);
   });
}

[[nodiscard]] bool listener::impl::request_close() noexcept {
   auto expected = state_value::open;
   const auto changed = state.compare_exchange_strong(expected, state_value::close_requested, std::memory_order_acq_rel,
                                                      std::memory_order_acquire);
   if (changed) {
      reuse_closed->notify();
   }
   return changed;
}

void listener::impl::close_on_owner() noexcept {
   auto expected = state_value::close_requested;
   if (!state.compare_exchange_strong(expected, state_value::closed, std::memory_order_acq_rel,
                                      std::memory_order_acquire)) {
      return;
   }
   auto ignored = boost::system::error_code{};
   acceptor.close(ignored);
}
} // namespace forge::net::tcp
