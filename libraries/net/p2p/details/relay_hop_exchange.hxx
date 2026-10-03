#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>

#include "resource_stream.hxx"

namespace forge::net::p2p::detail {

struct relay_hop_exchange {
   forge::net::p2p::stream stream;
   std::vector<std::uint8_t> buffered;
   relay::hop_message response;
};

using relay_stream_opener = std::function<boost::asio::awaitable<forge::net::p2p::stream>(stream_admission_handler)>;

boost::asio::awaitable<relay_hop_exchange>
async_exchange_relay_hop(boost::asio::io_context& context, std::chrono::milliseconds timeout, std::string operation,
                         relay_stream_opener open_stream, relay::hop_message request, std::size_t max_message_size,
                         std::shared_ptr<cancellation_latch> cancellation = {});

} // namespace forge::net::p2p::detail
