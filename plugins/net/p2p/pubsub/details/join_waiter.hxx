#pragma once

namespace forge::plugins::net::p2p::pubsub {

struct join_waiter : std::enable_shared_from_this<join_waiter> {
   explicit join_waiter(boost::asio::any_io_executor executor);
   void complete(std::exception_ptr failure = {});

   boost::asio::steady_timer timer;
   std::exception_ptr error;
   bool ready = false;
};

} // namespace forge::plugins::net::p2p::pubsub
