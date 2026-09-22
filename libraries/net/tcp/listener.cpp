module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

module forge.net.tcp.listener;

namespace forge::net::tcp {
namespace {

namespace asio = boost::asio;
using asio_tcp = boost::asio::ip::tcp;

[[noreturn]] void throw_invalid_endpoint(const transport::endpoint& endpoint, std::string message) {
   FORGE_THROW_EXCEPTION(exceptions::invalid_endpoint, std::move(message),
                         forge::exceptions::ctx("host", endpoint.host), forge::exceptions::ctx("port", endpoint.port),
                         forge::exceptions::ctx("protocol", static_cast<int>(endpoint.protocol)));
}

[[noreturn]] void throw_invalid_options(std::string message) {
   FORGE_THROW_EXCEPTION(exceptions::invalid_options, std::move(message));
}

[[noreturn]] void throw_listen_failed(const transport::endpoint& endpoint, const boost::system::error_code& error) {
   FORGE_THROW_EXCEPTION(exceptions::listen_failed, "tcp listen failed", forge::exceptions::ctx("host", endpoint.host),
                         forge::exceptions::ctx("port", endpoint.port),
                         forge::exceptions::ctx("reason", error.message()));
}

void validate_options(const options& value) {
   if (value.read_chunk_size == 0) {
      throw_invalid_options("tcp read_chunk_size must be greater than zero");
   }
}

asio_tcp::endpoint to_bind_endpoint(const transport::endpoint& endpoint) {
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

[[nodiscard]] transport::endpoint from_asio_endpoint(const asio_tcp::endpoint& endpoint) {
   return transport::endpoint::from_address(endpoint.address(), endpoint.port(),
                                            transport::endpoint::protocol_kind::tcp);
}

void configure_socket(asio_tcp::socket& socket, const options& tcp_options) {
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

enum class listener_state : std::uint8_t {
   open,
   close_requested,
   closed,
};

} // namespace

struct listener::impl final : transport::detail::stream_listener_concept, std::enable_shared_from_this<listener::impl> {
   impl(boost::asio::any_io_executor executor, transport::endpoint requested, transport::listen_options listen_options,
        options tcp_options_value)
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

   [[nodiscard]] bool valid() const noexcept override {
      return state.load(std::memory_order_acquire) == listener_state::open;
   }

   [[nodiscard]] transport::endpoint local_endpoint() const override {
      if (!valid()) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp listener");
      }
      return local;
   }

   boost::asio::awaitable<connection> async_accept_connection(std::shared_ptr<void> lifetime) {
      auto self = shared_from_this();
      co_return co_await asio::co_spawn(
          strand,
          [self = std::move(self), lifetime = std::move(lifetime)]() mutable -> asio::awaitable<connection> {
             if (!self->valid()) {
                FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp listener");
             }
             auto socket = asio_tcp::socket{self->acceptor.get_executor()};
             auto error = boost::system::error_code{};
             co_await self->acceptor.async_accept(socket, asio::redirect_error(asio::use_awaitable, error));
             if (error) {
                if (error == asio::error::operation_aborted) {
                   if (self->state.load(std::memory_order_acquire) != listener_state::open) {
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

   boost::asio::awaitable<transport::stream_connection> async_accept() override {
      auto tcp_connection = co_await async_accept_connection({});
      co_return std::move(tcp_connection).into_transport_stream();
   }

   boost::asio::awaitable<void> async_close() override {
      static_cast<void>(request_close());
      auto self = shared_from_this();
      co_await asio::co_spawn(
          strand,
          [self = std::move(self)]() -> asio::awaitable<void> {
             self->close_on_owner();
             co_return;
          },
          asio::use_awaitable);
   }

   void close() {
      if (request_close()) {
         auto self = shared_from_this();
         asio::post(strand, [self = std::move(self)] { self->close_on_owner(); });
      }
   }

   void cancel() override {
      auto self = shared_from_this();
      asio::post(strand, [self = std::move(self)] {
         if (self->state.load(std::memory_order_acquire) != listener_state::open) {
            return;
         }
         auto ignored = boost::system::error_code{};
         self->acceptor.cancel(ignored);
      });
   }

   [[nodiscard]] bool request_close() noexcept {
      auto expected = listener_state::open;
      return state.compare_exchange_strong(expected, listener_state::close_requested, std::memory_order_acq_rel,
                                           std::memory_order_acquire);
   }

   void close_on_owner() noexcept {
      auto expected = listener_state::close_requested;
      if (!state.compare_exchange_strong(expected, listener_state::closed, std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
         return;
      }
      auto ignored = boost::system::error_code{};
      acceptor.close(ignored);
   }

   asio::strand<asio::any_io_executor> strand;
   asio_tcp::acceptor acceptor;
   transport::endpoint local;
   options tcp_options;
   std::atomic<listener_state> state{listener_state::open};
};

listener::listener() = default;
listener::listener(boost::asio::any_io_executor executor, transport::endpoint local,
                   transport::listen_options listen_options, options tcp_options)
    : impl_(std::make_shared<impl>(std::move(executor), std::move(local), listen_options, tcp_options)) {}
listener::~listener() = default;
listener::listener(listener&&) noexcept = default;
listener& listener::operator=(listener&&) noexcept = default;

bool listener::valid() const noexcept {
   return impl_ && impl_->valid();
}

transport::endpoint listener::local_endpoint() const {
   if (!valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp listener");
   }
   return impl_->local_endpoint();
}

boost::asio::awaitable<connection> listener::async_accept_connection(std::shared_ptr<void> lifetime) {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp listener");
   }
   auto state = impl_;
   co_return co_await state->async_accept_connection(std::move(lifetime));
}

boost::asio::awaitable<transport::stream_connection> listener::async_accept() {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp listener");
   }
   auto state = impl_;
   co_return co_await state->async_accept();
}

boost::asio::awaitable<void> listener::async_close() {
   if (!impl_) {
      co_return;
   }
   auto state = impl_;
   co_await state->async_close();
}

void listener::close() {
   if (impl_) {
      impl_->close();
   }
}

void listener::cancel() {
   if (impl_) {
      impl_->cancel();
   }
}

transport::stream_listener listener::as_transport() const {
   if (!valid()) {
      return {};
   }
   return transport::detail::stream_listener_access::make(impl_);
}

} // namespace forge::net::tcp
