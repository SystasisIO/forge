#pragma once

namespace forge::net::p2p {

struct host_event_subscription::impl {
   read_callback read;
   close_callback close;
   active_callback active;
};

} // namespace forge::net::p2p
