module;

#include <coroutine>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "details/router_server_access.hxx"
#include "details/stream_server_access.hxx"
#include "details/server_log.hxx"
#include "details/request_read_ownership.hxx"

#include <forge/exceptions/macros.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/scope/scope_exit.hpp>
#include <openssl/ssl.h>

module forge.net.http.server;

import forge.asio.exceptions;
import forge.asio.runtime;
import forge.crypto.digest.sha256;
import forge.net.http.body;
import forge.net.http.exceptions;
import forge.net.http.negotiation;
import forge.net.http.route_context;
import forge.net.http.stream;
import forge.net.tls.context;
import forge.net.websocket.connection;

#include "details/server_session.hxx"

#include "details/server_session_model.hxx"

extern "C++" {
namespace forge::net::http::detail {
namespace beast = boost::beast;



server_session::~server_session() = default;

std::shared_ptr<server_session> server_session::create(
    forge::asio::runtime& runtime, beast::tcp_stream stream, server_config config,
    server_handler handler, std::shared_ptr<router> router_value, std::shared_ptr<server_log> log) {
   return std::make_shared<model<beast::tcp_stream>>(
      runtime, std::move(stream), std::move(config), std::move(handler), std::move(router_value), std::move(log));
}

std::shared_ptr<server_session> server_session::create(
    forge::asio::runtime& runtime, forge::net::tls::beast_tls_stream stream, server_config config,
    server_handler handler, std::shared_ptr<router> router_value, std::shared_ptr<server_log> log,
    forge::net::tls::context_snapshot_ptr snapshot) {
   return std::make_shared<model<forge::net::tls::beast_tls_stream>>(
      runtime, std::move(stream), std::move(config), std::move(handler), std::move(router_value),
      std::move(log), std::move(snapshot));
}


} // namespace forge::net::http::detail
}
