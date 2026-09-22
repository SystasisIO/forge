#include <boost/test/unit_test.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/system_error.hpp>
#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <stdexcept>

#if defined(__APPLE__)
#include <net/if.h>
#include <net/route.h>
#elif defined(__linux__)
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#endif

#include "../../libraries/net/p2p/details/interface_watcher.hxx"

namespace {
namespace asio = boost::asio;
using watcher = forge::net::p2p::detail::interface_watcher;
using update = forge::net::p2p::detail::interface_state::update;
using namespace std::chrono_literals;

std::vector<std::uint8_t> notification(std::uint32_t index) {
#if defined(__APPLE__)
   auto header = if_msghdr{};
   header.ifm_msglen = sizeof(header);
   header.ifm_version = RTM_VERSION;
   header.ifm_type = RTM_IFINFO;
   header.ifm_index = index;
   auto data = std::vector<std::uint8_t>(sizeof(header));
   std::memcpy(data.data(), &header, sizeof(header));
   return data;
#elif defined(__linux__)
   auto header = nlmsghdr{};
   header.nlmsg_len = NLMSG_LENGTH(sizeof(ifinfomsg));
   header.nlmsg_type = RTM_NEWLINK;
   auto body = ifinfomsg{};
   body.ifi_index = index;
   auto data = std::vector<std::uint8_t>(NLMSG_SPACE(sizeof(body)));
   std::memcpy(data.data(), &header, sizeof(header));
   std::memcpy(data.data() + sizeof(header), &body, sizeof(body));
   return data;
#else
   return {};
#endif
}

template <typename T>
void finish(asio::io_context& io, watcher& value, std::future<T>& future) {
   io.restart();
   io.run_for(5s);
   const auto ready = future.wait_for(0ms) == std::future_status::ready;
   if (!ready) {
      value.request_stop();
      io.restart();
      io.run_for(1s);
   }
   BOOST_REQUIRE_MESSAGE(ready, "interface watcher exceeded independent outer test deadline");
}
} // namespace

BOOST_AUTO_TEST_CASE(p2p_interface_watcher_native_notification_framing) {
   auto packet = notification(7);
   const auto second = notification(9);
   packet.insert(packet.end(), second.begin(), second.end());
   const auto duplicate = notification(7);
   packet.insert(packet.end(), duplicate.begin(), duplicate.end());
   const auto result = watcher::notification_indices(packet, 2);
   BOOST_REQUIRE_EQUAL(result.size(), 2U);
   BOOST_TEST(result[0] == 7U);
   BOOST_TEST(result[1] == 9U);
   BOOST_CHECK_THROW(watcher::notification_indices(packet, 1), std::length_error);
   packet.pop_back();
   BOOST_CHECK_THROW(watcher::notification_indices(packet, 2), boost::system::system_error);
   const auto zero = notification(0);
   BOOST_CHECK_THROW(watcher::notification_indices(zero, 2), boost::system::system_error);
   auto short_header = std::array<std::uint8_t, 1>{0};
   BOOST_CHECK_THROW(watcher::notification_indices(short_header, 2), boost::system::system_error);
}

BOOST_AUTO_TEST_CASE(p2p_interface_watcher_native_snapshot_and_timer_resync) {
   auto io = asio::io_context{};
   auto options = watcher::options{};
   options.resync_interval = 20ms;
   auto value = watcher{io.get_executor(), options};
   auto operation = [&]() -> asio::awaitable<void> {
      const auto first = co_await value.async_next();
      BOOST_TEST(first.revision == 1U);
      BOOST_REQUIRE(!first.interfaces.empty());
      auto loopback = false;
      for (const auto& item : first.interfaces) {
         BOOST_TEST(item.index != 0U);
         BOOST_TEST(item.generation != 0U);
         BOOST_TEST(!item.name.empty());
         for (const auto& address : item.addresses) {
            loopback = loopback || address.value.is_loopback();
            BOOST_TEST(address.flags_known);
            if (address.value.is_v6() && address.value.to_v6().is_link_local()) {
               BOOST_TEST(address.scope_id == item.index);
            }
         }
      }
      BOOST_TEST(loopback);
      auto reset = update{};
      for (unsigned i = 0; i < 16; ++i) {
         reset = co_await value.async_next();
         if (reset.continuity_lost) {
            break;
         }
      }
      BOOST_TEST(reset.continuity_lost);
      for (const auto& item : reset.interfaces) {
         for (const auto& old : first.interfaces) {
            if (old.index == item.index) {
               BOOST_TEST(item.generation > old.generation);
            }
         }
      }
      co_await value.async_stop();
      co_await value.async_stop();
   };
   auto future = asio::co_spawn(io, operation(), asio::use_future);
   finish(io, value, future);
   BOOST_CHECK_NO_THROW(future.get());
}

BOOST_AUTO_TEST_CASE(p2p_interface_watcher_stop_joins_pending_receive) {
   auto io = asio::io_context{};
   auto value = watcher{io.get_executor(), watcher::options{}};
   auto initial = asio::co_spawn(io, value.async_next(), asio::use_future);
   finish(io, value, initial);
   BOOST_REQUIRE_NO_THROW(static_cast<void>(initial.get()));
   auto pending = asio::co_spawn(io, value.async_next(), asio::use_future);
   auto stop = asio::co_spawn(io, value.async_stop(), asio::use_future);
   finish(io, value, stop);
   BOOST_CHECK_NO_THROW(stop.get());
   BOOST_REQUIRE(pending.wait_for(0ms) == std::future_status::ready);
   BOOST_CHECK_EXCEPTION(static_cast<void>(pending.get()), boost::system::system_error,
                         [](const auto& error) { return error.code() == asio::error::operation_aborted; });
   auto after = asio::co_spawn(io, value.async_next(), asio::use_future);
   finish(io, value, after);
   BOOST_CHECK_EXCEPTION(static_cast<void>(after.get()), boost::system::system_error,
                         [](const auto& error) { return error.code() == asio::error::operation_aborted; });
}

BOOST_AUTO_TEST_CASE(p2p_interface_watcher_precancel_and_operation_cancel) {
   auto io = asio::io_context{};
   auto value = watcher{io.get_executor(), watcher::options{}};
   auto signal = asio::cancellation_signal{};
   auto pre = [&]() -> asio::awaitable<update> {
      signal.emit(asio::cancellation_type::terminal);
      co_return co_await value.async_next();
   };
   auto canceled = asio::co_spawn(io, pre(), asio::bind_cancellation_slot(signal.slot(), asio::use_future));
   finish(io, value, canceled);
   BOOST_CHECK_EXCEPTION(static_cast<void>(canceled.get()), boost::system::system_error,
                         [](const auto& error) { return error.code() == asio::error::operation_aborted; });
   auto initial = asio::co_spawn(io, value.async_next(), asio::use_future);
   finish(io, value, initial);
   BOOST_REQUIRE_NO_THROW(static_cast<void>(initial.get()));
   auto pending = asio::co_spawn(io, value.async_next(),
                                asio::bind_cancellation_slot(signal.slot(), asio::use_future));
   asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); });
   finish(io, value, pending);
   BOOST_CHECK_EXCEPTION(static_cast<void>(pending.get()), boost::system::system_error,
                         [](const auto& error) { return error.code() == asio::error::operation_aborted; });
   auto stop = asio::co_spawn(io, value.async_stop(), asio::use_future);
   finish(io, value, stop);
   BOOST_CHECK_NO_THROW(stop.get());
}

BOOST_AUTO_TEST_CASE(p2p_interface_watcher_rejects_invalid_bounds) {
   auto io = asio::io_context{};
   auto options = watcher::options{};
   options.snapshot_bytes = 1;
   BOOST_CHECK_THROW((watcher{io.get_executor(), options}), std::invalid_argument);
   options = watcher::options{};
   options.resync_interval = 0ms;
   BOOST_CHECK_THROW((watcher{io.get_executor(), options}), std::invalid_argument);
}
