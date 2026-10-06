#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>

#include <optional>
#include <string_view>

#include "stream_model.hxx"

namespace forge::net::stcp {

enum class connection_state : std::uint8_t {
   active,
   cancel_requested,
   close_requested,
   handed_off,
   closed,
};

struct connection::impl final : std::enable_shared_from_this<connection::impl> {
   impl(std::shared_ptr<detail::stream_backend> stream_value, tls::context_snapshot_ptr context_value,
      std::size_t read_chunk_size_value, transport::endpoint local, transport::endpoint remote,
      std::shared_ptr<void> lifetime_value);
   void start_terminal_worker();
   [[nodiscard]] bool valid() const noexcept;
   [[nodiscard]] transport::endpoint local_endpoint() const;
   [[nodiscard]] transport::endpoint remote_endpoint() const;
   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> bytes);
   boost::asio::awaitable<std::size_t> async_read_some(std::span<std::uint8_t> bytes);
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read();
   boost::asio::awaitable<void> async_close();
   void cancel() noexcept;
   [[nodiscard]] transport::stream_connection into_transport_stream();
   [[nodiscard]] bool request_terminal(connection_state requested) noexcept;
   void mark_closed_from_io() noexcept;
   void commit_handoff(const std::shared_ptr<detail::stream_model>& model);
   void claim_operation();
   void release_operation() noexcept;

   std::shared_ptr<detail::stream_backend> stream;
   tls::context_snapshot_ptr context;
   boost::asio::strand<boost::asio::any_io_executor> strand;
   std::shared_ptr<detail::io_gates> gates;
   std::shared_ptr<forge::asio::notification> terminal_completed;
   std::shared_ptr<std::exception_ptr> terminal_failure;
   std::size_t read_chunk_size = 64 * 1024;
   std::int64_t id = -1;
   transport::endpoint local_value;
   transport::endpoint remote_value;
   std::optional<forge::net::stcp::peer_certificate> certificate_value;
   certificate_chain chain_value;
   std::string alpn_value;
   mutable std::mutex state_mutex;
   connection_state state = connection_state::active;
   std::size_t active_operations = 0;
   std::shared_ptr<void> lifetime;
};

} // namespace forge::net::stcp
