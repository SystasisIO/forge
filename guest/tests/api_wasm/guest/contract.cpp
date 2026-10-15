#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
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
   [[forge::action]] protocol::http_stream_open checkopen(protocol::http_stream_open value) {
      protocol::validate(value);
      return value;
   }
   [[forge::action]] protocol::http_stream_read checkread(protocol::http_stream_read value) {
      protocol::validate(value);
      return value;
   }
   [[forge::action]] protocol::http_stream_event checkitem(protocol::http_stream_event value) {
      protocol::validate(value);
      return value;
   }
   [[forge::action]] protocol::http_stream_event streamlimit(protocol::http_stream_event value) {
      auto bounds = protocol::limits{};
      bounds.payload_bytes = 4;
      protocol::validate(value, "stream_item", bounds);
      return value;
   }
   [[forge::action]] fixture::json_request checkjson(fixture::json_request value) { return value; }
   [[forge::action]] forge::codec::json::document jsonbound(std::vector<std::uint8_t> bytes) {
      return forge::raw::unpack_exact<forge::codec::json::document>(bytes,
         forge::raw::unpack_limits{.max_container_elements = 8, .max_total_container_elements = 8,
                                  .max_bytes = 8, .first_container_elements = 8});
   }
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
   [[forge::action]] protocol::step streamstart(fixture::stream_request input) {
      forge::contract::check(input.limit > 0 && input.limit <= 4, "fixture stream limit");
      const auto open =
          protocol::http_stream_open{{forge::net::http::method::get, "/stream", {}}, "stream_item", "END"};
      protocol::validate(open);
      return {protocol::awaiting{{"stream.open", protocol::encode("http_stream_open", open), "http_stream_event", 0},
                                 {1, protocol::encode("stream_state", fixture::stream_state{input.limit})}}};
   }
   [[forge::action]] protocol::step streamnext(protocol::continuation next, protocol::event input) {
      protocol::validate(next, 1, "stream_state");
      protocol::validate(input);
      auto state = protocol::decode<fixture::stream_state>(next.state, "stream_state");
      forge::contract::check(state.limit > 0 && state.limit <= 4 && state.received <= state.limit,
                             "fixture stream state");
      if (const auto* error = std::get_if<forge::api::core::error_payload>(&input.value)) {
         return {*error};
      }
      const auto item =
          protocol::decode<protocol::http_stream_event>(std::get<protocol::payload>(input.value), "http_stream_event");
      protocol::validate(item, "stream_item");
      if (state.finishing) {
         forge::contract::check(item.ended && !item.item, "fixture stream finish acknowledgement");
         return {protocol::completed{
             protocol::encode("stream_summary", fixture::stream_summary{state.received, state.sum})}};
      }
      forge::contract::check(!item.ended && item.item.has_value(), "fixture stream ended before requested items");
      const auto value = protocol::decode<fixture::stream_item>(*item.item, "stream_item");
      forge::contract::check(value.value <= 100, "fixture stream item limit");
      ++state.received;
      state.sum += value.value;
      state.finishing = state.received == state.limit;
      const auto read = protocol::http_stream_read{
          protocol::encode("stream_item", fixture::stream_item{value.value * 10}), state.finishing};
      protocol::validate(read);
      return {protocol::awaiting{{"stream.read", protocol::encode("http_stream_read", read), "http_stream_event", 0},
                                 {1, protocol::encode("stream_state", state)}}};
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
         case 10: return forge::raw::pack(fixture::json_request{{"{\"x\":1}"}, std::nullopt});
         case 11:
            return forge::raw::pack(
                protocol::http_stream_open{{forge::net::http::method::get, "/x", {}}, "item", "END"});
         case 12:
            return forge::raw::pack(protocol::http_stream_read{protocol::payload{"item", {1, 2}}, true});
         case 13:
            return forge::raw::pack(protocol::http_stream_event{200, false, protocol::payload{"item", {1, 2}}});
         case 14:
            return forge::raw::pack(protocol::http_stream_event{200, true, {}});
         default: forge::contract::check(false, "fixture case");
      }
      return {};
   }
};
