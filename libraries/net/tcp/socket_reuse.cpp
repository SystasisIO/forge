#include "details/socket_reuse.hxx"

#include <cerrno>
#include <cstring>
#include <memory>
#if defined(__APPLE__) || defined(__linux__) || defined(__FreeBSD__)
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace forge::net::tcp::detail {

boost::system::error_code enable_port_reuse(int native_socket) noexcept {
#if defined(SO_REUSEPORT)
   const auto enabled = 1;
   if (::setsockopt(native_socket, SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) == 0) {
      return {};
   }
   return {errno, boost::system::system_category()};
#else
   static_cast<void>(native_socket);
   return boost::system::errc::make_error_code(boost::system::errc::operation_not_supported);
#endif
}

bool is_assigned_local_address(const boost::asio::ip::address& address) noexcept {
   if (address.is_unspecified() || address.is_multicast()) {
      return false;
   }
#if defined(__APPLE__) || defined(__linux__) || defined(__FreeBSD__)
   ifaddrs* raw = nullptr;
   if (::getifaddrs(&raw) != 0) {
      return false;
   }
   const auto interfaces = std::unique_ptr<ifaddrs, decltype(&::freeifaddrs)>{raw, &::freeifaddrs};
   for (const auto* entry = raw; entry != nullptr; entry = entry->ifa_next) {
      if (entry->ifa_addr == nullptr || (entry->ifa_flags & IFF_UP) == 0) {
         continue;
      }
      if (address.is_v4() && entry->ifa_addr->sa_family == AF_INET) {
         const auto* local = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
         if (address.to_v4().to_uint() == ntohl(local->sin_addr.s_addr)) {
            return true;
         }
      } else if (address.is_v6() && entry->ifa_addr->sa_family == AF_INET6) {
         const auto value = address.to_v6();
         const auto bytes = value.to_bytes();
         const auto* local = reinterpret_cast<const sockaddr_in6*>(entry->ifa_addr);
         if (std::memcmp(bytes.data(), &local->sin6_addr, bytes.size()) == 0 &&
             (!value.is_link_local() || value.scope_id() != 0) &&
             (value.scope_id() == 0 || value.scope_id() == ::if_nametoindex(entry->ifa_name))) {
            return true;
         }
      }
   }
#endif
   return false;
}

} // namespace forge::net::tcp::detail
