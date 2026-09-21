#include "details/server_udp_socket.hxx"
#include "details/quic_engine.hxx"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <utility>
#include <chrono>

import forge.asio.exceptions;

namespace forge::net::quic::detail {
namespace asio = boost::asio;
namespace datagram_io = forge::net::transport::datagram_io;
using udp = asio::ip::udp;

server_udp_socket::server_udp_socket(asio::strand<asio::io_context::executor_type> strand)
    : _strand(std::move(strand)), _socket(_strand) {}

void server_udp_socket::open_and_bind(const udp::endpoint& endpoint) {
   try {
      _socket.open(endpoint.protocol());
      datagram_io::configure(_socket);
      _socket.bind(endpoint);
      _bound_endpoint = _socket.local_endpoint();
      if (endpoint.address().is_v6()) {
         _interface_index = static_cast<std::uint32_t>(endpoint.address().to_v6().scope_id());
      }
   } catch (const boost::system::system_error& error) {
      throw engine_failure{engine_error_kind::internal_error, error.what()};
   }
}

udp::endpoint server_udp_socket::local_endpoint() const noexcept {
   auto result = _bound_endpoint;
   if (_interface_index != 0 && result.address().is_v6()) {
      auto address = result.address().to_v6();
      address.scope_id(_interface_index);
      result.address(address);
   }
   return result;
}

asio::awaitable<server_udp_socket::packet> server_udp_socket::async_receive() {
   co_return co_await asio::co_spawn(
       _strand,
       [self = shared_from_this()]() -> asio::awaitable<packet> {
          if (self->_stopped) {
             throw boost::system::system_error{asio::error::operation_aborted};
          }
          auto result = packet{.bytes = std::vector<std::uint8_t>(65'536)};
          result.route = co_await datagram_io::async_receive(self->_socket, asio::buffer(result.bytes));
          if (self->_interface_index != 0 && result.route.interface_index != self->_interface_index) {
             throw boost::system::system_error{asio::error::invalid_argument, "QUIC listener interface mismatch"};
          }
          result.bytes.resize(result.route.size);
          co_return result;
       },
       asio::use_awaitable);
}

asio::awaitable<boost::system::error_code> server_udp_socket::async_send(packet value) {
   co_return co_await asio::co_spawn(
       _strand,
       [self = shared_from_this(), value = std::move(value)]() mutable -> asio::awaitable<boost::system::error_code> {
          using namespace asio::experimental::awaitable_operators;
          auto deadline = asio::steady_timer{self->_strand};
          deadline.expires_after(std::chrono::seconds{5});
          auto result = co_await (self->send(std::move(value)) || deadline.async_wait(asio::use_awaitable));
          co_return result.index() == 0 ? std::get<0>(result) : asio::error::timed_out;
       },
       asio::use_awaitable);
}

asio::awaitable<boost::system::error_code> server_udp_socket::send(packet value) {
   try {
      auto ticket = co_await _send_gate.acquire();
      if (_stopped) {
         co_return asio::error::operation_aborted;
      }
      if (value.route.local.port() != _bound_endpoint.port() ||
          (!_bound_endpoint.address().is_unspecified() && _bound_endpoint.address() != value.route.local.address())) {
         co_return asio::error::invalid_argument;
      }
      // Ordinary unicast requires source affinity, not interface pinning.
      // An explicit bind scope or a scoped path makes the interface mandatory.
      auto index = _interface_index;
      for (const auto& address :
           {_bound_endpoint.address(), value.route.local.address(), value.route.remote.address()}) {
         if (address.is_v6() && address.to_v6().scope_id() != 0) {
            const auto scope = address.to_v6().scope_id();
            if (index != 0 && index != scope) {
               co_return asio::error::invalid_argument;
            }
            index = static_cast<std::uint32_t>(scope);
         }
      }
      if (index != 0 && value.route.interface_index != 0 && index != value.route.interface_index) {
         co_return asio::error::invalid_argument;
      }
      co_await datagram_io::async_send(
          _socket, asio::buffer(value.bytes), value.route.remote,
          datagram_io::source{.address = value.route.local.address(), .interface_index = index});
      co_return boost::system::error_code{};
   } catch (const boost::system::system_error& error) {
      co_return error.code();
   } catch (const forge::asio::exceptions::canceled&) {
      co_return asio::error::operation_aborted;
   } catch (const forge::asio::exceptions::rejected&) {
      co_return asio::error::operation_aborted;
   }
}

void server_udp_socket::stop() {
   asio::dispatch(_strand, [self = shared_from_this()] {
      if (self->_stopped) {
         return;
      }
      self->_stopped = true;
      self->_send_gate.close();
      auto ignored = boost::system::error_code{};
      self->_socket.cancel(ignored);
      self->_socket.close(ignored);
   });
}

} // namespace forge::net::quic::detail
