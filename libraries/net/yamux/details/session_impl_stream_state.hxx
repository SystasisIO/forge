#pragma once

#include "session_impl.hxx"

namespace forge::net::yamux {

struct session::impl::stream_state {
   explicit stream_state(std::uint32_t stream_id);

   std::uint32_t id = 0;
   std::uint32_t send_window = 0;
   std::uint32_t receive_window = 0;
   std::uint32_t pending_receive_credit = 0;
   std::deque<detail::bytes> inbound;
   std::size_t buffered = 0;
   bool local_fin = false;
   bool local_fin_sent = false;
   bool remote_fin = false;
   bool reset = false;
   bool accepted = false;
   bool cancel_in_progress = false;
   bool local_reset_sent = false;
   std::atomic_bool cancel_requested = false;
   forge::asio::notification read_notification;
   forge::asio::notification window_notification;
   forge::asio::notification receive_credit_notification;
   // Completes a local/remote reset without coupling a stream close to the
   // session-wide reset writer's lifetime.
   forge::asio::notification terminal_notification;
};

} // namespace forge::net::yamux
