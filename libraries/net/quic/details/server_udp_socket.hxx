#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/strand.hpp>
#include <cstdint>
#include <memory>
#include <vector>

import forge.asio.gate;
import forge.net.transport.datagram_io;

namespace forge::net::quic::detail {

struct server_udp_socket : std::enable_shared_from_this<server_udp_socket> {
   struct packet {
      std::vector<std::uint8_t> bytes;
      forge::net::transport::datagram_io::received route;
   };

   explicit server_udp_socket(boost::asio::strand<boost::asio::io_context::executor_type> strand);
   void open_and_bind(const boost::asio::ip::udp::endpoint& endpoint);
   [[nodiscard]] boost::asio::ip::udp::endpoint local_endpoint() const noexcept;
   boost::asio::awaitable<packet> async_receive();
   boost::asio::awaitable<boost::system::error_code> async_send(packet value);
   void stop();

 private:
   boost::asio::awaitable<boost::system::error_code> send(packet value);
   boost::asio::strand<boost::asio::io_context::executor_type> _strand;
   boost::asio::ip::udp::socket _socket;
   boost::asio::ip::udp::endpoint _bound_endpoint;
   std::uint32_t _interface_index = 0;
   forge::asio::gate _send_gate;
   bool _stopped = false;
};

} // namespace forge::net::quic::detail
