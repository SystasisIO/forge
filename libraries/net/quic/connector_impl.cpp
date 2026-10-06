module;

#include <forge/exceptions/macros.hpp>

#include "details/wrapper_handles.hxx"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/system/system_error.hpp>

#include "details/client_token_cache.hxx"

module forge.net.quic.connector;

import forge.crypto.core.secret_string;
import forge.net.quic.exceptions;
import forge.net.quic.runtime;
import forge.net.quic.security;

#include "details/engine_client_options.hxx"

#include "details/connector_impl.hxx"

namespace forge::net::quic {
connector::impl::impl(forge::asio::runtime& runtime_value)
    : runtime(runtime_value), engine(runtime_value.context()),
      client_tokens(std::make_shared<detail::client_token_cache>()) {}

connector::impl::impl(forge::asio::runtime& runtime_value, detail::engine_listener& source, endpoint local)
    : runtime(runtime_value),
      engine(runtime_value.context(), source,
             {.host = std::move(local.host), .port = local.port, .zone = std::move(local.zone)}),
      client_tokens(std::make_shared<detail::client_token_cache>()) {}
} // namespace forge::net::quic
