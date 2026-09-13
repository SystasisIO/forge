#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>

namespace forge::net::p2p {
class cancellation_latch;

namespace detail {

using autonat_dialback_wait = std::function<boost::asio::awaitable<std::optional<endpoint>>(
    std::shared_ptr<cancellation_latch>)>;

boost::asio::awaitable<reachability::result> async_exchange_autonat(
    stream channel, peer_id local, std::vector<endpoint> candidates, bool v2, std::uint64_t nonce,
    autonat_dialback_wait dialback, boost::asio::io_context& context, std::chrono::milliseconds timeout,
    std::shared_ptr<cancellation_latch> cancellation);

} // namespace detail
} // namespace forge::net::p2p
