module;

#include <forge/exceptions/macros.hpp>
#include "details/socket_reuse.hxx"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

module forge.net.tcp.connector;

import forge.asio.notification;

#include "details/connector_impl.hxx"

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
                         forge::exceptions::ctx("reason", error.message()),
                         forge::exceptions::ctx("native_error_value", error.value()),
                         forge::exceptions::ctx("native_error_category", error.category().name()));
}

[[noreturn]] void throw_connect_canceled(const transport::endpoint& endpoint) {
   FORGE_THROW_EXCEPTION(exceptions::canceled, "tcp connect canceled", forge::exceptions::ctx("host", endpoint.host),
                         forge::exceptions::ctx("port", endpoint.port));
}

void validate_options(const options& value) {
   if (value.read_chunk_size == 0) {
      throw_invalid_options("tcp read_chunk_size must be greater than zero");
   }
   if (value.connect_timeout.count() <= 0 || value.max_pending_connects == 0) {
      throw_invalid_options("tcp connect timeout and pending connect limit must be positive");
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

connector::impl::impl(boost::asio::any_io_executor executor_value, options tcp_options_value)
    : strand(asio::make_strand(std::move(executor_value))), tcp_options(tcp_options_value),
      sockets(std::make_shared<socket_map>()), resolvers(std::make_shared<resolver_map>()),
      terminal_requested(std::make_shared<forge::asio::notification>()),
      terminal_completed(std::make_shared<forge::asio::notification>()) {
   validate_options(tcp_options);
}

connector::impl::~impl() {
   request_cancel();
}

void connector::impl::start_terminal_worker() {
   // The composed operation owns only terminal resources, never connector::impl.
   // This keeps stop sticky without creating a worker/self ownership cycle.
   auto active_sockets = sockets;
   auto active_resolvers = resolvers;
   auto requested = terminal_requested;
   auto completed = terminal_completed;
   auto source = source_closed;
   auto owner = source_owner;
   auto weak_self = weak_from_this();
   asio::co_spawn(
       strand,
       [requested = std::move(requested), source = std::move(source), weak_self]() -> asio::awaitable<void> {
          co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
          if (source) {
             using namespace asio::experimental::awaitable_operators;
             static_cast<void>(co_await (requested->async_wait(0) || source->async_wait(0)));
          } else {
             static_cast<void>(co_await requested->async_wait(0));
          }
          // A scoped fault at the actual waiter boundary, not a global allocator.
          if (const auto self = weak_self.lock(); self && self->fail_terminal_wait_for_test.exchange(false)) {
             throw std::bad_alloc{};
          }
       },
       [active_sockets = std::move(active_sockets), active_resolvers = std::move(active_resolvers),
        owner = std::move(owner), completed = std::move(completed), weak_self](std::exception_ptr failure) mutable noexcept {
          const auto self = weak_self.lock();
          if (self) {
             self->canceled.store(true, std::memory_order_release);
             self->terminal_failure = std::move(failure);
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
          owner.reset();
          if (self) {
             self->terminal_worker_completed = true;
             self->finish_terminal();
          } else if (active_sockets->empty() && active_resolvers->empty()) {
             completed->notify();
          }
       });
}

void connector::impl::finish_terminal() noexcept {
   // Worker completion and attempt teardown both run on the owning strand.
   if (!terminal_worker_completed || terminal_published || !sockets->empty() || !resolvers->empty()) {
      return;
   }
   source_owner.reset();
   terminal_published = true;
   terminal_completed->notify();
}

[[nodiscard]] bool connector::impl::valid() const noexcept {
   return !canceled.load(std::memory_order_acquire) && (!source_open || source_open());
}

boost::asio::awaitable<connection> connector::impl::async_connect_connection(transport::endpoint remote,
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
          const auto deadline_at = std::chrono::steady_clock::now() + self->tcp_options.connect_timeout;
          if (self->sockets->size() >= self->tcp_options.max_pending_connects) {
             throw_invalid_options("tcp pending connect limit exceeded");
          }
          if (self->local && remote.host_type != transport::endpoint::host_kind::ip4 &&
              remote.host_type != transport::endpoint::host_kind::ip6) {
             throw_invalid_endpoint(remote, "coordinated tcp connect requires a literal remote address");
          }
          if (self->local && self->local->literal_address().is_v4() != remote.literal_address().is_v4()) {
             throw_invalid_endpoint(remote, "coordinated tcp address family mismatch");
          }

          const auto generation = self->next_generation++;
          auto socket = std::make_shared<asio_tcp::socket>(self->strand);
          self->sockets->emplace(generation, socket);
          const auto finish = [self, generation, &socket, &lifetime](impl*) noexcept {
             auto ignored = boost::system::error_code{};
             socket->close(ignored);
             lifetime.reset();
             self->resolvers->erase(generation);
             self->sockets->erase(generation);
             self->finish_terminal();
          };
          auto operation = std::unique_ptr<impl, decltype(finish)>{self.get(), finish};
          // Destroy the composed connect/timer state before publishing teardown.
          {
             auto deadline = asio::steady_timer{self->strand};
             deadline.expires_at(deadline_at);
             auto used_fallback = false;
             using namespace asio::experimental::awaitable_operators;
             auto connect = [&]() -> asio::awaitable<boost::system::error_code> {
                auto error = boost::system::error_code{};
                if (self->local) {
                   const auto local_address = self->local->literal_address();
                   const auto remote_address = remote.literal_address();
                   const auto destination = asio_tcp::endpoint{remote_address, remote.port};
                   socket->open(destination.protocol(), error);
                   if (!error) {
                      socket->set_option(asio::socket_base::reuse_address{true}, error);
                   }
                   if (!error) {
                      error = detail::enable_port_reuse(socket->native_handle());
                   }
                   if (error) {
                      co_return error;
                   }
                   socket->bind(asio_tcp::endpoint{local_address, self->local->port}, error);
                   if (!error) {
                      co_await socket->async_connect(destination, asio::redirect_error(asio::use_awaitable, error));
                   }
                   if (!error || self->reuse == reuse_policy::required ||
                       (error != boost::system::errc::address_in_use &&
                        error != boost::system::errc::address_not_available)) {
                      co_return error;
                   }

                   // Ordinary listener-port reuse is preferred, not mandatory.
                   // Close the failed native socket before opening its one
                   // unbound retry; keep the same generation, token and timer.
                   const auto descriptor = socket->native_handle();
                   auto close_error = boost::system::error_code{};
                   socket->close(close_error);
                   if (close_error) {
                      co_return close_error;
                   }
                   if (const auto observer = self->before_reuse_fallback_for_test) {
                      co_await observer(*socket, descriptor, error);
                   }
                   const auto cancellation = co_await asio::this_coro::cancellation_state;
                   if (!self->valid() || cancellation.cancelled() != asio::cancellation_type::none) {
                      co_return asio::error::operation_aborted;
                   }
                   if (std::chrono::steady_clock::now() >= deadline_at) {
                      co_return asio::error::timed_out;
                   }
                   used_fallback = true;
                   socket->open(destination.protocol(), error);
                   if (!error) {
                      co_await socket->async_connect(destination, asio::redirect_error(asio::use_awaitable, error));
                   }
                   co_return error;
                }
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
                co_return error;
             };
             auto result = co_await (connect() || deadline.async_wait(asio::use_awaitable));
             auto error = result.index() == 0 ? std::get<0>(result) : asio::error::timed_out;
             if (const auto gate = self->before_attempt_release_for_test) {
                co_await gate();
             }

             if (error) {
                if (error == asio::error::operation_aborted || self->canceled.load(std::memory_order_acquire)) {
                   throw_connect_canceled(remote);
                }
                throw_connect_failed(remote, error);
             }
             if (!self->valid()) {
                throw_connect_canceled(remote);
             }
             if (self->local && !used_fallback) {
                const auto actual = socket->local_endpoint();
                if (actual.address() != self->local->literal_address() || actual.port() != self->local->port) {
                   throw_invalid_endpoint(*self->local, "coordinated tcp local endpoint changed");
                }
             }

             configure_socket(*socket, self->tcp_options);
             co_return connection{std::move(*socket), self->tcp_options, std::move(lifetime)};
          }
       },
       asio::use_awaitable);
}

boost::asio::awaitable<transport::stream_connection> connector::impl::async_connect(transport::endpoint remote,
                                                                                    transport::connect_options) {
   auto tcp_connection = co_await async_connect_connection(std::move(remote), {});
   co_return std::move(tcp_connection).into_transport_stream();
}

void connector::impl::cancel() {
   request_cancel();
}

void connector::impl::request_cancel() noexcept {
   auto expected = false;
   if (!canceled.compare_exchange_strong(expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
      return;
   }
   terminal_requested->notify();
}

boost::asio::awaitable<void> connector::impl::async_stop() {
   request_cancel();
   auto self = shared_from_this();
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
   co_await terminal_completed->async_wait(0);
   co_await asio::co_spawn(
       strand,
       [self]() -> asio::awaitable<void> {
          if (self->terminal_failure) {
             std::rethrow_exception(self->terminal_failure);
          }
          co_return;
       },
       asio::use_awaitable);
}

} // namespace forge::net::tcp
