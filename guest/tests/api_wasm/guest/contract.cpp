#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

import forge.contract;
import forge.raw.codec;
import forge.api.wasm.codec;
import fixture.api_wasm;

namespace protocol = forge::api::wasm;

class [[forge::contract("wasmproto")]] wasmproto : public forge::contract::context {
 public:
   using context::context;

   [[forge::action]] protocol::step checkstep(protocol::step value) { protocol::validate(value); return value; }
   [[forge::action]] protocol::event checkevent(protocol::event value) { protocol::validate(value); return value; }
   [[forge::action]] protocol::context checkctx(protocol::context value) { protocol::validate(value); return value; }
   [[forge::action]] protocol::http_command checkhttp(protocol::http_command value) { protocol::validate(value); return value; }
   [[forge::action]] protocol::http_response checkreply(protocol::http_response value) { protocol::validate(value); return value; }
   [[forge::action]] fixture::request unbox(protocol::payload value) {
      return protocol::decode<fixture::request>(value, "request");
   }
   [[forge::action]] protocol::effect plan(protocol::effect value) {
      protocol::validate(value, protocol::capability{
         .binding = "catalog.read", .request_type = "q", .response_type = "reply", .max_delay_ms = 25});
      return value;
   }
   [[forge::action]] protocol::continuation next(protocol::continuation value) {
      protocol::validate(value, 1, "state");
      return value;
   }
   [[forge::action]] protocol::http_response response(protocol::http_response value) {
      using presence = protocol::capability::body_presence;
      protocol::validate(value, protocol::capability::http_policy{
         .responses = {{200, {presence::required, "item"}}, {204, {presence::absent, {}}},
                       {404, {presence::status_only, {}}}}});
      return value;
   }
   [[forge::action]] protocol::step exact(std::vector<std::uint8_t> bytes) {
      return protocol::decode<protocol::step>(bytes);
   }
   [[forge::action]] protocol::event resume(protocol::event value) {
      using presence = protocol::capability::body_presence;
      const auto allowed = protocol::capability{
         .binding = "http.read", .request_type = "http_command", .response_type = "http_response",
         .http = protocol::capability::http_policy{
            .responses = {{200, {presence::required, "item"}}}}};
      const auto pending = protocol::effect{
         "http.read", protocol::encode("http_command", protocol::http_command{
            forge::net::http::method::get, "/item", {}}), "http_response", 0};
      protocol::validate(value, pending, allowed);
      return value;
   }
   [[forge::action]] protocol::step bounded(std::vector<std::uint8_t> bytes) {
      auto limits = protocol::limits{};
      limits.wire.max_bytes = 8;
      return protocol::decode<protocol::step>(bytes, limits);
   }
   [[forge::action]] protocol::step access(protocol::context trusted, fixture::request input) {
      // Fixture policy is not authentication. Real hosts construct trusted context.
      const auto allowed = trusted.subject && trusted.subject->issuer == "issuer" && input.item == 7;
      return {protocol::completed{{"access_result", forge::raw::pack(fixture::access_result{allowed})}}};
   }
   [[forge::action]] std::vector<std::uint8_t> fresh(std::uint8_t index) {
      const auto problem = forge::api::core::error_payload{
         .error = "E", .message = "msg", .retryable = true,
         .status_code = forge::api::core::status::conflict,
         .identity = {.category = "cat", .code = 0x01020304U},
         .details_codec = forge::api::core::codec_id{.value = "raw"},
         .details = forge::api::core::bytes{0, 0x80, 0xff},
      };
      switch (index) {
         case 0: return forge::raw::pack(protocol::step{protocol::awaiting{
            {"catalog.read", {"q", {1}}, "reply", 25}, {1, {"s", {2}}}}});
         case 1: return forge::raw::pack(protocol::step{protocol::completed{{"reply", {3}}}});
         case 2: return forge::raw::pack(protocol::step{problem});
         case 3: return forge::raw::pack(protocol::event{protocol::payload{"reply", {3}}});
         case 4: return forge::raw::pack(protocol::event{problem});
         case 5: return forge::raw::pack(protocol::http_command{forge::net::http::method::get, "/items?q=1", std::nullopt});
         case 6: return forge::raw::pack(protocol::http_response{200, protocol::payload{"item", {4}}});
         case 7: return forge::raw::pack(protocol::http_response{204, std::nullopt});
         case 8: return forge::raw::pack(protocol::context{
            protocol::subject_view{"issuer", "alice"}, {{"admission", {1}}}});
         case 9: return forge::raw::pack(protocol::context{});
         default: forge::contract::check(false, "fixture case");
      }
      return {};
   }
};
