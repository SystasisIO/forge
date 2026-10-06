#pragma once

#include "quic_engine.hxx"

namespace forge::net::quic::detail {

struct listener_callback {
   listener_callback(std::shared_ptr<engine_listener::impl> listener,
                     std::shared_ptr<engine_connection::impl> connection);
   ~listener_callback();
   listener_callback(const listener_callback&) = delete;
   listener_callback& operator=(const listener_callback&) = delete;
   std::shared_ptr<engine_listener::impl> listener;
   std::shared_ptr<engine_connection::impl> connection;
};

} // namespace forge::net::quic::detail
