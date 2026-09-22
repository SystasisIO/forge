module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

module forge.net.tcp.connector;

import forge.asio.notification;

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

[[noreturn]] void throw_connect_failed(const transport::endpoint& endpoint, const boost::system::error_code& error) {
   FORGE_THROW_EXCEPTION(exceptions::connect_failed, "tcp connect failed",
                         forge::exceptions::ctx("host", endpoint.host), forge::exceptions::ctx("port", endpoint.port),
                         forge::exceptions::ctx("reason", error.message()));
}

[[noreturn]] void throw_connect_canceled(const transport::endpoint& endpoint) {
   FORGE_THROW_EXCEPTION(exceptions::canceled, "tcp connect canceled", forge::exceptions::ctx("host", endpoint.host),
                         forge::exceptions::ctx("port", endpoint.port));
}

void validate_options(const options& value) {
   if (value.read_chunk_size == 0) {
      throw_invalid_options("tcp read_chunk_size must be greater than zero");
   }
}

void validate_remote_endpoint(const transport::endpoint& endpoint) {
   if (endpoint.protocol != transport::endpoint::protocol_kind::tcp) {
      throw_invalid_endpoint(endpoint, "tcp connector requires tcp endpoint protocol");
   }
   if (endpoint.host.empty()) {
      throw_invalid_endpoint(endpoint, "tcp connector requires non-empty host");
   }
   if (endpoint.host.find('\0') != std::string::npos) {
      throw_invalid_endpoint(endpoint, "tcp connector host must not contain NUL");
   }
   if (endpoint.port == 0) {
      throw_invalid_endpoint(endpoint, "tcp connector requires non-zero remote port");
   }

   switch (endpoint.host_type) {
   case transport::endpoint::host_kind::ip4:
   case transport::endpoint::host_kind::ip6:
      try {
         static_cast<void>(endpoint.literal_address());
      } catch (const boost::system::system_error&) {
         throw_invalid_endpoint(endpoint, "tcp connector requires valid scoped literal host");
      }
      return;
   case transport::endpoint::host_kind::dns:
   case transport::endpoint::host_kind::dns4:
   case transport::endpoint::host_kind::dns6:
      if (!endpoint.zone.empty()) {
         throw_invalid_endpoint(endpoint, "tcp connector cannot resolve DNS host with zone");
      }
      return;
   }
   throw_invalid_endpoint(endpoint, "tcp connector received unsupported host kind");
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

[[nodiscard]] std::vector<asio_tcp::endpoint> filter_results(asio_tcp::resolver::results_type results,
                                                             transport::endpoint::host_kind host_type) {
   auto out = std::vector<asio_tcp::endpoint>{};
   for (const auto& entry : results) {
      const auto endpoint = entry.endpoint();
      if (host_type == transport::endpoint::host_kind::dns4 && !endpoint.address().is_v4()) {
         continue;
      }
      if (host_type == transport::endpoint::host_kind::dns6 && !endpoint.address().is_v6()) {
         continue;
      }
      out.push_back(endpoint);
   }
   return out;
}

} // namespace

struct connector::impl final : transport::detail::stream_connector_concept,
                               std::enable_shared_from_this<connector::impl> {
   impl(boost::asio::any_io_executor executor_value, options tcp_options_value)
       : strand(asio::make_strand(std::move(executor_value))), tcp_options(tcp_options_value),
         sockets(std::make_shared<socket_map>()), resolvers(std::make_shared<resolver_map>()),
         terminal_requested(std::make_shared<forge::asio::notification>()),
         terminal_completed(std::make_shared<forge::asio::notification>()) {
      validate_options(tcp_options);
   }

   ~impl() override {
      request_cancel();
   }

   using socket_map = std::map<std::uint64_t, std::shared_ptr<asio_tcp::socket>>;
   using resolver_map = std::map<std::uint64_t, std::shared_ptr<asio_tcp::resolver>>;

   void start_terminal_worker() {
      // The composed operation owns only terminal resources, never connector::impl.
      // This keeps stop sticky without creating a worker/self ownership cycle.
      auto active_sockets = sockets;
      auto active_resolvers = resolvers;
      auto requested = terminal_requested;
      auto completed = terminal_completed;
      asio::co_spawn(
          strand,
          [active_sockets = std::move(active_sockets), active_resolvers = std::move(active_resolvers),
           requested = std::move(requested), completed]() mutable -> asio::awaitable<void> {
             try {
                static_cast<void>(co_await requested->async_wait(0));
             } catch (...) {
                // Failure to arm is terminal: owner cleanup still runs below.
             }
             for (const auto& [_, resolver] : *active_resolvers) {
                try {
                   resolver->cancel();
                } catch (...) {
                }
             }
             for (const auto& [_, socket] : *active_sockets) {
                auto ignored = boost::system::error_code{};
                socket->cancel(ignored);
             }
             completed->notify();
          },
          [completed = std::move(completed)](std::exception_ptr) noexcept { completed->notify(); });
   }

   [[nodiscard]] bool valid() const noexcept override {
      return !canceled.load(std::memory_order_acquire);
   }

   boost::asio::awaitable<connection> async_connect_connection(transport::endpoint remote,
                                                               std::shared_ptr<void> lifetime) {
      auto self = shared_from_this();
      co_return co_await asio::co_spawn(
          strand,
          [self = std::move(self), remote = std::move(remote),
           lifetime = std::move(lifetime)]() mutable -> asio::awaitable<connection> {
             if (!self->valid()) {
                FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connector");
             }
             validate_remote_endpoint(remote);

             const auto generation = self->next_generation++;
             auto socket = std::make_shared<asio_tcp::socket>(self->strand);
             self->sockets->emplace(generation, socket);
             try {
                auto error = boost::system::error_code{};
                if (remote.host_type == transport::endpoint::host_kind::ip4 ||
                    remote.host_type == transport::endpoint::host_kind::ip6) {
                   const auto address = [&remote] {
                      try {
                         return remote.literal_address();
                      } catch (const boost::system::system_error&) {
                         throw_invalid_endpoint(remote, "tcp connector requires valid scoped literal host");
                      }
                   }();
                   co_await socket->async_connect(asio_tcp::endpoint{address, remote.port},
                                                  asio::redirect_error(asio::use_awaitable, error));
                } else {
                   auto resolver = std::make_shared<asio_tcp::resolver>(self->strand);
                   self->resolvers->emplace(generation, resolver);
                   const auto service = std::to_string(remote.port);
                   auto results = co_await resolver->async_resolve(remote.host, service,
                                                                   asio::redirect_error(asio::use_awaitable, error));
                   self->resolvers->erase(generation);
                   if (!error) {
                      auto filtered = filter_results(std::move(results), remote.host_type);
                      if (filtered.empty()) {
                         error = asio::error::host_not_found;
                      } else {
                         co_await asio::async_connect(*socket, filtered,
                                                      asio::redirect_error(asio::use_awaitable, error));
                      }
                   }
                }

                if (error) {
                   if (error == asio::error::operation_aborted || self->canceled.load(std::memory_order_acquire)) {
                      throw_connect_canceled(remote);
                   }
                   throw_connect_failed(remote, error);
                }
                if (self->canceled.load(std::memory_order_acquire)) {
                   throw_connect_canceled(remote);
                }

                configure_socket(*socket, self->tcp_options);
                self->sockets->erase(generation);
                co_return connection{std::move(*socket), self->tcp_options, std::move(lifetime)};
             } catch (...) {
                self->resolvers->erase(generation);
                self->sockets->erase(generation);
                throw;
             }
          },
          asio::use_awaitable);
   }

   boost::asio::awaitable<transport::stream_connection> async_connect(transport::endpoint remote,
                                                                      transport::connect_options) override {
      auto tcp_connection = co_await async_connect_connection(std::move(remote), {});
      co_return std::move(tcp_connection).into_transport_stream();
   }

   void cancel() override {
      request_cancel();
   }

   void request_cancel() noexcept {
      auto expected = false;
      if (!canceled.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
         return;
      }
      terminal_requested->notify();
   }

   asio::strand<asio::any_io_executor> strand;
   options tcp_options;
   std::shared_ptr<socket_map> sockets;
   std::shared_ptr<resolver_map> resolvers;
   std::shared_ptr<forge::asio::notification> terminal_requested;
   std::shared_ptr<forge::asio::notification> terminal_completed;
   std::uint64_t next_generation = 1;
   std::atomic_bool canceled = false;
};

connector::connector() = default;
connector::connector(boost::asio::any_io_executor executor, options tcp_options)
    : impl_(std::make_shared<impl>(std::move(executor), tcp_options)) {
   impl_->start_terminal_worker();
}
connector::~connector() = default;
connector::connector(connector&&) noexcept = default;
connector& connector::operator=(connector&&) noexcept = default;

bool connector::valid() const noexcept {
   return impl_ && impl_->valid();
}

boost::asio::awaitable<connection> connector::async_connect_connection(transport::endpoint remote,
                                                                       transport::connect_options,
                                                                       std::shared_ptr<void> lifetime) {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connector");
   }
   auto state = impl_;
   co_return co_await state->async_connect_connection(std::move(remote), std::move(lifetime));
}

boost::asio::awaitable<transport::stream_connection> connector::async_connect(transport::endpoint remote,
                                                                              transport::connect_options options) {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connector");
   }
   auto state = impl_;
   co_return co_await state->async_connect(std::move(remote), options);
}

void connector::cancel() {
   request_cancel();
}

void connector::request_cancel() noexcept {
   if (impl_) {
      impl_->request_cancel();
   }
}

transport::stream_connector connector::as_transport() const {
   if (!valid()) {
      return {};
   }
   return transport::detail::stream_connector_access::make(impl_);
}

} // namespace forge::net::tcp
