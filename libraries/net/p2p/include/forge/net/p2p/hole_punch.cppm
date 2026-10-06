module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

export module forge.net.p2p.hole_punch;

import forge.net.p2p.endpoint;
import forge.net.p2p.identity;

export namespace forge::net::p2p {

struct hole_punch {
   enum class status : std::uint16_t {
      not_attempted = 0,
      prepared = 1,
      synced = 2,
      succeeded = 3,
      failed = 4,
   };

   struct options {
      std::chrono::milliseconds timeout{10'000};
      std::size_t max_observed_endpoints = 32;
      std::size_t max_message_size = 4 * 1024;
   };

   struct result {
      status value = status::not_attempted;
      std::vector<endpoint> attempted;
   };

   struct message {
      enum class message_kind : std::uint16_t {
         connect = 100,
         sync = 300,
      };

      message_kind kind = message_kind::connect;
      std::vector<endpoint> observed_endpoints;
   };

   struct codec {
      [[nodiscard]] static std::vector<std::uint8_t> encode(const message& value);
      [[nodiscard]] static message decode(std::span<const std::uint8_t> bytes);
      [[nodiscard]] static message decode(std::span<const std::uint8_t> bytes, options opts);
   };
};

} // namespace forge::net::p2p
