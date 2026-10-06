#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>

#include "stream_backend.hxx"

namespace forge::net::stcp::detail {

enum class io_stop_reason : std::uint8_t {
   none,
   closed,
   canceled,
};

struct io_gates {
   io_gates();
   ~io_gates();

   boost::asio::awaitable<forge::asio::gate::ticket> acquire(forge::asio::gate& gate);
   void stop(io_stop_reason value) noexcept;
   [[nodiscard]] bool stopped() const noexcept;
   [[noreturn]] void throw_stopped() const;

   forge::asio::gate read;
   forge::asio::gate write;
   forge::asio::notification terminal_requested;
   std::atomic<io_stop_reason> reason{io_stop_reason::none};
};

[[noreturn]] void terminalize_io_error(stream_backend& stream, io_gates& gates,
      const boost::system::error_code& error);
[[noreturn]] void terminalize_closed(stream_backend* stream, io_gates& gates, std::string_view message);

} // namespace forge::net::stcp::detail
