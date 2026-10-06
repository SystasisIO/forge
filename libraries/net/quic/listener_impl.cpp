module;

#include <forge/exceptions/macros.hpp>

#include "details/wrapper_handles.hxx"

#include <memory>
#include <utility>

#include <boost/asio/awaitable.hpp>

module forge.net.quic.listener;

import forge.crypto.core.secret_string;
import forge.net.quic.exceptions;
import forge.net.quic.runtime;
import forge.net.quic.security;

#include "details/engine_server_options.hxx"

#include "details/listener_impl.hxx"

namespace forge::net::quic {
listener::impl::impl(forge::asio::runtime& runtime_value, endpoint bind_endpoint_value, server_options options_value)
    : runtime(runtime_value), engine(std::make_shared<detail::engine_listener>(
                                  runtime_value.context(),
                                  detail::engine_endpoint{.host = std::move(bind_endpoint_value.host),
                                                          .port = bind_endpoint_value.port,
                                                          .zone = std::move(bind_endpoint_value.zone)},
                                  map_options(options_value))) {}
} // namespace forge::net::quic
