#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import fixture.wire_values;
import forge.chain.api.abi;
import forge.chain.protocol.values;
import forge.codec.hex;
import forge.codec.json;
import forge.raw.raw;
import forge.raw.exceptions;
import forge.tooling.testing.host;

namespace {

void require(bool condition, std::string_view message) {
   if (!condition) {
      throw std::runtime_error{std::string{message}};
   }
}

template <typename Error, typename Function> void rejects(Function&& function, std::string_view message) {
   auto rejected = false;
   try {
      function();
   } catch (const Error&) {
      rejected = true;
   }
   require(rejected, message);
}

template <typename Function>
void rejects_abi(Function&& function, forge::chain::api::abi_error_code code, std::string_view message) {
   auto rejected = false;
   try {
      function();
   } catch (const forge::chain::api::abi_serialization_error& error) {
      require(error.diagnostic().code == code, "wrong ABI rejection code");
      rejected = true;
   }
   require(rejected, message);
}

} // namespace

int main() {
   namespace api = forge::chain::api;
   namespace protocol = forge::chain::protocol;
   using status = forge::api::core::status;
   using method = forge::net::http::method;

   auto file = std::ifstream{WIRE_VALUES_WASM, std::ios::binary};
   require(file.good(), "cannot read WASM artifact");
   const auto code = std::vector<std::uint8_t>{std::istreambuf_iterator<char>{file}, {}};
   const auto abi_input = forge::codec::json::load<protocol::abi_def>(WIRE_VALUES_ABI);
   require(abi_input.ok(), "cannot read generated ABI");
   const auto& abi = abi_input.value;
   const auto metadata_input = forge::codec::json::load<protocol::abi_metadata>(WIRE_VALUES_METADATA);
   require(metadata_input.ok(), "cannot read generated enum metadata");
   const auto& metadata = metadata_input.value;
   auto host = forge::tooling::testing::host{};
   const auto account = protocol::make_name("wirevalues").value;
   const auto invoke = [&](std::string_view action, const auto& bytes) {
      return host.invoke(code, account, account, protocol::make_name(action).value, bytes).return_value;
   };
   const auto alias = [&](std::string_view name, std::string_view target) {
      const auto found = std::ranges::find(abi.types, name, &protocol::type_def::new_type_name);
      require(found != abi.types.end() && found->type == target, "wrong generated enum wire alias");
      const auto enum_info = std::ranges::find(metadata.enums, name, &protocol::abi_enum_def::name);
      require(enum_info != metadata.enums.end() && enum_info->type == target, "wrong generated enum metadata");
   };
   alias("status", "int64");
   alias("frame_kind", "int64");
   alias("method_kind", "int64");
   alias("stream_direction", "int64");
   alias("capability", "int64");
   alias("surface", "int64");
   alias("method", "int32");
   alias("signed_value", "int64");
   alias("unsigned_value", "uint64");
   alias("narrow_value", "int8");
   alias("legacy_value", "uint16");
   alias("signed_extreme", "int64");

   const auto check = [&](std::string_view action, std::string_view type, const auto& value,
                          forge::variant json, std::string_view hex) {
      const auto expected = forge::codec::hex::decode(hex);
      require(forge::raw::pack(value) == expected, "native literal mismatch");
      require(api::abi_json_to_bin(abi, type, json) == expected, "ABI literal mismatch");
      const auto named = api::abi_bin_to_json(abi, metadata, type, expected);
      require(named.is_string(), "enum metadata must decode a symbolic name");
      require(api::abi_json_to_bin(abi, metadata, type, named) == expected, "metadata ABI literal mismatch");
      require(invoke(action, expected) == expected, "guest literal mismatch");
   };
   const auto check_codes = [&](std::string_view action, std::string_view type,
                                const auto& values, const auto& literals) {
      require(values.size() == literals.size(), "enum fixture count mismatch");
      for (auto index = std::size_t{}; index < values.size(); ++index) {
         check(action, type, values[index], forge::variant{static_cast<int>(values[index])}, literals[index]);
      }
   };
   using frame = forge::api::core::frame_kind;
   check_codes("checkframe", "frame_kind",
      std::array{frame::request, frame::response, frame::error, frame::cancel, frame::stream_item,
                 frame::stream_end, frame::session_hello, frame::stream_window},
      std::array{"0100000000000000", "0200000000000000", "0300000000000000", "0400000000000000",
                 "0500000000000000", "0600000000000000", "0700000000000000", "0800000000000000"});
   using kind = forge::api::core::method_kind;
   check_codes("checkkind", "method_kind",
      std::array{kind::unary, kind::server_stream, kind::client_stream, kind::bidirectional_stream},
      std::array{"0100000000000000", "0200000000000000", "0300000000000000", "0400000000000000"});
   using direction = forge::api::core::stream_direction;
   check_codes("checkdir", "stream_direction", std::array{direction::input, direction::output},
      std::array{"0100000000000000", "0200000000000000"});
   using capability = forge::api::core::capability;
   check_codes("checkcap", "capability",
      std::array{capability::unary, capability::server_stream, capability::client_stream,
                 capability::bidirectional_stream, capability::stream_window},
      std::array{"0100000000000000", "0200000000000000", "0400000000000000", "0800000000000000", "1000000000000000"});
   using surface = forge::api::core::surface;
   check_codes("checksurface", "surface", std::array{surface::none, surface::local, surface::remote},
      std::array{"0000000000000000", "0100000000000000", "0200000000000000"});
   constexpr auto statuses = std::array{
      status::ok, status::invalid_argument, status::unauthenticated, status::permission_denied,
      status::not_found, status::conflict, status::failed_precondition, status::resource_exhausted,
      status::deadline_exceeded, status::unavailable, status::internal, status::bad_gateway};
   constexpr auto status_bytes = std::array<std::string_view, 12>{
      "0000000000000000", "9001000000000000", "9101000000000000", "9301000000000000",
      "9401000000000000", "9901000000000000", "9c01000000000000", "ad01000000000000",
      "f801000000000000", "f701000000000000", "f401000000000000", "f601000000000000"};
   for (auto index = std::size_t{}; index < statuses.size(); ++index) {
      check("checkstatus", "status", statuses[index], forge::variant{static_cast<int>(statuses[index])}, status_bytes[index]);
   }
   constexpr auto methods = std::array{method::unknown, method::delete_, method::get, method::head,
                                      method::options, method::patch, method::post, method::put};
   constexpr auto method_bytes = std::array<std::string_view, 8>{
      "00000000", "01000000", "02000000", "03000000", "04000000", "05000000", "06000000", "07000000"};
   for (auto index = std::size_t{}; index < methods.size(); ++index) {
      check("checkmethod", "method", methods[index], forge::variant{static_cast<int>(methods[index])}, method_bytes[index]);
   }
   check("signednum", "signed_value", fixture::signed_value::low, forge::variant{-32768}, "0080ffffffffffff");
   check("signednum", "signed_value", fixture::signed_value::negative, forge::variant{-7}, "f9ffffffffffffff");
   check("signednum", "signed_value", fixture::signed_value::high, forge::variant{32767}, "ff7f000000000000");
   check("unsignednum", "unsigned_value", fixture::unsigned_value::high,
         forge::variant{std::uint64_t{18446744073709551615ULL}}, "ffffffffffffffff");
   check("unsignednum", "unsigned_value", fixture::unsigned_value::low, forge::variant{0}, "0000000000000000");
   check("narrownum", "narrow_value", fixture::narrow_value::low, forge::variant{-128}, "80");
   check("narrownum", "narrow_value", fixture::narrow_value::high, forge::variant{127}, "7f");
   check("legacy", "legacy_value", fixture::legacy_value::value, forge::variant{400}, "9001");
   check("wideint", "signed_extreme", fixture::signed_extreme::low,
         forge::variant{"-9223372036854775808"}, "0000000000000080");
   check("wideint", "signed_extreme", fixture::signed_extreme::high,
         forge::variant{"9223372036854775807"}, "ffffffffffffff7f");

   const auto value = fixture::packet{
      .error = {.error = "E", .message = "msg", .retryable = true, .status_code = status::conflict,
                .identity = {.category = "cat", .code = 0x01020304U},
                .details_codec = forge::api::core::codec_id{.value = "raw"},
                .details = forge::api::core::bytes{0x00U, 0x80U, 0xffU}},
      .verb = method::post, .statuses = {status::invalid_argument, status::bad_gateway},
      .fixed = {status::invalid_argument, status::bad_gateway},
      .optional = status::ok, .choice = method::get,
   };
   const auto expected = forge::codec::hex::decode(
      "0145036d73670199010000000000000363617404030201010372617701030080ff"
      "06000000" "029001000000000000f601000000000000" "9001f601"
      "f9ffffffffffffff" "ffffffffffffffff" "80" "010000000000000000" "0102000000");
   require(forge::raw::pack(value) == expected, "native nested error literal mismatch");
   require(invoke("fresh", std::vector<std::uint8_t>{}) == expected, "guest-generated value literal mismatch");
   require(invoke("echo", expected) == expected, "guest nested error roundtrip mismatch");
   require(forge::raw::unpack_exact<fixture::packet>(expected) == value, "native nested decode mismatch");
   const auto json = forge::codec::json::read<forge::variant>(R"({
      "error":{"error":"E","message":"msg","retryable":true,"status_code":409,
               "identity":{"category":"cat","code":16909060},"details_codec":{"value":"raw"},"details":"0080ff"},
      "verb":6,"statuses":[400,502],"fixed":[400,502],"signed_number":-7,
      "unsigned_number":"18446744073709551615","narrow":-128,"optional":0,"choice":["method",2]
   })");
   require(json.ok(), "invalid fixture JSON");
   require(api::abi_json_to_bin(abi, "packet", json.value) == expected, "ABI nested error literal mismatch");
   const auto named_json = forge::codec::json::read<forge::variant>(R"({
      "error":{"error":"E","message":"msg","retryable":true,"status_code":"conflict",
               "identity":{"category":"cat","code":16909060},"details_codec":{"value":"raw"},"details":"0080ff"},
      "verb":"post","statuses":["invalid_argument","bad_gateway"],"fixed":[400,502],"signed_number":"negative",
      "unsigned_number":"high","narrow":"low","optional":"ok","choice":["method","get"]
   })");
   require(named_json.ok(), "invalid symbolic fixture JSON");
   require(api::abi_json_to_bin(abi, metadata, "packet", named_json.value) == expected,
           "metadata nested error literal mismatch");
   require(api::abi_json_to_bin(abi, "packet", api::abi_bin_to_json(abi, "packet", expected)) == expected,
           "ABI nested error roundtrip mismatch");

   auto alternative = value;
   alternative.error.details_codec.reset();
   alternative.error.details.reset();
   alternative.optional.reset();
   alternative.choice = status::unavailable;
   alternative.statuses.clear();
   const auto alternate_bytes = forge::raw::pack(alternative);
   require(invoke("echo", alternate_bytes) == alternate_bytes, "optional/variant guest parity mismatch");
   require(api::abi_json_to_bin(abi, "packet", api::abi_bin_to_json(abi, "packet", alternate_bytes)) == alternate_bytes,
           "optional/variant ABI parity mismatch");
   require(api::abi_json_to_bin(abi, metadata, "status", forge::variant{"conflict"}) ==
              forge::codec::hex::decode("9901000000000000"), "symbolic status metadata mismatch");

   rejects<forge::raw::exceptions::codec_error>([&] { (void)forge::raw::pack(static_cast<fixture::narrow_value>(128)); },
                                               "native narrowing accepted");
   rejects<forge::tooling::testing::exceptions::assertion_failure>(
      [&] { (void)invoke("badnarrow", std::vector<std::uint8_t>{}); }, "guest narrowing accepted");
   for (const auto hex : {"0000010000000000", "ffffffffffffffff"}) {
      const auto invalid = forge::codec::hex::decode(hex);
      rejects<forge::raw::exceptions::codec_error>([&] { (void)forge::raw::unpack_exact<status>(invalid); },
                                                  "native invalid status accepted");
      rejects<forge::tooling::testing::exceptions::assertion_failure>([&] { (void)invoke("checkstatus", invalid); },
                                                                    "guest invalid status accepted");
   }
   const auto short_status = forge::codec::hex::decode("9001");
   rejects<forge::raw::exceptions::range_error>([&] { (void)forge::raw::unpack_exact<status>(short_status); },
                                               "native short status accepted");
   rejects<forge::tooling::testing::exceptions::assertion_failure>([&] { (void)invoke("checkstatus", short_status); },
                                                                 "guest short status accepted");
   auto truncated = expected;
   truncated.pop_back();
   rejects<forge::raw::exceptions::range_error>([&] { (void)forge::raw::unpack_exact<fixture::packet>(truncated); },
                                               "native truncation accepted");
   rejects<forge::tooling::testing::exceptions::assertion_failure>([&] { (void)invoke("echo", truncated); },
                                                                 "guest truncation accepted");
   rejects_abi([&] { (void)api::abi_bin_to_json(abi, "packet", truncated); }, api::abi_error_code::invalid_binary,
               "ABI truncation accepted");
   auto trailing = expected;
   trailing.push_back(0);
   rejects<forge::raw::exceptions::codec_error>([&] { (void)forge::raw::unpack_exact<fixture::packet>(trailing); },
                                               "native trailing bytes accepted");
   rejects<forge::tooling::testing::exceptions::assertion_failure>(
      [&] { (void)invoke("decode", forge::raw::pack(trailing)); }, "guest exact trailing bytes accepted");
   require(invoke("echo", trailing) == expected, "CDT action trailing-byte behavior changed");
   rejects_abi([&] { (void)api::abi_bin_to_json(abi, "packet", trailing); }, api::abi_error_code::trailing_bytes,
               "ABI trailing bytes accepted");
}
