module;

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

export module forge.api.wasm.capability;

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
   };

   struct http_status {
      std::uint16_t status = 0;
      body_policy body;
   };

   struct http_policy {
      body_policy request_body;
      std::vector<http_status> responses;
   };

   std::string binding;
   std::string request_type;
   std::string response_type;
   std::uint32_t max_delay_ms = 0;
   std::optional<http_policy> http;
};

} // namespace forge::api::wasm
