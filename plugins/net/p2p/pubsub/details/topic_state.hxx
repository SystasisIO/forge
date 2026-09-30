#pragma once

#include "handler_record.hxx"
#include "join_waiter.hxx"

namespace forge::plugins::net::p2p::pubsub {

struct topic_state {
   std::map<std::uint64_t, handler_record> handlers;
   std::vector<std::shared_ptr<join_waiter>> waiters;
   bool joining = false;
   bool joined = false;
};

} // namespace forge::plugins::net::p2p::pubsub
