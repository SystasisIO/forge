#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

import fixture.api_wasm;
import forge.execution.wasm.compiled_module;
import forge.chain.api.abi;
import forge.chain.protocol.values;
import forge.codec.hex;
import forge.codec.json;
import forge.raw.raw;
import forge.raw.exceptions;

namespace {
void require(bool condition, std::string_view message) {
   if (!condition) {
      throw std::runtime_error{std::string{message}};
   }
}

template <typename Error, typename Function>
void rejects(Function&& function, std::string_view message) {
   bool rejected = false;
   try {
      function();
   } catch (const Error&) {
      rejected = true;
   }
   require(rejected, message);
}
}

int main() {
   namespace wasm = forge::api::wasm;
   namespace abi_api = forge::chain::api;
   namespace chain = forge::chain::protocol;
   namespace execution = forge::execution::wasm;

   auto file = std::ifstream{API_WASM_WASM, std::ios::binary};
   require(file.good(), "missing WASM artifact");
   const auto code = std::vector<std::uint8_t>{std::istreambuf_iterator<char>{file}, {}};
   const auto abi_input = forge::codec::json::load<chain::abi_def>(API_WASM_ABI);
   const auto metadata_input = forge::codec::json::load<chain::abi_metadata>(API_WASM_METADATA);
   require(abi_input.ok() && metadata_input.ok(), "invalid generated ABI artifacts");
   const auto& abi = abi_input.value;
   const auto& metadata = metadata_input.value;
   const auto account = chain::account_name{chain::make_name("wasmproto").value};
   const auto invoke = [&](std::string_view action, const auto& input) {
      // A failed host assertion conservatively poisons an executor instance.
      auto module = execution::compiled_module{code};
      return module.invoke({account, chain::action_name{chain::make_name(action).value}, input}).data;
   };
   const auto json = [](std::string_view text) {
      const auto result = forge::codec::json::read<forge::variant>(text);
      require(result.ok(), "invalid fixture JSON");
      return result.value;
   };
   const auto check = [&](std::uint8_t index, std::string_view action, std::string_view type,
                          const auto& value, std::string_view input_json, std::string_view hex) {
      const auto expected = forge::codec::hex::decode(hex);
      require(forge::raw::pack(value) == expected, "native literal bytes differ");
      require(abi_api::abi_json_to_bin(abi, type, json(input_json)) == expected, "generated ABI literal bytes differ");
      require(invoke(action, expected) == expected, "guest direct DTO bytes differ");
      const auto fresh = invoke("fresh", forge::raw::pack(index));
      require(forge::raw::unpack_exact<forge::raw::bytes>(fresh) == expected,
              "independently constructed guest value differs");
      const auto symbolic = abi_api::abi_bin_to_json(abi, metadata, type, expected);
      require(abi_api::abi_json_to_bin(abi, metadata, type, symbolic) == expected,
              "symbolic ABI conversion differs");
   };
   const auto problem = forge::api::core::error_payload{
      .error = "E", .message = "msg", .retryable = true,
      .status_code = forge::api::core::status::conflict,
      .identity = {.category = "cat", .code = 0x01020304U},
      .details_codec = forge::api::core::codec_id{.value = "raw"},
      .details = forge::api::core::bytes{0, 0x80, 0xff},
   };
   check(0, "checkstep", "step", wasm::step{wasm::awaiting{
      {"catalog.read", {"q", {1}}, "reply", 25}, {1, {"s", {2}}}}},
      R"({"result":["awaiting",{"command":{"binding":"catalog.read","request":{"type":"q","data":"01"},"response_type":"reply","after_ms":25},"next":{"version":1,"state":{"type":"s","data":"02"}}}]})",
      "000c636174616c6f672e7265616401710101057265706c79190000000100000001730102");
   check(1, "checkstep", "step", wasm::step{wasm::completed{{"reply", {3}}}},
      R"({"result":["completed",{"output":{"type":"reply","data":"03"}}]})", "01057265706c790103");
   check(2, "checkstep", "step", wasm::step{problem},
      R"({"result":["error_payload",{"error":"E","message":"msg","retryable":true,"status_code":409,"identity":{"category":"cat","code":16909060},"details_codec":{"value":"raw"},"details":"0080ff"}]})",
      "020145036d73670199010000000000000363617404030201010372617701030080ff");
   check(3, "checkevent", "event", wasm::event{wasm::payload{"reply", {3}}},
      R"({"value":["payload",{"type":"reply","data":"03"}]})", "00057265706c790103");
   check(4, "checkevent", "event", wasm::event{problem},
      R"({"value":["error_payload",{"error":"E","message":"msg","retryable":true,"status_code":409,"identity":{"category":"cat","code":16909060},"details_codec":{"value":"raw"},"details":"0080ff"}]})",
      "010145036d73670199010000000000000363617404030201010372617701030080ff");
   check(5, "checkhttp", "http_command", wasm::http_command{forge::net::http::method::get, "/items?q=1", std::nullopt},
      R"({"method":2,"target":"/items?q=1","body":null})", "020000000a2f6974656d733f713d3100");
   check(6, "checkreply", "http_response", wasm::http_response{200, wasm::payload{"item", {4}}},
      R"({"status":200,"body":{"type":"item","data":"04"}})", "c80001046974656d0104");
   check(7, "checkreply", "http_response", wasm::http_response{204, std::nullopt},
      R"({"status":204,"body":null})", "cc0000");
   const auto context = wasm::context{wasm::subject_view{"issuer", "alice"}, {{"admission", {1}}}};
   check(8, "checkctx", "context", context,
      R"({"subject":{"issuer":"issuer","subject":"alice"},"facts":[{"type":"admission","data":"01"}]})",
      "010669737375657205616c696365010961646d697373696f6e0101");
   check(9, "checkctx", "context", wasm::context{}, R"({"subject":null,"facts":[]})", "0000");

   const auto valid = forge::codec::hex::decode("01057265706c790103");
   require(invoke("exact", forge::raw::pack(valid)) == valid, "exact guest decode changed bytes");
   auto trailing = valid;
   trailing.push_back(0);
   rejects<forge::raw::exceptions::codec_error>([&] { (void)forge::raw::unpack_exact<wasm::step>(trailing); },
                                                "native trailing bytes accepted");
   rejects<execution::exceptions::assertion_failed>([&] { (void)invoke("exact", forge::raw::pack(trailing)); },
                                                    "guest trailing bytes accepted");
   rejects<execution::exceptions::assertion_failed>([&] { (void)invoke("bounded", forge::raw::pack(valid)); },
                                                    "guest allocation bound ignored");
   const auto at_bound = forge::codec::hex::decode("01057265706c7900");
   require(invoke("bounded", forge::raw::pack(at_bound)) == at_bound, "guest inclusive byte boundary rejected");
   for (const auto action : {"checkstep", "checkevent"}) {
      rejects<execution::exceptions::assertion_failed>([&] { (void)invoke(action, forge::raw::bytes{0xff, 0xff, 0xff, 0xff, 0x0f}); },
                                                       "invalid guest variant accepted");
   }
   auto truncated = valid;
   truncated.pop_back();
   rejects<forge::raw::exceptions::range_error>([&] { (void)forge::raw::unpack_exact<wasm::step>(truncated); },
                                                "native truncated payload accepted");
   rejects<execution::exceptions::assertion_failed>([&] { (void)invoke("checkstep", truncated); },
                                                    "guest truncated payload accepted");
   rejects<abi_api::abi_serialization_error>([&] {
      (void)abi_api::abi_json_to_bin(abi, "step", json(R"({"result":["missing",{}]})"));
   }, "unknown ABI alternative accepted");
   const auto arguments = forge::raw::pack(std::tuple{context, fixture::request{7}});
   require(arguments == forge::codec::hex::decode(
      "010669737375657205616c696365010961646d697373696f6e010107000000"), "action argument literal differs");
   const auto action = std::ranges::find(abi.actions,
      chain::action_name{chain::make_name("access").value}, &chain::action_def::name);
   require(action != abi.actions.end(), "generated access action missing");
   require(abi_api::abi_json_to_bin(abi, action->type, json(
      R"({"trusted":{"subject":{"issuer":"issuer","subject":"alice"},"facts":[{"type":"admission","data":"01"}]},"input":{"item":7}})")) == arguments,
      "generated action arguments disagree with shared DTOs");
   const auto allowed = invoke("access", arguments);
   const auto output = forge::raw::unpack_exact<wasm::step>(allowed);
   require(std::get<wasm::completed>(output.result).output.type == "access_result" &&
           std::get<wasm::completed>(output.result).output.data == forge::raw::bytes{1},
           "shared product DTO and host context dispatch mismatch");
   require(abi_api::abi_json_to_bin(abi, "access_result", json(R"({"allowed":true})")) ==
           std::get<wasm::completed>(output.result).output.data, "product result ABI differs");

   require(invoke("unbox", forge::raw::pack(wasm::payload{"request", forge::raw::pack(fixture::request{7})})) ==
           forge::raw::pack(fixture::request{7}), "guest typed payload decode failed");
   const auto rejects_value = [&](std::string_view action, const auto& value) {
      rejects<execution::exceptions::assertion_failed>([&] { (void)invoke(action, forge::raw::pack(value)); },
                                                      "guest semantic validation accepted invalid value");
   };
   rejects_value("unbox", wasm::payload{"wrong", forge::raw::pack(fixture::request{7})});
   rejects_value("checkstep", wasm::step{wasm::completed{{"", {1}}}});
   rejects_value("next", wasm::continuation{2, {"state", {}}});
   rejects_value("plan", wasm::effect{"unknown", {"q", {}}, "reply", 0});
   rejects_value("plan", wasm::effect{"catalog.read", {"q", {}}, "reply", 26});
   rejects_value("response", wasm::http_response{200, std::nullopt});
   rejects_value("response", wasm::http_response{204, wasm::payload{"item", {}}});
   rejects_value("response", wasm::http_response{200, wasm::payload{"wrong", {}}});
   rejects_value("resume", wasm::event{wasm::payload{"item", {1}}});
   const auto resumed = forge::raw::pack(wasm::event{wasm::payload{"http_response",
      forge::raw::pack(wasm::http_response{200, wasm::payload{"item", {1}}})}});
   require(invoke("resume", resumed) == resumed, "guest full-response/body capability distinction failed");
   const auto status_only = forge::raw::pack(wasm::http_response{404, std::nullopt});
   require(invoke("response", status_only) == status_only, "guest explicit status-only branch rejected");

   const auto document_bytes = forge::codec::hex::decode("077b2278223a317d00");
   require(forge::raw::pack(fixture::json_request{{"{\"x\":1}"}, std::nullopt}) == document_bytes,
           "native opaque JSON document bytes differ");
   require(invoke("checkjson", document_bytes) == document_bytes, "guest opaque JSON document bytes differ");
   const auto bounded_document = forge::codec::hex::decode("077b2278223a317d");
   require(invoke("jsonbound", forge::raw::pack(bounded_document)) == bounded_document,
           "guest document byte bound rejected its exact boundary");
   rejects<execution::exceptions::assertion_failed>([&] {
      (void)invoke("jsonbound", forge::raw::pack(forge::raw::pack(forge::codec::json::document{"12345678"})));
   }, "guest document byte bound ignored");
   require(forge::raw::unpack_exact<forge::raw::bytes>(invoke("fresh", forge::raw::pack(std::uint8_t{10}))) == document_bytes,
           "independently constructed guest JSON document bytes differ");
   require(metadata.version == "forge::abi-metadata/1.1" && metadata.json_types &&
           *metadata.json_types == std::vector<std::string>{"json_document"}, "generated JSON semantics metadata missing");
   require(abi_api::abi_json_to_bin(abi, metadata, "json_request", json(R"({"parameters":{"x":1},"choice":null})")) ==
           document_bytes, "generated JSON ABI marker did not preserve native bytes");
   require(abi_api::abi_bin_to_json(abi, metadata, "json_request", document_bytes) ==
           json(R"({"parameters":{"x":1},"choice":null})"), "generated JSON ABI marker did not restore object");
   rejects<abi_api::abi_serialization_error>([&] {
      (void)abi_api::abi_bin_to_json(abi, metadata, "json_request",
         forge::raw::pack(fixture::json_request{{"{"}, std::nullopt}));
   }, "malformed opaque JSON document reached native JSON output");
   rejects<execution::exceptions::assertion_failed>([&] {
      (void)invoke("checkjson", forge::raw::bytes{10, 1});
   }, "truncated guest JSON document accepted");
}
