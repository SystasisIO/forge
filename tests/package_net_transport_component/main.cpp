#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/use_future.hpp>
#include <array>
#include <chrono>
#include <future>

import forge.net.transport.datagram_io;
import forge.net.transport.endpoint;

int main() {
   const auto endpoint = forge::net::transport::endpoint{.host = "127.0.0.1", .port = 9000};
   if (endpoint.port != 9000) {
      return 1;
   }
   namespace asio = boost::asio;
   namespace io = forge::net::transport::datagram_io;
   using udp = asio::ip::udp;
   using namespace std::chrono_literals;
   auto context = asio::io_context{};
   auto receiver = udp::socket{context, udp::endpoint{udp::v4(), 0}};
   auto sender = udp::socket{context, udp::endpoint{asio::ip::address_v4::loopback(), 0}};
   io::configure(receiver);
   io::configure(sender);
   auto exchange = [&]() -> asio::awaitable<bool> {
      auto payload = std::array<char, 1>{'x'};
      const auto destination = udp::endpoint{asio::ip::address_v4::loopback(), receiver.local_endpoint().port()};
      static_cast<void>(co_await io::async_send(sender, asio::buffer(payload), destination));
      const auto incoming = co_await io::async_receive(receiver, asio::buffer(payload));
      if (incoming.size != 1 || incoming.local != destination || incoming.interface_index == 0) {
         co_return false;
      }
      static_cast<void>(co_await io::async_send(receiver, asio::buffer(payload), incoming.remote,
          io::source{.address = incoming.local.address()}));
      const auto reply = co_await io::async_receive(sender, asio::buffer(payload));
      co_return reply.size == 1 && reply.remote == destination && payload[0] == 'x';
   };
   auto result = asio::co_spawn(context, exchange, asio::use_future);
   context.run_for(2s);
   if (result.wait_for(0s) != std::future_status::ready) {
      context.stop();
      return 2;
   }
   return result.get() ? 0 : 3;
}
