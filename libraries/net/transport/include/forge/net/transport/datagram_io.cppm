module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>

export module forge.net.transport.datagram_io;

export namespace forge::net::transport::datagram_io {

struct received {
   std::size_t size = 0;
   boost::asio::ip::udp::endpoint remote;
   boost::asio::ip::udp::endpoint local;
   std::uint32_t interface_index = 0;
};

struct source {
   boost::asio::ip::address address;
   std::uint32_t interface_index = 0;
};

// macOS/Linux only. Call on an open socket before starting I/O; enables
// destination/interface ancillary data and native nonblocking mode.
// Failure throws boost::system::system_error; configuration is not transactional.
void configure(boost::asio::ip::udp::socket& socket);

// Receive requires a configured, bound socket. Socket and buffer storage remain
// caller-owned until completion. Run on the
// socket's owning executor/strand, serializing configuration, initiation and
// close. Allow at most one receive and one send at a time (one of each may
// overlap); do not mix with other socket I/O. Serialize sends at the caller.
// Missing/malformed packet info or payload/control truncation consumes the
// datagram and throws. Zero-byte datagrams are successful, not EOF.
// Cancellation is checked before native I/O and propagated through async_wait;
// a completed native send/receive is not rolled back. No threads or queues.
[[nodiscard]] boost::asio::awaitable<received>
async_receive(boost::asio::ip::udp::socket& socket, boost::asio::mutable_buffer buffer);

// nullopt deliberately selects the kernel route/source, without connecting the
// socket. It is not suitable for replies that must use received route metadata.
// An explicit source must be unicast, never an unspecified/multicast destination
// copied from a received multicast packet. The bound socket supplies the port.
// IPv6 scope and interface_index must agree; link-local routes require a scope.
// IPv4-mapped IPv6 endpoints stay IPv6; families are not implicitly converted.
// Same ownership/executor/cancellation contract as async_receive. Socket-wide
// cancel affects every waiter; use per-operation cancellation slots when shared.
// Darwin IPv4 source+interface requires an exact source bind or an already
// matching persistent IP_BOUND_IF; otherwise operation_not_supported precedes
// sendmsg. Source-only (index zero) does not guarantee an egress interface.
// No send changes socket options. Owners must not reconfigure during pending I/O.
[[nodiscard]] boost::asio::awaitable<std::size_t>
async_send(boost::asio::ip::udp::socket& socket, boost::asio::const_buffer buffer,
           boost::asio::ip::udp::endpoint remote, std::optional<source> local = std::nullopt);

} // namespace forge::net::transport::datagram_io
