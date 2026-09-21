module;

#if defined(__APPLE__) && !defined(__APPLE_USE_RFC_3542)
#define __APPLE_USE_RFC_3542
#endif

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#endif

module forge.net.transport.datagram_io;

namespace forge::net::transport::datagram_io {
namespace {

namespace asio = boost::asio;
using udp = asio::ip::udp;

[[noreturn]] void invalid(const char* message) {
   throw boost::system::system_error{asio::error::invalid_argument, message};
}

[[noreturn]] void truncated() {
   throw boost::system::system_error{asio::error::message_size, "truncated UDP payload or packet info"};
}

void check_cancelled(const asio::cancellation_state& state) {
   if (state.cancelled() != asio::cancellation_type::none) {
      throw boost::system::system_error{asio::error::operation_aborted};
   }
}

#if defined(__APPLE__) || defined(__linux__)
[[noreturn]] void native_error(const char* operation) {
   throw boost::system::system_error{{errno, boost::system::generic_category()}, operation};
}

void enable(udp::socket& socket, int level, int option) {
   const int value = 1;
   if (::setsockopt(socket.native_handle(), level, option, &value, sizeof(value)) != 0) {
      native_error("UDP packet-info setsockopt");
   }
}

udp::endpoint remote_endpoint(const sockaddr_storage& address, socklen_t length) {
   if ((address.ss_family != AF_INET && address.ss_family != AF_INET6) ||
       length != (address.ss_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6))) {
      invalid("invalid UDP sender sockaddr");
   }
   auto result = udp::endpoint{};
   std::memcpy(result.data(), &address, length);
   result.resize(length);
   return result;
}

received packet_info(const msghdr& message, std::size_t size, const udp::endpoint& bound,
                     const sockaddr_storage& sender) {
   if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
      truncated();
   }
   auto result = received{.size = size, .remote = remote_endpoint(sender, message.msg_namelen)};
   auto found = false;
   auto offset = std::size_t{};
   const auto* control = static_cast<const unsigned char*>(message.msg_control);
   while (offset < message.msg_controllen) {
      const auto remaining = message.msg_controllen - offset;
      if (remaining < sizeof(cmsghdr)) {
         invalid("short UDP ancillary header");
      }
      auto header = cmsghdr{};
      std::memcpy(&header, control + offset, sizeof(header));
      if (header.cmsg_len < CMSG_LEN(0) || header.cmsg_len > remaining) {
         invalid("invalid UDP ancillary length");
      }
      const auto* data = control + offset + CMSG_LEN(0);
      if (header.cmsg_level == IPPROTO_IP && header.cmsg_type == IP_PKTINFO) {
         if (found || bound.address().is_v6() || header.cmsg_len != CMSG_LEN(sizeof(in_pktinfo))) {
            invalid("invalid IPv4 UDP packet info");
         }
         auto info = in_pktinfo{};
         std::memcpy(&info, data, sizeof(info));
         asio::ip::address_v4::bytes_type bytes{};
         std::memcpy(bytes.data(), &info.ipi_addr, bytes.size());
         result.local = udp::endpoint{asio::ip::address_v4{bytes}, bound.port()};
         if (info.ipi_ifindex <= 0) {
            invalid("missing UDP receive interface");
         }
         result.interface_index = static_cast<std::uint32_t>(info.ipi_ifindex);
         found = true;
      } else if (header.cmsg_level == IPPROTO_IPV6 && header.cmsg_type == IPV6_PKTINFO) {
         if (found || !bound.address().is_v6() || header.cmsg_len != CMSG_LEN(sizeof(in6_pktinfo))) {
            invalid("invalid IPv6 UDP packet info");
         }
         auto info = in6_pktinfo{};
         std::memcpy(&info, data, sizeof(info));
         asio::ip::address_v6::bytes_type bytes{};
         std::memcpy(bytes.data(), &info.ipi6_addr, bytes.size());
         auto address = asio::ip::address_v6{bytes};
         result.interface_index = info.ipi6_ifindex;
         if (address.is_link_local() || address.is_multicast()) {
            address.scope_id(result.interface_index);
         }
         result.local = udp::endpoint{address, bound.port()};
         found = true;
      }
      const auto payload_size = header.cmsg_len - CMSG_LEN(0);
      const auto step = CMSG_SPACE(payload_size);
      if (step < header.cmsg_len) {
         invalid("UDP ancillary alignment overflow");
      }
      // The final message may omit its alignment padding.
      if (step >= remaining) {
         offset = message.msg_controllen;
      } else {
         offset += step;
      }
   }
   if (!found || result.interface_index == 0 || result.local.address().is_unspecified() ||
       result.remote.address().is_v4() != result.local.address().is_v4()) {
      invalid("missing or inconsistent UDP destination/interface packet info");
   }
   if (result.remote.address().is_v6()) {
      auto address = result.remote.address().to_v6();
      if (address.is_link_local()) {
         if (address.scope_id() != 0 && address.scope_id() != result.interface_index) {
            invalid("UDP sender scope disagrees with receive interface");
         }
         address.scope_id(result.interface_index);
         result.remote.address(address);
      }
   }
   return result;
}

#if defined(__APPLE__)
unsigned ipv4_send_index(udp::socket& socket, const udp::endpoint& bound, const source& local) {
   if (local.interface_index == 0) {
      return 0;
   }
   // udp_output delegates source selection to in_pcbladdr when pktinfo names
   // an interface; in_pcbladdr retains an explicitly bound inp_laddr.
   if (bound.address() == local.address && bound.port() != 0) {
      return local.interface_index;
   }
   auto index = unsigned{};
   auto length = static_cast<socklen_t>(sizeof(index));
   if (::getsockopt(socket.native_handle(), IPPROTO_IP, IP_BOUND_IF, &index, &length) != 0) {
      native_error("UDP read IP_BOUND_IF");
   }
   if (length == sizeof(index) && index == local.interface_index) {
      // Existing sticky scope selects the interface; ancillary data selects
      // the exact source. Never mutate the shared socket's receive policy.
      return 0;
   }
   throw boost::system::system_error{asio::error::operation_not_supported,
       "Darwin IPv4 source+interface requires exact source bind or matching persistent IP_BOUND_IF"};
}
#endif
#endif

void validate_source(const udp::endpoint& bound, udp::endpoint& remote, source& local) {
   if (local.address.is_unspecified() || local.address.is_multicast() || remote.address().is_unspecified() ||
       remote.port() == 0 || bound.address().is_v4() != local.address.is_v4() ||
       local.address.is_v4() != remote.address().is_v4()) {
      invalid("UDP send requires an explicit unicast source and matching address families");
   }
   if (local.address.is_v4()) {
      if (local.address.to_v4() == asio::ip::address_v4::broadcast()) {
         invalid("UDP send source cannot be broadcast");
      }
      if (local.interface_index > static_cast<std::uint32_t>((std::numeric_limits<int>::max)())) {
         invalid("UDP interface index exceeds native range");
      }
      return;
   }
   auto address = local.address.to_v6();
   if (address.scope_id() > (std::numeric_limits<std::uint32_t>::max)()) {
      invalid("UDP source scope exceeds native range");
   }
   if (address.scope_id() != 0) {
      if (local.interface_index != 0 && local.interface_index != address.scope_id()) {
         invalid("UDP source scope disagrees with send interface");
      }
      local.interface_index = static_cast<std::uint32_t>(address.scope_id());
   }
   auto destination = remote.address().to_v6();
   if (destination.scope_id() > (std::numeric_limits<std::uint32_t>::max)()) {
      invalid("UDP destination scope exceeds native range");
   }
   if (destination.scope_id() != 0) {
      if (local.interface_index != 0 && destination.scope_id() != local.interface_index) {
         invalid("UDP destination scope disagrees with send interface");
      }
      local.interface_index = static_cast<std::uint32_t>(destination.scope_id());
   }
   if ((address.is_link_local() || destination.is_link_local() || destination.is_multicast_link_local()) &&
       local.interface_index == 0) {
      invalid("link-local UDP send requires an interface");
   }
   if (destination.is_link_local() || destination.is_multicast_link_local()) {
      destination.scope_id(local.interface_index);
      remote.address(destination);
   }
}

} // namespace

void configure(udp::socket& socket) {
#if defined(__APPLE__) || defined(__linux__)
   const auto bound = socket.local_endpoint();
   if (bound.address().is_v4()) {
      enable(socket, IPPROTO_IP, IP_PKTINFO);
   } else {
      enable(socket, IPPROTO_IPV6, IPV6_RECVPKTINFO);
   }
   socket.native_non_blocking(true);
#else
   throw boost::system::system_error{asio::error::operation_not_supported};
#endif
}

asio::awaitable<received> async_receive(udp::socket& socket, asio::mutable_buffer buffer) {
   const auto cancellation = co_await asio::this_coro::cancellation_state;
   check_cancelled(cancellation);
#if defined(__APPLE__) || defined(__linux__)
   const auto bound = socket.local_endpoint();
   if (bound.port() == 0) {
      invalid("UDP receive requires a bound socket");
   }
   for (;;) {
      check_cancelled(cancellation);
      auto sender = sockaddr_storage{};
      alignas(cmsghdr) auto control = std::array<unsigned char, 256>{};
      // Darwin may short-circuit a zero-capacity recvmsg without consuming the
      // datagram or filling metadata. Always supply storage, then enforce the
      // caller's actual capacity (including zero) after the receive.
      unsigned char scratch{};
      auto vector = iovec{.iov_base = buffer.size() == 0 ? &scratch : buffer.data(),
                          .iov_len = buffer.size() == 0 ? 1 : buffer.size()};
      auto message = msghdr{};
      message.msg_name = &sender;
      message.msg_namelen = sizeof(sender);
      message.msg_iov = &vector;
      message.msg_iovlen = 1;
      message.msg_control = control.data();
      message.msg_controllen = control.size();
      const auto size = ::recvmsg(socket.native_handle(), &message, MSG_DONTWAIT);
      if (size >= 0) {
         if (static_cast<std::size_t>(size) > buffer.size() || message.msg_controllen > control.size()) {
            truncated();
         }
         co_return packet_info(message, static_cast<std::size_t>(size), bound, sender);
      }
      if (errno == EINTR) {
         continue;
      }
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
         native_error("UDP recvmsg");
      }
      co_await socket.async_wait(udp::socket::wait_read, asio::use_awaitable);
   }
#else
   throw boost::system::system_error{asio::error::operation_not_supported};
#endif
}

asio::awaitable<std::size_t> async_send(udp::socket& socket, asio::const_buffer buffer,
                                      udp::endpoint remote, std::optional<source> local) {
   const auto cancellation = co_await asio::this_coro::cancellation_state;
   check_cancelled(cancellation);
#if defined(__APPLE__) || defined(__linux__)
   const auto bound = socket.local_endpoint();
   if (local) {
      validate_source(bound, remote, *local);
   } else if (remote.address().is_unspecified() || remote.port() == 0 ||
              bound.address().is_v4() != remote.address().is_v4() ||
              (remote.address().is_v6() &&
               (remote.address().to_v6().scope_id() > (std::numeric_limits<std::uint32_t>::max)() ||
                ((remote.address().to_v6().is_link_local() || remote.address().to_v6().is_multicast_link_local()) &&
                 remote.address().to_v6().scope_id() == 0)))) {
      invalid("invalid UDP kernel-route destination");
   }
   alignas(cmsghdr) auto control = std::array<unsigned char, CMSG_SPACE(sizeof(in6_pktinfo)) + CMSG_SPACE(sizeof(in_pktinfo))>{};
   auto vector = iovec{.iov_base = const_cast<void*>(buffer.data()), .iov_len = buffer.size()};
   auto message = msghdr{};
   message.msg_name = remote.data();
   message.msg_namelen = static_cast<socklen_t>(remote.size());
   message.msg_iov = &vector;
   message.msg_iovlen = 1;
   message.msg_control = local ? control.data() : nullptr;
   auto* header = reinterpret_cast<cmsghdr*>(control.data());
   if (local && local->address.is_v4()) {
      header->cmsg_level = IPPROTO_IP;
      header->cmsg_type = IP_PKTINFO;
      header->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
      message.msg_controllen = CMSG_SPACE(sizeof(in_pktinfo));
      auto info = in_pktinfo{};
#if defined(__APPLE__)
      info.ipi_ifindex = ipv4_send_index(socket, bound, *local);
#else
      info.ipi_ifindex = static_cast<decltype(info.ipi_ifindex)>(local->interface_index);
#endif
      const auto bytes = local->address.to_v4().to_bytes();
      std::memcpy(&info.ipi_spec_dst, bytes.data(), bytes.size());
      std::memcpy(CMSG_DATA(header), &info, sizeof(info));
   } else if (local) {
      header->cmsg_level = IPPROTO_IPV6;
      header->cmsg_type = IPV6_PKTINFO;
      header->cmsg_len = CMSG_LEN(sizeof(in6_pktinfo));
      message.msg_controllen = CMSG_SPACE(sizeof(in6_pktinfo));
      auto info = in6_pktinfo{};
      info.ipi6_ifindex = local->interface_index;
      const auto bytes = local->address.to_v6().to_bytes();
      std::memcpy(&info.ipi6_addr, bytes.data(), bytes.size());
      std::memcpy(CMSG_DATA(header), &info, sizeof(info));
   }
   for (;;) {
      check_cancelled(cancellation);
      const auto size = ::sendmsg(socket.native_handle(), &message, MSG_DONTWAIT);
      if (size >= 0) {
         if (static_cast<std::size_t>(size) != buffer.size()) {
            throw boost::system::system_error{asio::error::message_size, "partial UDP send"};
         }
         co_return static_cast<std::size_t>(size);
      }
      if (errno == EINTR) {
         continue;
      }
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
         native_error("UDP sendmsg");
      }
      co_await socket.async_wait(udp::socket::wait_write, asio::use_awaitable);
   }
#else
   throw boost::system::system_error{asio::error::operation_not_supported};
#endif
}

} // namespace forge::net::transport::datagram_io
