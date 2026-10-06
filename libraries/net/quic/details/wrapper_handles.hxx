#pragma once

#include "quic_engine.hxx"

#include <memory>

namespace forge::net::quic::detail {

struct connection_handle {
   std::shared_ptr<engine_connection> engine;
};

struct listener_handle {
   std::shared_ptr<engine_listener> engine;
};

struct stream_handle {
   std::shared_ptr<engine_stream> engine;
   std::shared_ptr<engine_connection> connection;
};

} // namespace forge::net::quic::detail
