#pragma once

#include "interface_state.hxx"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

import forge.asio.notification;

namespace forge::net::p2p::detail {

// Private, caller-owned and executor-affine. All calls, including destruction,
// must be serialized on the supplied executor. At most one async_next is active.
// The owner must await async_stop before destruction; no detached work is started.
class interface_watcher {
 public:
   struct options {
      interface_state::limits state;
      std::size_t snapshot_bytes = 1U << 20;
      std::size_t messages_per_pass = 256;
      unsigned resync_attempts = 3;
      std::chrono::milliseconds snapshot_timeout{2000};
      std::chrono::milliseconds resync_interval{30'000};
   };

   explicit interface_watcher(boost::asio::any_io_executor executor, options bounds);
   ~interface_watcher();
   interface_watcher(const interface_watcher&) = delete;
   interface_watcher& operator=(const interface_watcher&) = delete;

   // Subscribes before the first snapshot. Each update is a full replacement.
   // A failed resync throws, never returns a partial or stale successful update.
   boost::asio::awaitable<interface_state::update> async_next();
   void request_stop() noexcept;
   boost::asio::awaitable<void> async_stop();

   // Native framing seam for deterministic tests, not a public library API.
   // Unknown well-framed messages are skipped; malformed messages throw.
   static std::vector<std::uint32_t> notification_indices(std::span<const std::uint8_t> bytes,
                                                         std::size_t limit);

 private:
   void subscribe();
   void check_stop() const;
   bool drain_notifications();
   void invalidate(std::uint32_t index);
   boost::asio::awaitable<void> wait_for_change();
   boost::asio::awaitable<std::vector<interface_state::interface>> snapshot();
   boost::asio::awaitable<interface_state::update> next();
#if defined(__linux__)
   boost::asio::awaitable<void> dump(std::uint16_t type, std::vector<interface_state::interface>& result,
                                    std::chrono::steady_clock::time_point deadline);
#endif

   options _bounds;
   interface_state _state;
   boost::asio::posix::stream_descriptor _subscription;
   boost::asio::posix::stream_descriptor _query;
   boost::asio::steady_timer _timer;
   forge::asio::notification _finished;
   std::vector<std::uint32_t> _invalidated;
   std::chrono::steady_clock::time_point _resync_at;
   std::uint32_t _sequence = 0;
   bool _reset = false;
   bool _active = false;
   bool _stopped = false;
};

} // namespace forge::net::p2p::detail
