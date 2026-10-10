#pragma once

#include "handler_record.hxx"

namespace forge::plugins::net::p2p::pubsub {

struct topic_state {
   forge::net::p2p::pubsub::topic subject;
   forge::asio::gate transition;
   std::map<std::uint64_t, std::shared_ptr<handler_record>> handlers;
   std::size_t participants = 0;
   bool joined = false;
   bool native_dirty = false;
   bool gate_closed = false;
   bool shutdown_attempted = false;
   std::exception_ptr join_error;
   std::exception_ptr leave_error;
};

} // namespace forge::plugins::net::p2p::pubsub
