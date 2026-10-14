#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <vector>

import forge.api.core.types;
import forge.codec.hex;
import forge.raw.codec;
import forge.raw.exceptions;

enum class legacy_override_mode : std::uint16_t { value = 400 };

namespace forge::raw {
template <> struct enum_wire_type<legacy_override_mode> {
   using type = std::int32_t;
};
}

// Register this translation unit in its own executable, never alongside forge.raw.raw consumers.
// This proves portable-codec agreement with the frozen native layout, not WASM execution.
BOOST_AUTO_TEST_SUITE(codec_only_wire_values_baseline)

BOOST_AUTO_TEST_CASE(all_api_status_constants_use_explicit_int64_without_native_raw_traits) {
   using status = forge::api::core::status;
   static_assert(std::is_same_v<std::underlying_type_t<status>, std::uint16_t>);

   struct fixture {
      status value;
      std::uint16_t number;
      std::string_view codec_hex;
      std::string_view native_hex;
   };
   constexpr auto fixtures = std::array{
      fixture{status::ok, 0, "0000", "0000000000000000"},
      fixture{status::invalid_argument, 400, "9001", "9001000000000000"},
      fixture{status::unauthenticated, 401, "9101", "9101000000000000"},
      fixture{status::permission_denied, 403, "9301", "9301000000000000"},
      fixture{status::not_found, 404, "9401", "9401000000000000"},
      fixture{status::conflict, 409, "9901", "9901000000000000"},
      fixture{status::failed_precondition, 412, "9c01", "9c01000000000000"},
      fixture{status::resource_exhausted, 429, "ad01", "ad01000000000000"},
      fixture{status::deadline_exceeded, 504, "f801", "f801000000000000"},
      fixture{status::unavailable, 503, "f701", "f701000000000000"},
      fixture{status::internal, 500, "f401", "f401000000000000"},
      fixture{status::bad_gateway, 502, "f601", "f601000000000000"},
   };
   for (const auto& item : fixtures) {
      BOOST_TEST_CONTEXT("codec-only status=" << item.number) {
         const auto former = forge::codec::hex::decode(item.codec_hex);
         const auto native = forge::codec::hex::decode(item.native_hex);
         const auto encoded = forge::raw::pack(item.value);
         BOOST_CHECK_EQUAL(static_cast<std::uint16_t>(item.value), item.number);
         BOOST_CHECK_EQUAL(encoded.size(), 8U);
         BOOST_CHECK_EQUAL(forge::raw::pack_size(item.value), 8U);
         BOOST_CHECK_EQUAL(forge::codec::hex::encode(encoded), item.native_hex);
         BOOST_CHECK(encoded == native);
         BOOST_CHECK(forge::raw::unpack_exact<status>(native) == item.value);
         BOOST_CHECK_THROW(forge::raw::unpack_exact<status>(former), forge::raw::exceptions::range_error);
      }
   }
}

BOOST_AUTO_TEST_CASE(codec_only_status_array_preserves_underlying_width_while_vector_uses_shared_contract) {
   using status = forge::api::core::status;
   const auto array = std::array{status::invalid_argument, status::bad_gateway};
   const auto vector = std::vector{status::invalid_argument, status::bad_gateway};
   const auto array_fixture = forge::codec::hex::decode("9001f601");
   const auto vector_fixture = forge::codec::hex::decode("029001000000000000f601000000000000");

   BOOST_CHECK_EQUAL(forge::codec::hex::encode(forge::raw::pack(array)), "9001f601");
   BOOST_CHECK_EQUAL(forge::codec::hex::encode(forge::raw::pack(vector)), "029001000000000000f601000000000000");
   BOOST_CHECK((forge::raw::unpack_exact<std::array<status, 2>>(array_fixture) == array));
   BOOST_CHECK(forge::raw::unpack_exact<std::vector<status>>(vector_fixture) == vector);
}

BOOST_AUTO_TEST_CASE(codec_only_error_preserves_native_status_and_field_layout) {
   const auto value = forge::api::core::error_payload{
      .error = "E",
      .message = "msg",
      .retryable = true,
      .status_code = forge::api::core::status::conflict,
      .identity = {.category = "cat", .code = 0x01020304U},
      .details_codec = forge::api::core::codec_id{.value = "raw"},
      .details = forge::api::core::bytes{0x00U, 0x80U, 0xffU},
   };
   constexpr auto hex = std::string_view{
      "0145036d736701"    // error, message, retryable
      "9901000000000000"  // status_code: explicit int64
      "0363617404030201"  // identity
      "0103726177"        // details_codec
      "01030080ff"        // details
   };
   const auto expected = forge::codec::hex::decode(hex);
   const auto encoded = forge::raw::pack(value);
   BOOST_CHECK_EQUAL(forge::codec::hex::encode(encoded), hex);
   BOOST_CHECK_EQUAL(encoded.size(), 33U);
   BOOST_CHECK(forge::raw::unpack_exact<forge::api::core::error_payload>(expected) == value);
}

BOOST_AUTO_TEST_CASE(unopted_enums_keep_underlying_defaults_and_shared_decode_rejects_truncation) {
   enum class ordinary : std::uint16_t { value = 400 };
   BOOST_CHECK_EQUAL(forge::codec::hex::encode(forge::raw::pack(ordinary::value)), "9001");
   static_assert(!forge::raw::shared_enum_wire_v<legacy_override_mode>);
   BOOST_CHECK_EQUAL(forge::codec::hex::encode(forge::raw::pack(legacy_override_mode::value)), "9001");
   using status = forge::api::core::status;
   BOOST_CHECK_THROW(forge::raw::unpack_exact<status>(forge::codec::hex::decode("0000010000000000")),
                     forge::raw::exceptions::codec_error);
   BOOST_CHECK_THROW(forge::raw::unpack_exact<status>(forge::codec::hex::decode("ffffffffffffffff")),
                     forge::raw::exceptions::codec_error);
}

BOOST_AUTO_TEST_SUITE_END()
