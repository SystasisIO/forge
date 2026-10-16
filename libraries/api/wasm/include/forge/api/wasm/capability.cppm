module;

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#if !defined(FORGE_CONTRACT_GUEST)
#include <boost/describe.hpp>
#endif

export module forge.api.wasm.capability;

import forge.raw.enum_wire_type;

export namespace forge::api::wasm {

// Selected by the host. These validation options grant nothing when supplied by a guest.
struct capability {
   enum class body_presence : std::uint8_t {
      absent,
      optional,
      required,
      status_only,
   };

   struct body_policy {
      body_presence presence = body_presence::absent;
      std::string type;

      bool operator==(const body_policy&) const = default;
   };

   struct http_status {
      std::uint16_t status = 0;
      body_policy body;

      bool operator==(const http_status&) const = default;
   };

   struct http_policy {
      body_policy request_body;
      std::vector<http_status> responses;

      bool operator==(const http_policy&) const = default;
   };

   std::string binding;
   std::string request_type;
   std::string response_type;
   std::uint32_t max_delay_ms = 0;
   std::optional<http_policy> http;

   bool operator==(const capability&) const = default;
};

#if !defined(FORGE_CONTRACT_GUEST)
BOOST_DESCRIBE_ENUM(capability::body_presence, absent, optional, required, status_only)
BOOST_DESCRIBE_STRUCT(capability::body_policy, (), (presence, type))
BOOST_DESCRIBE_STRUCT(capability::http_status, (), (status, body))
BOOST_DESCRIBE_STRUCT(capability::http_policy, (), (request_body, responses))
BOOST_DESCRIBE_STRUCT(capability, (), (binding, request_type, response_type, max_delay_ms, http))
#endif

} // namespace forge::api::wasm

export namespace forge::raw {

template <> struct enum_wire_type<api::wasm::capability::body_presence> {
   using type = std::uint8_t;
   static constexpr bool shared = true;
};

} // namespace forge::raw
