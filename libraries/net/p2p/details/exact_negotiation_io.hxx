#pragma once

#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>

namespace forge::net::p2p::detail {

template <typename Connection> class exact_negotiation_io {
 public:
   explicit exact_negotiation_io(Connection& connection) : connection_(connection) {}

   boost::asio::awaitable<void> write(protocol_negotiation::message value) {
      const auto payload = protocol_negotiation::encode_message(value);
      const auto frame = protocol_negotiation::encode_frame(payload);
      co_await connection_.async_write(frame);
   }

   boost::asio::awaitable<protocol_negotiation::message> read() {
      while (true) {
         try {
            auto frame = protocol_negotiation::decode_frame(buffer_);
            auto payload = std::move(frame.payload);
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(frame.consumed));
            co_return protocol_negotiation::decode_message(payload);
         } catch (const forge::exceptions::base& error) {
            const auto code = exceptions::code_of(error);
            if (!code || *code != exceptions::code::closed) {
               throw;
            }
         }
         if constexpr (std::is_same_v<Connection, forge::net::p2p::stream>) {
            auto chunk = co_await connection_.async_read();
            if (chunk.empty()) {
               FORGE_THROW_EXCEPTION(exceptions::closed, "multistream-select stream closed");
            }
            buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());
         } else {
            auto byte = std::array<std::uint8_t, 1>{};
            const auto size = co_await connection_.async_read_some(byte);
            if (size == 0) {
               FORGE_THROW_EXCEPTION(exceptions::closed, "multistream-select connection closed");
            }
            buffer_.push_back(byte.front());
         }
      }
   }

   [[nodiscard]] std::vector<std::uint8_t> release_buffer() && noexcept {
      return std::move(buffer_);
   }

 private:
   Connection& connection_;
   std::vector<std::uint8_t> buffer_;
};

struct exact_negotiation_result {
   protocol_id protocol;
   std::vector<std::uint8_t> buffered;
};

template <typename Connection>
boost::asio::awaitable<exact_negotiation_result>
select_protocol(Connection& connection, std::span<const protocol_id> protocols) {
   auto io = exact_negotiation_io<Connection>{connection};
   co_await io.write(protocol_negotiation::message{.kind = protocol_negotiation::message_kind::header,
                                                   .protocol = protocol_negotiation::multistream_v1});
   auto first = true;
   for (const auto& protocol : protocols) {
      co_await io.write(
          protocol_negotiation::message{.kind = protocol_negotiation::message_kind::protocol, .protocol = protocol});
      if (first) {
         auto header = co_await io.read();
         if (header.kind != protocol_negotiation::message_kind::header) {
            FORGE_THROW_EXCEPTION(exceptions::protocol_error, "multistream-select expected security header response");
         }
         first = false;
      }
      auto selected = co_await io.read();
      if (selected.kind == protocol_negotiation::message_kind::not_available) {
         continue;
      }
      if (selected.kind != protocol_negotiation::message_kind::protocol || selected.protocol.value != protocol.value) {
         FORGE_THROW_EXCEPTION(exceptions::protocol_error, "multistream-select selected unexpected security protocol");
      }
      co_return exact_negotiation_result{.protocol = protocol, .buffered = std::move(io).release_buffer()};
   }
   FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "remote peer supports no compatible security protocol");
}

template <typename Connection>
boost::asio::awaitable<exact_negotiation_result>
accept_protocol(Connection& connection, std::span<const protocol_id> protocols) {
   auto io = exact_negotiation_io<Connection>{connection};
   auto header = co_await io.read();
   if (header.kind != protocol_negotiation::message_kind::header) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "multistream-select expected security header");
   }
   co_await io.write(protocol_negotiation::message{.kind = protocol_negotiation::message_kind::header,
                                                   .protocol = protocol_negotiation::multistream_v1});
   while (true) {
      auto proposal = co_await io.read();
      if (proposal.kind != protocol_negotiation::message_kind::protocol) {
         FORGE_THROW_EXCEPTION(exceptions::protocol_error, "multistream-select expected security protocol proposal");
      }
      const auto found = std::ranges::find_if(
          protocols, [&proposal](const auto& value) { return value.value == proposal.protocol.value; });
      if (found != protocols.end()) {
         co_await io.write(
             protocol_negotiation::message{.kind = protocol_negotiation::message_kind::protocol, .protocol = *found});
         co_return exact_negotiation_result{.protocol = *found, .buffered = std::move(io).release_buffer()};
      }
      co_await io.write(protocol_negotiation::message{.kind = protocol_negotiation::message_kind::not_available,
                                                      .protocol = protocol_negotiation::not_available});
   }
}

template <typename Connection>
boost::asio::awaitable<protocol_id> negotiate_yamux(Connection& connection, bool outbound) {
   const auto yamux = protocol_id{.value = "/yamux/1.0.0"};
   if (outbound) {
      auto selected = co_await select_protocol(connection, std::span<const protocol_id>{&yamux, 1});
      co_return std::move(selected.protocol);
   }
   auto selected = co_await accept_protocol(connection, std::span<const protocol_id>{&yamux, 1});
   co_return std::move(selected.protocol);
}

} // namespace forge::net::p2p::detail
