#pragma once

#include "handler_record.hxx"

namespace forge::plugins::net::p2p::pubsub {

struct partial_record {
   forge::net::p2p::pubsub::partial_topic token;
   std::shared_ptr<handler_record> fallback;
   forge::net::p2p::pubsub::partial_options callbacks;
   std::stop_source stop;
   bool committed = false;
   bool admission = false;
   bool removing = false;
   bool attempted = false;
   bool cleanup_pending = false;
};

} // namespace forge::plugins::net::p2p::pubsub
