module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

export module forge.net.p2p.reachability;

import forge.net.p2p.endpoint;
import forge.net.p2p.identity;

export namespace forge::net::p2p {

struct reachability {
   enum class state : std::uint16_t {
      unknown = 0,
      publicly_reachable = 1,
      private_network = 2,
      blocked = 3,
      relay_only = 4,
   };

   enum class dial_status : std::uint16_t {
      ok = 0,
      dial_error = 100,
      dial_refused = 101,
      bad_request = 200,
      internal_error = 300,
   };

   struct options {
      std::chrono::milliseconds timeout{10'000};
      std::size_t max_endpoints = 32;
      std::size_t max_message_size = 64 * 1024;
      std::size_t max_data_response_size = 4 * 1024;
   };

   struct result {
      state value = state::unknown;
      std::optional<endpoint> observed;
      std::string status_text;
   };

   struct observation {
      peer_id observer;
      state value = state::unknown;
      std::optional<endpoint> observed;
      std::chrono::system_clock::time_point observed_at{};
      std::chrono::system_clock::time_point expires_at{};
   };

   struct peer_info {
      peer_id peer;
      std::vector<endpoint> endpoints;
   };

   struct dial_response {
      dial_status status = dial_status::bad_request;
      std::string status_text;
      std::optional<endpoint> endpoint;
   };

   struct message {
      enum class message_kind : std::uint16_t {
         dial = 0,
         dial_response = 1,
      };

      message_kind kind = message_kind::dial;
      std::optional<peer_info> peer;
      std::optional<dial_response> response;
   };

   struct v2 {
      enum class dial_status : std::uint16_t {
         unused = 0,
         dial_error = 100,
         dial_back_error = 101,
         ok = 200,
      };

      enum class response_status : std::uint16_t {
         internal_error = 0,
         request_rejected = 100,
         dial_refused = 101,
         ok = 200,
      };

      enum class dial_back_status : std::uint16_t {
         ok = 0,
      };

      struct dial_request {
         std::vector<endpoint> endpoints;
         std::uint64_t nonce = 0;
      };

      struct dial_response {
         response_status status = response_status::internal_error;
         std::uint32_t index = 0;
         dial_status dial_status = dial_status::unused;
      };

      struct dial_data_request {
         std::uint32_t index = 0;
         std::uint64_t bytes = 0;
      };

      struct dial_data_response {
         std::vector<std::uint8_t> data;
      };

      struct dial_back {
         std::uint64_t nonce = 0;
      };

      struct dial_back_response {
         dial_back_status status = dial_back_status::ok;
      };

      struct message {
         enum class kind : std::uint16_t {
            dial_request = 1,
            dial_response = 2,
            dial_data_request = 3,
            dial_data_response = 4,
         };

         kind type = kind::dial_request;
         std::optional<dial_request> dial_request;
         std::optional<dial_response> dial_response;
         std::optional<dial_data_request> dial_data_request;
         std::optional<dial_data_response> dial_data_response;
      };
   };

   struct codec {
      [[nodiscard]] static std::vector<std::uint8_t> encode_v1(const message& value);
      [[nodiscard]] static std::vector<std::uint8_t> encode_v1(const message& value, options opts);
      [[nodiscard]] static message decode_v1(std::span<const std::uint8_t> bytes);
      [[nodiscard]] static message decode_v1(std::span<const std::uint8_t> bytes, options opts);

      [[nodiscard]] static std::vector<std::uint8_t> encode_v2(const v2::message& value);
      [[nodiscard]] static std::vector<std::uint8_t> encode_v2(const v2::message& value, options opts);
      [[nodiscard]] static v2::message decode_v2(std::span<const std::uint8_t> bytes);
      [[nodiscard]] static v2::message decode_v2(std::span<const std::uint8_t> bytes, options opts);

      [[nodiscard]] static std::vector<std::uint8_t> encode_v2_dial_back(const v2::dial_back& value);
      [[nodiscard]] static v2::dial_back decode_v2_dial_back(std::span<const std::uint8_t> bytes);
      [[nodiscard]] static v2::dial_back decode_v2_dial_back(std::span<const std::uint8_t> bytes, options opts);

      [[nodiscard]] static std::vector<std::uint8_t>
      encode_v2_dial_back_response(const v2::dial_back_response& value);
      [[nodiscard]] static v2::dial_back_response
      decode_v2_dial_back_response(std::span<const std::uint8_t> bytes);
      [[nodiscard]] static v2::dial_back_response
      decode_v2_dial_back_response(std::span<const std::uint8_t> bytes, options opts);
   };
};

} // namespace forge::net::p2p
