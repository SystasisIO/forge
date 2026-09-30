#pragma once

namespace forge::plugins::net::p2p::pubsub {

struct handler_record {
   std::uint64_t id = 0;
   forge::net::p2p::pubsub::topic subject;
   handler callback;
   std::chrono::milliseconds deadline{0};
};

} // namespace forge::plugins::net::p2p::pubsub
