#include "details/interface_watcher.hxx"

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>
#include <fcntl.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <net/if_dl.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet6/in6_var.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <linux/if_addr.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#endif

namespace forge::net::p2p::detail {
namespace {

namespace asio = boost::asio;
using bytes = std::span<const std::uint8_t>;
using record = interface_state::interface;

void native_error(const char* operation) {
   throw boost::system::system_error{errno, boost::system::generic_category(), operation};
}

void malformed() {
   throw boost::system::system_error{boost::system::errc::make_error_code(boost::system::errc::protocol_error),
                                    "invalid interface notification/snapshot"};
}

template <typename T>
T read(bytes data, std::size_t offset = 0) {
   if (offset > data.size() || sizeof(T) > data.size() - offset) {
      malformed();
   }
   T value{};
   std::memcpy(&value, data.data() + offset, sizeof(value));
   return value;
}

void adopt(asio::posix::stream_descriptor& owner, int fd) {
   if (fd < 0) {
      native_error("interface socket");
   }
   if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1 || ::fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
      const auto saved = errno;
      ::close(fd);
      errno = saved;
      native_error("interface socket flags");
   }
   auto error = boost::system::error_code{};
   owner.assign(fd, error);
   if (error) {
      ::close(fd);
      throw boost::system::system_error{error};
   }
}

void set_flags(record& item, unsigned flags) {
   item.up = (flags & IFF_UP) != 0;
   item.running = (flags & IFF_RUNNING) != 0;
   item.multicast = (flags & IFF_MULTICAST) != 0;
   item.loopback = (flags & IFF_LOOPBACK) != 0;
   item.point_to_point = (flags & IFF_POINTOPOINT) != 0;
}

record& find_record(std::vector<record>& result, std::uint32_t index) {
   const auto it = std::find_if(result.begin(), result.end(),
                              [index](const auto& value) { return value.index == index; });
   if (it == result.end()) {
      malformed();
   }
   return *it;
}

void add_address(record& item, interface_state::address value, const interface_state::limits& bounds) {
   if (item.addresses.size() >= bounds.addresses_per_interface) {
      throw std::length_error{"interface address limit exceeded"};
   }
   item.addresses.push_back(std::move(value));
}

void add_index(std::vector<std::uint32_t>& result, std::uint32_t index, std::size_t limit) {
   if (index == 0) {
      malformed();
   }
   if (std::find(result.begin(), result.end(), index) == result.end()) {
      if (result.size() >= limit) {
         throw std::length_error{"interface notification index limit exceeded"};
      }
      result.push_back(index);
   }
}

#if defined(__APPLE__)
std::array<bytes, RTAX_MAX> route_addresses(bytes data, std::size_t offset, unsigned mask) {
   auto result = std::array<bytes, RTAX_MAX>{};
   for (unsigned i = 0; i < RTAX_MAX; ++i) {
      if ((mask & (1U << i)) == 0) {
         continue;
      }
      if (offset >= data.size()) {
         malformed();
      }
      const auto length = data[offset];
      // Darwin route messages use 32-bit sockaddr alignment, not sizeof(long).
      const auto aligned = length == 0 ? 4U : (static_cast<unsigned>(length) + 3U) & ~3U;
      if (aligned > data.size() - offset) {
         malformed();
      }
      result[i] = data.subspan(offset, length);
      offset += aligned;
   }
   if ((mask >> RTAX_MAX) != 0 || offset != data.size()) {
      malformed();
   }
   return result;
}

template <typename T>
T padded_sockaddr(bytes data) {
   T value{};
   if (data.size() > sizeof(value)) {
      malformed();
   }
   if (!data.empty()) {
      std::memcpy(&value, data.data(), data.size());
   }
   return value;
}
#elif defined(__linux__)
template <typename F>
void attributes(bytes data, F consume) {
   while (!data.empty()) {
      const auto header = read<rtattr>(data);
      if (header.rta_len < sizeof(rtattr) || header.rta_len > data.size()) {
         malformed();
      }
      consume(header.rta_type, data.subspan(sizeof(rtattr), header.rta_len - sizeof(rtattr)));
      const auto aligned = static_cast<std::size_t>(RTA_ALIGN(header.rta_len));
      if (aligned > data.size()) {
         malformed();
      }
      data = data.subspan(aligned);
   }
}

boost::asio::ip::address netlink_address(bytes data, unsigned family) {
   if (family == AF_INET && data.size() == 4) {
      auto value = asio::ip::address_v4::bytes_type{};
      std::copy(data.begin(), data.end(), value.begin());
      return asio::ip::address_v4{value};
   }
   if (family == AF_INET6 && data.size() == 16) {
      auto value = asio::ip::address_v6::bytes_type{};
      std::copy(data.begin(), data.end(), value.begin());
      return asio::ip::address_v6{value};
   }
   malformed();
   return {};
}
#endif

} // namespace

interface_watcher::interface_watcher(asio::any_io_executor executor, options bounds)
    : _bounds(bounds), _state(bounds.state), _subscription(executor), _query(executor), _timer(executor) {
   if (bounds.snapshot_bytes < 4096 || bounds.snapshot_bytes > (16U << 20) ||
       bounds.messages_per_pass == 0 || bounds.messages_per_pass > 4096 ||
       bounds.resync_attempts == 0 || bounds.resync_attempts > 16 ||
       bounds.snapshot_timeout <= std::chrono::milliseconds::zero() ||
       bounds.snapshot_timeout > std::chrono::seconds{30} ||
       bounds.resync_interval <= std::chrono::milliseconds::zero() ||
       bounds.resync_interval > std::chrono::minutes{5}) {
      throw std::invalid_argument{"invalid interface watcher limits"};
   }
}

interface_watcher::~interface_watcher() = default;

void interface_watcher::check_stop() const {
   if (_stopped) {
      throw boost::system::system_error{asio::error::operation_aborted};
   }
}

void interface_watcher::subscribe() {
   try {
#if defined(__APPLE__)
      adopt(_subscription, ::socket(PF_ROUTE, SOCK_RAW, 0));
      adopt(_query, ::socket(AF_INET6, SOCK_DGRAM, 0));
#elif defined(__linux__)
      adopt(_subscription, ::socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE));
      auto address = sockaddr_nl{};
      address.nl_family = AF_NETLINK;
      address.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;
      if (::bind(_subscription.native_handle(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
         native_error("interface subscription bind");
      }
      adopt(_query, ::socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE));
      address.nl_groups = 0;
      if (::bind(_query.native_handle(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
         native_error("interface query bind");
      }
#else
      throw boost::system::system_error{asio::error::operation_not_supported};
#endif
      _resync_at = std::chrono::steady_clock::now() + _bounds.resync_interval;
   } catch (...) {
      auto ignored = boost::system::error_code{};
      _subscription.close(ignored);
      _query.close(ignored);
      throw;
   }
}

std::vector<std::uint32_t> interface_watcher::notification_indices(bytes data, std::size_t limit) {
   auto result = std::vector<std::uint32_t>{};
   while (!data.empty()) {
#if defined(__APPLE__)
      const auto length = read<std::uint16_t>(data);
      if (data.size() < 4 || length < 4 || length > data.size() || data[2] != RTM_VERSION) {
         malformed();
      }
      const auto message = data.first(length);
      switch (data[3]) {
      case RTM_IFINFO:
         add_index(result, read<if_msghdr>(message).ifm_index, limit);
         break;
      case RTM_IFINFO2:
         add_index(result, read<if_msghdr2>(message).ifm_index, limit);
         break;
      case RTM_NEWADDR:
      case RTM_DELADDR:
         add_index(result, read<ifa_msghdr>(message).ifam_index, limit);
         break;
      default:
         break;
      }
      data = data.subspan(length);
#elif defined(__linux__)
      const auto header = read<nlmsghdr>(data);
      if (header.nlmsg_len < sizeof(header) || header.nlmsg_len > data.size()) {
         malformed();
      }
      const auto message = data.subspan(sizeof(header), header.nlmsg_len - sizeof(header));
      switch (header.nlmsg_type) {
      case RTM_NEWLINK:
      case RTM_DELLINK: {
         const auto value = read<ifinfomsg>(message).ifi_index;
         if (value <= 0) {
            malformed();
         }
         add_index(result, static_cast<std::uint32_t>(value), limit);
         break;
      }
      case RTM_NEWADDR:
      case RTM_DELADDR:
         add_index(result, read<ifaddrmsg>(message).ifa_index, limit);
         break;
      case NLMSG_ERROR:
      case NLMSG_OVERRUN:
         malformed();
         break;
      default:
         break;
      }
      const auto length = static_cast<std::size_t>(NLMSG_ALIGN(header.nlmsg_len));
      if (length > data.size()) {
         malformed();
      }
      data = data.subspan(length);
#else
      throw boost::system::system_error{asio::error::operation_not_supported};
#endif
   }
   return result;
}

void interface_watcher::invalidate(std::uint32_t index) {
   if (_reset) {
      return;
   }
   try {
      add_index(_invalidated, index, _bounds.state.interfaces);
   } catch (const std::length_error&) {
      _invalidated.clear();
      _reset = true;
   }
}

bool interface_watcher::drain_notifications() {
   auto buffer = std::array<std::uint8_t, 65536>{};
   auto changed = false;
   for (std::size_t i = 0; i < _bounds.messages_per_pass; ++i) {
      check_stop();
      auto iov = iovec{buffer.data(), buffer.size()};
      auto message = msghdr{};
      message.msg_iov = &iov;
      message.msg_iovlen = 1;
#if defined(__linux__)
      auto sender = sockaddr_nl{};
      message.msg_name = &sender;
      message.msg_namelen = sizeof(sender);
#endif
      const auto size = ::recvmsg(_subscription.native_handle(), &message, MSG_DONTWAIT);
      if (size < 0) {
         if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return changed;
         }
         if (errno == EINTR) {
            continue;
         }
         if (errno == ENOBUFS) {
            _reset = changed = true;
            continue;
         }
         native_error("interface notification receive");
      }
#if defined(__linux__)
      if (message.msg_namelen != sizeof(sender) || sender.nl_family != AF_NETLINK || sender.nl_pid != 0) {
         _reset = changed = true;
         continue;
      }
#endif
      if (size == 0 || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
         _reset = changed = true;
         continue;
      }
      try {
         const auto indices = notification_indices(bytes{buffer.data(), static_cast<std::size_t>(size)},
                                                    _bounds.state.interfaces);
         changed = changed || !indices.empty();
         for (auto index : indices) {
            invalidate(index);
         }
      } catch (const boost::system::system_error&) {
         _reset = changed = true;
      } catch (const std::length_error&) {
         _reset = changed = true;
      }
   }
   // Work budget exhaustion cannot be mistaken for a drained subscription.
   _reset = true;
   return true;
}

asio::awaitable<void> interface_watcher::wait_for_change() {
   using namespace asio::experimental::awaitable_operators;
   while (true) {
      check_stop();
      _timer.expires_at(_resync_at);
      const auto result = co_await (
          _subscription.async_wait(asio::posix::stream_descriptor::wait_read, asio::as_tuple(asio::use_awaitable)) ||
          _timer.async_wait(asio::as_tuple(asio::use_awaitable)));
      check_stop();
      const auto cancellation = co_await asio::this_coro::cancellation_state;
      if (cancellation.cancelled() != asio::cancellation_type::none) {
         throw boost::system::system_error{asio::error::operation_aborted};
      }
      const auto error = result.index() == 0 ? std::get<0>(std::get<0>(result))
                                            : std::get<0>(std::get<1>(result));
      if (error) {
         throw boost::system::system_error{error};
      }
      if (std::chrono::steady_clock::now() >= _resync_at) {
         // PF_ROUTE can lose notifications silently. This is a bounded owner
         // timer reconciliation, not a promise of kernel incarnation identity.
         _reset = true;
         co_return;
      }
      if (drain_notifications()) {
         co_return;
      }
   }
}

asio::awaitable<interface_state::update> interface_watcher::next() {
   check_stop();
   if (!_subscription.is_open()) {
      subscribe();
   } else if (_state.current().revision != 0 && !_reset && _invalidated.empty()) {
      co_await wait_for_change();
   }
   auto last_error = std::exception_ptr{};
   for (unsigned attempt = 0; attempt < _bounds.resync_attempts; ++attempt) {
      check_stop();
      drain_notifications();
      auto observed = std::vector<record>{};
      try {
         observed = co_await snapshot();
      } catch (const boost::system::system_error& error) {
         if (error.code() == asio::error::operation_aborted || _stopped) {
            throw;
         }
         last_error = std::current_exception();
         _reset = true;
      }
      if (last_error) {
         if (attempt + 1 == _bounds.resync_attempts) {
            std::rethrow_exception(last_error);
         }
         last_error = {};
         co_await asio::post(asio::use_awaitable);
         continue;
      }
      if (drain_notifications()) {
         co_await asio::post(asio::use_awaitable);
         continue;
      }
      check_stop();
      if (std::chrono::steady_clock::now() >= _resync_at) {
         _reset = true;
      }
      auto result = _state.reconcile(std::move(observed), _invalidated, _reset);
      _invalidated.clear();
      _reset = false;
      // Ordinary event traffic must not postpone the lossy-channel safety scan.
      if (std::chrono::steady_clock::now() >= _resync_at) {
         _resync_at = std::chrono::steady_clock::now() + _bounds.resync_interval;
      }
      co_return result;
   }
   _reset = true;
   throw boost::system::system_error{
       boost::system::errc::make_error_code(boost::system::errc::resource_unavailable_try_again),
       "interface snapshot did not settle within resync budget"};
}

asio::awaitable<interface_state::update> interface_watcher::async_next() {
   const auto cancellation = co_await asio::this_coro::cancellation_state;
   if (cancellation.cancelled() != asio::cancellation_type::none) {
      throw boost::system::system_error{asio::error::operation_aborted};
   }
   check_stop();
   if (_active) {
      throw std::logic_error{"only one interface async_next may be active"};
   }
   _active = true;
   try {
      auto result = co_await next();
      _active = false;
      _finished.notify();
      co_return result;
   } catch (...) {
      _reset = true;
      _active = false;
      _finished.notify();
      throw;
   }
}

void interface_watcher::request_stop() noexcept {
   _stopped = true;
   auto ignored = boost::system::error_code{};
   _subscription.close(ignored);
   _query.close(ignored);
   try {
      _timer.cancel();
   } catch (...) {
      // Descriptor closure still wakes the joined readiness operation.
   }
}

asio::awaitable<void> interface_watcher::async_stop() {
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
   request_stop();
   while (_active) {
      const auto epoch = _finished.epoch();
      if (_active) {
         co_await _finished.async_wait(epoch);
      }
   }
}

#if defined(__APPLE__)
asio::awaitable<std::vector<record>> interface_watcher::snapshot() {
   const auto deadline = std::chrono::steady_clock::now() + _bounds.snapshot_timeout;
   auto mib = std::array<int, 6>{CTL_NET, PF_ROUTE, 0, AF_UNSPEC, NET_RT_IFLIST, 0};
   auto size = std::size_t{0};
   if (::sysctl(mib.data(), mib.size(), nullptr, &size, nullptr, 0) < 0) {
      native_error("interface snapshot size");
   }
   if (size > _bounds.snapshot_bytes) {
      throw std::length_error{"interface snapshot byte limit exceeded"};
   }
   auto storage = std::vector<std::uint8_t>(size);
   if (::sysctl(mib.data(), mib.size(), storage.data(), &size, nullptr, 0) < 0) {
      native_error("interface snapshot");
   }
   if (size > storage.size()) {
      malformed();
   }
   auto data = bytes{storage.data(), size};
   auto result = std::vector<record>{};
   while (!data.empty()) {
      check_stop();
      if (std::chrono::steady_clock::now() >= deadline) {
         throw boost::system::system_error{asio::error::timed_out};
      }
      const auto length = read<std::uint16_t>(data);
      if (data.size() < 4 || length < 4 || length > data.size() || data[2] != RTM_VERSION) {
         malformed();
      }
      const auto message = data.first(length);
      if (data[3] == RTM_IFINFO) {
         const auto header = read<if_msghdr>(message);
         const auto addresses = route_addresses(message, sizeof(header), header.ifm_addrs);
         const auto link = addresses[RTAX_IFP];
         constexpr auto base = offsetof(sockaddr_dl, sdl_data);
         if (link.size() < base || link[1] != AF_LINK) {
            malformed();
         }
         const auto name_size = link[offsetof(sockaddr_dl, sdl_nlen)];
         if (name_size == 0 || name_size >= IFNAMSIZ || name_size > link.size() - base) {
            malformed();
         }
         if (result.size() >= _bounds.state.interfaces) {
            throw std::length_error{"interface snapshot count exceeded"};
         }
         auto item = record{};
         item.index = header.ifm_index;
         item.name.assign(reinterpret_cast<const char*>(link.data() + base), name_size);
         set_flags(item, header.ifm_flags);
         result.push_back(std::move(item));
      } else if (data[3] == RTM_NEWADDR) {
         const auto header = read<ifa_msghdr>(message);
         const auto addresses = route_addresses(message, sizeof(header), header.ifam_addrs);
         const auto local = addresses[RTAX_IFA];
         if (local.size() < 2) {
            malformed();
         }
         auto& item = find_record(result, header.ifam_index);
         auto address = interface_state::address{};
         const auto mask = addresses[RTAX_NETMASK];
         if (local[1] == AF_INET) {
            const auto native = read<sockaddr_in>(local);
            auto octets = asio::ip::address_v4::bytes_type{};
            std::memcpy(octets.data(), &native.sin_addr, octets.size());
            address.value = asio::ip::address_v4{octets};
            address.flags_known = true;
            if (!mask.empty()) {
               const auto native_mask = padded_sockaddr<sockaddr_in>(mask);
               std::memcpy(octets.data(), &native_mask.sin_addr, octets.size());
               address.prefix_length = interface_state::prefix(octets);
            }
         } else if (local[1] == AF_INET6) {
            auto native = read<sockaddr_in6>(local);
            auto octets = asio::ip::address_v6::bytes_type{};
            std::memcpy(octets.data(), &native.sin6_addr, octets.size());
            if (asio::ip::address_v6{octets}.is_link_local()) {
               // BSD can embed the link scope in bytes 2..3 of sockaddr_in6.
               const auto embedded = (static_cast<unsigned>(octets[2]) << 8U) | octets[3];
               if ((embedded != 0 && embedded != item.index) ||
                   (native.sin6_scope_id != 0 && native.sin6_scope_id != item.index)) {
                  malformed();
               }
               octets[2] = octets[3] = 0;
               address.scope_id = item.index;
            } else {
               address.scope_id = native.sin6_scope_id;
            }
            address.value = asio::ip::address_v6{octets};
            if (!mask.empty()) {
               const auto native_mask = padded_sockaddr<sockaddr_in6>(mask);
               auto mask_bytes = asio::ip::address_v6::bytes_type{};
               std::memcpy(mask_bytes.data(), &native_mask.sin6_addr, mask_bytes.size());
               address.prefix_length = interface_state::prefix(mask_bytes);
            }
            auto request = in6_ifreq{};
            std::memcpy(request.ifr_name, item.name.c_str(), item.name.size() + 1);
            request.ifr_ifru.ifru_addr = native;
            if (::ioctl(_query.native_handle(), SIOCGIFAFLAG_IN6, &request) < 0) {
               native_error("interface IPv6 address flags");
            }
            const auto flags = request.ifr_ifru.ifru_flags6;
            address.flags_known = true;
            address.tentative = (flags & IN6_IFF_DADPROGRESS) != 0;
            address.duplicate = (flags & IN6_IFF_DUPLICATED) != 0;
            address.deprecated = (flags & IN6_IFF_DEPRECATED) != 0;
            address.detached = (flags & IN6_IFF_DETACHED) != 0;
         } else {
            data = data.subspan(length);
            continue;
         }
         add_address(item, std::move(address), _bounds.state);
      }
      data = data.subspan(length);
   }
   co_return result;
}
#elif defined(__linux__)
asio::awaitable<void> interface_watcher::dump(std::uint16_t type, std::vector<record>& result,
                                             std::chrono::steady_clock::time_point deadline) {
   using namespace asio::experimental::awaitable_operators;
   if (++_sequence == 0) {
      ++_sequence;
   }
   auto request = std::array<std::uint8_t, NLMSG_SPACE(sizeof(rtgenmsg))>{};
   auto header = nlmsghdr{};
   header.nlmsg_len = NLMSG_LENGTH(sizeof(rtgenmsg));
   header.nlmsg_type = type;
   header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
   header.nlmsg_seq = _sequence;
   std::memcpy(request.data(), &header, sizeof(header));
   request[sizeof(header)] = AF_UNSPEC;
   auto kernel = sockaddr_nl{};
   kernel.nl_family = AF_NETLINK;
   const auto sent = ::sendto(_query.native_handle(), request.data(), header.nlmsg_len, MSG_DONTWAIT,
                              reinterpret_cast<const sockaddr*>(&kernel), sizeof(kernel));
   if (sent < 0) {
      native_error("interface dump request");
   }
   if (static_cast<std::size_t>(sent) != header.nlmsg_len) {
      malformed();
   }
   auto storage = std::array<std::uint8_t, 65536>{};
   auto total = std::size_t{0};
   auto packets = std::size_t{0};
   while (true) {
      check_stop();
      if (std::chrono::steady_clock::now() >= deadline) {
         throw boost::system::system_error{asio::error::timed_out};
      }
      auto sender = sockaddr_nl{};
      auto iov = iovec{storage.data(), storage.size()};
      auto message = msghdr{};
      message.msg_name = &sender;
      message.msg_namelen = sizeof(sender);
      message.msg_iov = &iov;
      message.msg_iovlen = 1;
      const auto size = ::recvmsg(_query.native_handle(), &message, MSG_DONTWAIT);
      if (size < 0) {
         if (errno == EINTR) {
            continue;
         }
         if (errno != EAGAIN && errno != EWOULDBLOCK) {
            native_error("interface dump receive");
         }
         _timer.expires_at(deadline);
         const auto ready = co_await (
             _query.async_wait(asio::posix::stream_descriptor::wait_read, asio::as_tuple(asio::use_awaitable)) ||
             _timer.async_wait(asio::as_tuple(asio::use_awaitable)));
         check_stop();
         const auto cancellation = co_await asio::this_coro::cancellation_state;
         if (cancellation.cancelled() != asio::cancellation_type::none) {
            throw boost::system::system_error{asio::error::operation_aborted};
         }
         const auto error = ready.index() == 0 ? std::get<0>(std::get<0>(ready))
                                              : std::get<0>(std::get<1>(ready));
         if (error) {
            throw boost::system::system_error{error};
         }
         if (ready.index() == 1) {
            throw boost::system::system_error{asio::error::timed_out};
         }
         continue;
      }
      if (size == 0 || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
          message.msg_namelen != sizeof(sender) || sender.nl_pid != 0 || sender.nl_family != AF_NETLINK) {
         malformed();
      }
      if (static_cast<std::size_t>(size) > _bounds.snapshot_bytes - total ||
          ++packets > _bounds.messages_per_pass) {
         throw std::length_error{"interface dump budget exceeded"};
      }
      total += size;
      auto data = bytes{storage.data(), static_cast<std::size_t>(size)};
      while (!data.empty()) {
         const auto h = read<nlmsghdr>(data);
         if (h.nlmsg_len < sizeof(h) || h.nlmsg_len > data.size() ||
             h.nlmsg_seq != _sequence || (h.nlmsg_flags & NLM_F_DUMP_INTR) != 0) {
            malformed();
         }
         const auto payload = data.subspan(sizeof(h), h.nlmsg_len - sizeof(h));
         if (h.nlmsg_type == NLMSG_DONE) {
            if (!payload.empty() && read<int>(payload) != 0) {
               malformed();
            }
            co_return;
         }
         if (h.nlmsg_type == NLMSG_ERROR || h.nlmsg_type == NLMSG_OVERRUN) {
            malformed();
         }
         if (type == RTM_GETLINK && h.nlmsg_type == RTM_NEWLINK) {
            const auto native = read<ifinfomsg>(payload);
            if (native.ifi_index <= 0) {
               malformed();
            }
            if (result.size() >= _bounds.state.interfaces) {
               throw std::length_error{"interface snapshot count exceeded"};
            }
            auto item = record{};
            item.index = native.ifi_index;
            set_flags(item, native.ifi_flags);
            attributes(payload.subspan(NLMSG_ALIGN(sizeof(native))), [&](unsigned kind, bytes value) {
               if (kind == IFLA_IFNAME) {
                  if (value.empty() || value.size() > IFNAMSIZ || value.back() != 0) {
                     malformed();
                  }
                  item.name.assign(reinterpret_cast<const char*>(value.data()), value.size() - 1);
               }
            });
            result.push_back(std::move(item));
         } else if (type == RTM_GETADDR && h.nlmsg_type == RTM_NEWADDR) {
            const auto native = read<ifaddrmsg>(payload);
            if (native.ifa_family == AF_INET || native.ifa_family == AF_INET6) {
               auto flags = static_cast<std::uint32_t>(native.ifa_flags);
               auto local = bytes{};
               auto peer = bytes{};
               attributes(payload.subspan(NLMSG_ALIGN(sizeof(native))), [&](unsigned kind, bytes value) {
                  if (kind == IFA_LOCAL) {
                     local = value;
                  } else if (kind == IFA_ADDRESS) {
                     peer = value;
                  } else if (kind == IFA_FLAGS) {
                     if (value.size() != sizeof(std::uint32_t)) {
                        malformed();
                     }
                     flags = read<std::uint32_t>(value);
                  }
               });
               auto& item = find_record(result, native.ifa_index);
               auto address = interface_state::address{};
               address.value = netlink_address(local.empty() ? peer : local, native.ifa_family);
               address.prefix_length = native.ifa_prefixlen;
               address.flags_known = true;
               address.tentative = (flags & (IFA_F_TENTATIVE | IFA_F_OPTIMISTIC)) != 0;
               address.duplicate = (flags & IFA_F_DADFAILED) != 0;
               address.deprecated = (flags & IFA_F_DEPRECATED) != 0;
               if (address.value.is_v6() && address.value.to_v6().is_link_local()) {
                  address.scope_id = item.index;
               }
               add_address(item, std::move(address), _bounds.state);
            }
         }
         const auto aligned = static_cast<std::size_t>(NLMSG_ALIGN(h.nlmsg_len));
         if (aligned > data.size()) {
            malformed();
         }
         data = data.subspan(aligned);
      }
   }
}

asio::awaitable<std::vector<record>> interface_watcher::snapshot() {
   // Fresh query socket after an interrupted dump; never consume a previous
   // multipart response as the next snapshot. The subscription stays open.
   auto ignored = boost::system::error_code{};
   _query.close(ignored);
   adopt(_query, ::socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE));
   auto address = sockaddr_nl{};
   address.nl_family = AF_NETLINK;
   if (::bind(_query.native_handle(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
      native_error("interface query bind");
   }
   auto result = std::vector<record>{};
   const auto deadline = std::chrono::steady_clock::now() + _bounds.snapshot_timeout;
   co_await dump(RTM_GETLINK, result, deadline);
   co_await dump(RTM_GETADDR, result, deadline);
   co_return result;
}
#else
asio::awaitable<std::vector<record>> interface_watcher::snapshot() {
   throw boost::system::system_error{asio::error::operation_not_supported};
   co_return std::vector<record>{};
}
#endif

} // namespace forge::net::p2p::detail
