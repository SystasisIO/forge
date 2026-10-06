#include "details/listener_callback.hxx"
#include "details/engine_listener_impl.hxx"

namespace forge::net::quic::detail {

listener_callback::listener_callback(std::shared_ptr<engine_listener::impl> listener_value,
                                     std::shared_ptr<engine_connection::impl> connection_value)
    : listener(std::move(listener_value)), connection(std::move(connection_value)) {
   listener->begin_callback();
}

listener_callback::~listener_callback() {
   connection.reset();
   listener->finish_callback();
}

} // namespace forge::net::quic::detail
