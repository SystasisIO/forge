#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/steady_timer.hpp>

#include "direct_transport.hxx"
#include "path_manager.hxx"

namespace forge::net::p2p::direct::detail {
struct pending_quic_connection;
}

namespace forge::net::p2p::detail {

// One explicit tuple/generation owner, separate from relay/DCUtR coalescing.
class coordinated_dial final : public std::enable_shared_from_this<coordinated_dial> {
 public:
   using time_point = std::chrono::steady_clock::time_point;
   static constexpr std::size_t max_active = 8;
   static constexpr std::size_t max_joiners = 8;
   static constexpr std::size_t max_native_workers = 4;

   coordinated_dial(boost::asio::any_io_executor executor, endpoint remote,
                    node::coordinated_connect_options options, std::uint64_t generation, upgrade_role role);

   const endpoint remote;
   const node::coordinated_connect_options options;
   const std::uint64_t generation;
   const upgrade_role role;
   const time_point deadline;
   const std::shared_ptr<cancellation_latch> cancellation;
   const std::shared_ptr<cancellation_latch> transports;

   // Written once by synchronous profile preparation, before native work.
   std::shared_ptr<void> source;
   std::shared_ptr<void> source_admission;
   std::string source_key;
   resource_manager::dial_reservation permit;
   std::shared_ptr<direct::detail::pending_quic_connection> pending_quic;

   [[nodiscard]] bool begin_inbound() noexcept;
   void end_inbound() noexcept;
   void end_outbound(std::exception_ptr failure) noexcept;
   [[nodiscard]] bool install(direct::connection& connection, bool inbound);
   [[nodiscard]] direct::connection take();
   [[nodiscard]] bool inbound_winner() const;
   [[nodiscard]] bool stopped() const;
   [[nodiscard]] bool timed_out() const;
   [[nodiscard]] std::exception_ptr failure() const;
   void request_cancel(bool timed_out = false) noexcept;
   void finish() noexcept;
   boost::asio::awaitable<void> async_join();

   template <typename Launch, typename CompletionToken>
   auto async_run(Launch launch, CompletionToken&& token) {
      return boost::asio::async_initiate<CompletionToken, void(boost::system::error_code)>(
          [self = shared_from_this(), launch = std::move(launch)](auto handler) mutable {
             const auto owner = std::move(self);
             {
                const auto lock = std::scoped_lock{owner->_mutex};
                owner->_ready.async_wait(boost::asio::bind_cancellation_slot(
                    boost::asio::cancellation_slot{}, std::move(handler)));
                owner->_armed = true;
             }
             try { launch(); }
             catch (...) { owner->request_cancel(); owner->end_outbound(std::current_exception()); }
          }, token);
   }

 private:
   void signal_locked() noexcept;
   mutable std::mutex _mutex;
   boost::asio::steady_timer _ready;
   direct::connection _winner;
   std::size_t _workers = 1;
   bool _armed = false;
   bool _closed = false;
   bool _signaled = false;
   bool _inbound_winner = false;
   bool _canceled = false;
   bool _timed_out = false;
   bool _finishing = false;
   bool _done = false;
   std::exception_ptr _failure;
   std::vector<std::weak_ptr<path_manager::dial_batch>> _joiners;
};

} // namespace forge::net::p2p::detail
