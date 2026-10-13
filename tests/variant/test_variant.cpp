#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <flat_map>
#include <forge/exceptions/macros.hpp>
#include <limits>
#include <string>

import forge.variant.exceptions;
import forge.variant.value;
import forge.variant.conversion;
import forge.variant.containers;
import forge.variant.chrono;
import forge.variant.multiprecision;
import forge.variant.format;
import forge.variant.described;
import forge.exceptions;
import forge.codec.base64;
using namespace forge;
using std::string;

namespace {
enum class described_access : int64_t { read = 1, write = 2 };
BOOST_DESCRIBE_ENUM(described_access, read, write)

struct described_variant_config {
   std::string name = "default";
   uint32_t limit = 7;
   described_access access = described_access::read;

   bool operator==(const described_variant_config&) const = default;
};
BOOST_DESCRIBE_STRUCT(described_variant_config, (), (name, limit, access))
} // namespace

BOOST_AUTO_TEST_SUITE(variant_test_suite)
BOOST_AUTO_TEST_CASE(variant_portable_integer_construction_preserves_full_ranges) {
   const auto signed_long = forge::variant{std::numeric_limits<long>::min()};
   BOOST_REQUIRE(signed_long.is_int64());
   BOOST_TEST(signed_long.as_int64() == std::numeric_limits<long>::min());
   const auto unsigned_long = forge::variant{std::numeric_limits<unsigned long>::max()};
   BOOST_REQUIRE(unsigned_long.is_uint64());
   BOOST_TEST(unsigned_long.as_uint64() == std::numeric_limits<unsigned long>::max());
   const auto signed_long_long = forge::variant{std::numeric_limits<long long>::min()};
   BOOST_REQUIRE(signed_long_long.is_int64());
   BOOST_TEST(signed_long_long.as_int64() == std::numeric_limits<long long>::min());
   const auto unsigned_long_long = forge::variant{std::numeric_limits<unsigned long long>::max()};
   BOOST_REQUIRE(unsigned_long_long.is_uint64());
   BOOST_TEST(unsigned_long_long.as_uint64() == std::numeric_limits<unsigned long long>::max());
}

BOOST_AUTO_TEST_CASE(boost_describe_struct_variant_roundtrip) {
   const described_variant_config original{"workspace", 42, described_access::write};

   forge::variant as_variant;
   forge::to_variant(original, as_variant);

   const auto& object = as_variant.get_object();
   BOOST_CHECK_EQUAL(object["name"].as_string(), "workspace");
   BOOST_CHECK_EQUAL(object["limit"].as_uint64(), 42u);
   BOOST_CHECK_EQUAL(object["access"].as_string(), "write");

   described_variant_config roundtrip;
   forge::from_variant(as_variant, roundtrip);
   BOOST_CHECK(original == roundtrip);
}

BOOST_AUTO_TEST_CASE(variant_equality_covers_all_value_kinds) {
   const auto object = forge::variant{forge::mutable_variant_object{}("enabled", true)(
       "payload", forge::blob{{0x01U, 0x02U}})("nested", forge::variants{forge::variant{nullptr}})};
   const auto equal = forge::variant{forge::mutable_variant_object{}("enabled", true)(
       "payload", forge::blob{{0x01U, 0x02U}})("nested", forge::variants{forge::variant{nullptr}})};
   const auto different = forge::variant{forge::mutable_variant_object{}("enabled", false)(
       "payload", forge::blob{{0x01U, 0x02U}})("nested", forge::variants{forge::variant{nullptr}})};

   BOOST_CHECK(object == equal);
   BOOST_CHECK(object != different);
   BOOST_CHECK(forge::variant{true} == forge::variant{true});
   BOOST_CHECK(forge::variant{true} != forge::variant{false});
   BOOST_CHECK(forge::variant{nullptr} == forge::variant{nullptr});
   BOOST_CHECK(forge::variant{forge::blob{{0x01U}}} == forge::variant{forge::blob{{0x01U}}});
   BOOST_CHECK(forge::variant{forge::blob{{0x01U}}} != forge::variant{forge::blob{{0x02U}}});
   BOOST_CHECK(forge::variant{forge::variants{forge::variant{1}}} != forge::variant{true});
}

BOOST_AUTO_TEST_CASE(variant_object_equality_is_independent_of_member_insertion_order) {
   const auto left = forge::variant{forge::mutable_variant_object{}("first", 1)("second", "value")};
   const auto reordered = forge::variant{forge::mutable_variant_object{}("second", "value")("first", 1)};
   const auto different = forge::variant{forge::mutable_variant_object{}("second", "other")("first", 1)};
   const auto duplicates =
       forge::variant{forge::mutable_variant_object{}("same", forge::variant{1})("same", forge::variant{2})};
   const auto reordered_duplicates =
       forge::variant{forge::mutable_variant_object{}("same", forge::variant{2})("same", forge::variant{1})};
   const auto mismatched_duplicates =
       forge::variant{forge::mutable_variant_object{}("same", forge::variant{1})("other", forge::variant{2})};

   BOOST_CHECK(left == reordered);
   BOOST_CHECK(reordered == left);
   BOOST_CHECK(left != different);
   BOOST_CHECK(duplicates == reordered_duplicates);
   BOOST_CHECK(duplicates != mismatched_duplicates);
}

BOOST_AUTO_TEST_CASE(variant_equality_rejects_structural_scalar_mismatches_without_conversion) {
   const auto object = forge::variant{forge::mutable_variant_object{}("value", 1)};
   const auto array = forge::variant{forge::variants{forge::variant{1}}};
   const auto blob_value = forge::variant{forge::blob{{0x01U}}};
   const auto text = forge::variant{"value"};
   const auto number = forge::variant{1};

   BOOST_CHECK_NO_THROW(static_cast<void>(object == text));
   BOOST_CHECK_NO_THROW(static_cast<void>(text == object));
   BOOST_CHECK_NO_THROW(static_cast<void>(array == number));
   BOOST_CHECK_NO_THROW(static_cast<void>(number == array));
   BOOST_CHECK_NO_THROW(static_cast<void>(blob_value == text));
   BOOST_CHECK_NO_THROW(static_cast<void>(text == blob_value));
   BOOST_CHECK(object != text);
   BOOST_CHECK(text != object);
   BOOST_CHECK(array != number);
   BOOST_CHECK(number != array);
   BOOST_CHECK(blob_value != text);
   BOOST_CHECK(text != blob_value);
}

BOOST_AUTO_TEST_CASE(boost_describe_variant_missing_fields_keep_defaults_and_unknown_fields_are_ignored) {
   const forge::variant input = forge::mutable_variant_object()("name", "override")("unknown", "ignored");

   described_variant_config parsed;
   forge::from_variant(input, parsed);

   BOOST_CHECK_EQUAL(parsed.name, "override");
   BOOST_CHECK_EQUAL(parsed.limit, 7u);
   BOOST_CHECK(parsed.access == described_access::read);
}

BOOST_AUTO_TEST_CASE(boost_describe_variant_bad_enum_value_fails) {
   const forge::variant input = forge::mutable_variant_object()("name", "bad")("access", "delete");

   described_variant_config parsed;
   BOOST_CHECK_THROW(forge::from_variant(input, parsed), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(std_flat_map_variant_uses_sorted_array_of_pairs) {
   auto value = std::flat_map<std::uint32_t, std::string>{};
   value.emplace(2U, "second");
   value.emplace(1U, "first");

   const auto encoded = forge::variant{value};
   const auto& items = encoded.get_array();
   BOOST_REQUIRE_EQUAL(items.size(), 2U);
   BOOST_CHECK_EQUAL(items[0].get_array()[0].as_uint64(), 1U);
   BOOST_CHECK_EQUAL(items[0].get_array()[1].as_string(), "first");
   BOOST_CHECK((encoded.as<std::flat_map<std::uint32_t, std::string>>() == value));
}

BOOST_AUTO_TEST_CASE(std_chrono_variant_iso_roundtrip) {
   const std::chrono::sys_time<std::chrono::microseconds> timestamp{std::chrono::seconds{1}};
   forge::variant encoded;
   forge::to_variant(timestamp, encoded);
   BOOST_CHECK_EQUAL(encoded.as_string(), "1970-01-01T00:00:01.000");

   std::chrono::sys_time<std::chrono::microseconds> decoded;
   forge::from_variant(encoded, decoded);
   BOOST_CHECK(decoded == timestamp);

   forge::variant seconds_encoded;
   forge::to_variant(std::chrono::sys_seconds{std::chrono::seconds{1}}, seconds_encoded);
   BOOST_CHECK_EQUAL(seconds_encoded.as_string(), "1970-01-01T00:00:01");

   std::chrono::microseconds micros;
   forge::from_variant(forge::variant(int64_t{-1}), micros);
   BOOST_CHECK(micros == std::chrono::microseconds{-1});
}

BOOST_AUTO_TEST_CASE(mutable_variant_object_test) {
   // no BOOST_CHECK / BOOST_REQUIRE, just see that this compiles on all supported platforms
   try {
      variant v(42);
      variant_object vo;
      mutable_variant_object mvo;
      variants vs;
      vs.push_back(mutable_variant_object("level", "debug")("color", v));
      vs.push_back(mutable_variant_object()("level", "debug")("color", v));
      vs.push_back(mutable_variant_object("level", "debug")("color", "green"));
      vs.push_back(mutable_variant_object()("level", "debug")("color", "green"));
      vs.push_back(mutable_variant_object("level", "debug")(vo));
      vs.push_back(mutable_variant_object()("level", "debug")(mvo));
      vs.push_back(mutable_variant_object("level", "debug").set("color", v));
      vs.push_back(mutable_variant_object()("level", "debug").set("color", v));
      vs.push_back(mutable_variant_object("level", "debug").set("color", "green"));
      vs.push_back(mutable_variant_object()("level", "debug").set("color", "green"));
   }
   FORGE_LOG_AND_RETHROW();
}

BOOST_AUTO_TEST_CASE(variant_format_string_limited) {
   constexpr size_t long_rep_char_num = 1024;
   const std::string a_long_list = std::string(long_rep_char_num, 'a');
   const std::string b_long_list = std::string(long_rep_char_num, 'b');
   {
      const string format = "${a} ${b} ${c}";
      forge::mutable_variant_object mu;
      mu("a", string(long_rep_char_num, 'a'));
      mu("b", string(long_rep_char_num, 'b'));
      mu("c", string(long_rep_char_num, 'c'));
      const string result = forge::format_string(format, mu, true);
      BOOST_CHECK_LT(0u, mu.size());
      const auto arg_limit_size = (1024 - format.size()) / mu.size();
      BOOST_CHECK_EQUAL(result, string(arg_limit_size, 'a') + "... " + string(arg_limit_size, 'b') + "... " +
                                    string(arg_limit_size, 'c') + "...");
      BOOST_CHECK_LT(result.size(), 1024 + 3 * mu.size());
   }
   { // verify object, array, blob, and string, all exceeds limits, fold display for each
      forge::mutable_variant_object mu;
      mu("str", a_long_list);
      mu("obj", variant_object(mutable_variant_object()("a", a_long_list)("b", b_long_list)));
      mu("arr", variants{variant(a_long_list), variant(b_long_list)});
      mu("blob", blob({std::vector<std::uint8_t>(a_long_list.begin(), a_long_list.end())}));
      const string format_prefix = "Format string test: ";
      const string format_str = format_prefix + "${str} ${obj} ${arr} {blob}";
      const string result = forge::format_string(format_str, mu, true);
      BOOST_CHECK_LT(0u, mu.size());
      const auto arg_limit_size = (1024 - format_str.size()) / mu.size();
      BOOST_CHECK_EQUAL(result,
                        format_prefix + a_long_list.substr(0, arg_limit_size) + "..." + " ${obj} ${arr} {blob}");
      BOOST_CHECK_LT(result.size(), 1024 + 3 * mu.size());
   }
   { // verify object, array can be displayed properly
      const string format_prefix = "Format string test: ";
      const string format_str = format_prefix + "${str} ${obj} ${arr} ${blob} ${var}";
      BOOST_CHECK_LT(format_str.size(), 1024u);
      const size_t short_rep_char_num = (1024 - format_str.size()) / 5 - 1;
      const std::string a_short_list = std::string(short_rep_char_num, 'a');
      const std::string b_short_list = std::string(short_rep_char_num / 3, 'b');
      const std::string c_short_list = std::string(short_rep_char_num / 3, 'c');
      const std::string d_short_list = std::string(short_rep_char_num / 3, 'd');
      const std::string e_short_list = std::string(short_rep_char_num / 3, 'e');
      const std::string f_short_list = std::string(short_rep_char_num, 'f');
      const std::string g_short_list = std::string(short_rep_char_num, 'g');
      forge::mutable_variant_object mu;
      const variant_object vo(mutable_variant_object()("b", b_short_list)("c", c_short_list));
      const variants variant_list{variant(d_short_list), variant(e_short_list)};
      const blob a_blob({std::vector<std::uint8_t>(f_short_list.begin(), f_short_list.end())});
      const variant a_variant(g_short_list);
      mu("str", a_short_list);
      mu("obj", vo);
      mu("arr", variant_list);
      mu("blob", a_blob);
      mu("var", a_variant);
      const string result = forge::format_string(format_str, mu, true);
      const string target_result = format_prefix + a_short_list + " " + "{" + "\"b\":\"" + b_short_list +
                                   "\",\"c\":\"" + c_short_list + "\"}" + " " + "[\"" + d_short_list + "\",\"" +
                                   e_short_list + "\"]" + " " + forge::codec::base64::encode(a_blob.data) + " " +
                                   g_short_list;

      BOOST_CHECK_EQUAL(result, target_result);
      BOOST_CHECK_LT(result.size(), 1024 + 3 * mu.size());
   }
}

BOOST_AUTO_TEST_CASE(variant_blob) {
   // Some test cases from https://github.com/ReneNyffenegger/cpp-base64
   {
      std::string a17_orig = "aaaaaaaaaaaaaaaaa";
      std::string a17_encoded = "YWFhYWFhYWFhYWFhYWFhYWE=";
      forge::mutable_variant_object mu;
      mu("blob", blob{{a17_orig.begin(), a17_orig.end()}});
      mu("str", a17_encoded);

      BOOST_CHECK_EQUAL(mu["blob"].as_string(), a17_encoded);
      const auto b64 = mu["str"].as_blob().data;
      std::string_view b64_str(reinterpret_cast<const char*>(b64.data()), b64.size());
      BOOST_CHECK_EQUAL(b64_str, a17_orig);
   }
   {
      std::string s_6364 = "\x03"
                           "\xef"
                           "\xff"
                           "\xf9";
      std::string s_6364_encoded = "A+//+Q==";
      forge::mutable_variant_object mu;
      mu("blob", blob{{s_6364.begin(), s_6364.end()}});
      mu("str", s_6364_encoded);

      BOOST_CHECK_EQUAL(mu["blob"].as_string(), s_6364_encoded);
      const auto b64 = mu["str"].as_blob().data;
      std::string_view b64_str(reinterpret_cast<const char*>(b64.data()), b64.size());
      BOOST_CHECK_EQUAL(b64_str, s_6364);
   }
   {
      std::string org = "abc";
      std::string encoded = "YWJj";

      forge::mutable_variant_object mu;
      mu("blob", blob{{org.begin(), org.end()}});
      mu("str", encoded);

      BOOST_CHECK_EQUAL(mu["blob"].as_string(), encoded);
      const auto b64 = mu["str"].as_blob().data;
      std::string_view b64_str(reinterpret_cast<const char*>(b64.data()), b64.size());
      BOOST_CHECK_EQUAL(b64_str, org);
   }
}

BOOST_AUTO_TEST_CASE(variant_blob_backwards_compatibility) {
   // pre-5.0 variant would add an additional `=` as a flag that the blob data was base64 encoded
   // verify variant can process encoded data with the extra `=`
   {
      std::string a17_orig = "aaaaaaaaaaaaaaaaa";
      std::string a17_encoded = "YWFhYWFhYWFhYWFhYWFhYWE=";
      std::string a17_encoded_old = a17_encoded + '=';
      forge::mutable_variant_object mu;
      mu("blob", blob{{a17_orig.begin(), a17_orig.end()}});
      mu("str", a17_encoded_old);

      BOOST_CHECK_EQUAL(mu["blob"].as_string(), a17_encoded);
      const auto b64 = mu["str"].as_blob().data;
      std::string_view b64_str(reinterpret_cast<const char*>(b64.data()), b64.size());
      BOOST_CHECK_EQUAL(b64_str, a17_orig);
   }
   {
      std::string org = "abc";
      std::string encoded = "YWJj";
      std::string encoded_old = encoded + '=';

      forge::mutable_variant_object mu;
      mu("blob", blob{{org.begin(), org.end()}});
      mu("str", encoded_old);

      BOOST_CHECK_EQUAL(mu["blob"].as_string(), encoded);
      const auto b64 = mu["str"].as_blob().data;
      std::string_view b64_str(reinterpret_cast<const char*>(b64.data()), b64.size());
      BOOST_CHECK_EQUAL(b64_str, org);
   }
}

BOOST_AUTO_TEST_CASE(variant_blob_from_variant_preserves_extra_padding_compatibility) {
   const auto check_legacy_value = [](std::string_view encoded, std::string_view expected) {
      auto decoded = blob{};
      forge::from_variant(forge::variant{std::string{encoded}}, decoded);
      BOOST_TEST(std::string(decoded.data.begin(), decoded.data.end()) == expected);
   };

   check_legacy_value("YWFhYWFhYWFhYWFhYWFhYWE==", "aaaaaaaaaaaaaaaaa");
   check_legacy_value("YWJj=", "abc");
}

BOOST_AUTO_TEST_CASE(variant_blob_rejects_malformed_base64_trailing_garbage) {
   const auto malformed = std::string{"YQ==evil"};
   const auto value = forge::variant{malformed};

   const auto blob_value = value.as_blob().data;
   BOOST_TEST(std::string(blob_value.begin(), blob_value.end()) == malformed);

   auto decoded = forge::blob{};
   BOOST_CHECK_THROW(forge::from_variant(value, decoded), forge::codec::base64::exceptions::invalid_input);
}

BOOST_AUTO_TEST_CASE(array) {
   // check that variant arrays can be created or updated from:
   //  - `std::initializer_list`
   //  - `std::vector`
   //  - `std::array`
   // --------------------------------------------------------------------------------------------
   auto check_variant_round_trip = []<class C>(const C& arr) {
      forge::variant m(arr);

      std::vector<int> v;
      forge::from_variant(m, v);
      auto expected = std::vector<int>{arr.begin(), arr.end()};
      BOOST_TEST(v == expected);
   };

   check_variant_round_trip(std::initializer_list<int>{});
   check_variant_round_trip(std::initializer_list<int>{1, 2, 3});

   check_variant_round_trip(std::vector<int>{});
   check_variant_round_trip(std::vector<int>{1, 2, 3});

   check_variant_round_trip(std::array<int, 0>{});
   check_variant_round_trip(std::array{1, 2, 3});

   // check that mutable_variant_object arrays can be created or updated from:
   //  - `std::initializer_list`
   //  - `std::vector`
   //  - `std::array`
   // --------------------------------------------------------------------------------------------
   auto check_variant_round_trip2 = []<class C>(const C& arr) {
      forge::mutable_variant_object mu("a", arr);

      std::vector<int> v;
      forge::from_variant(mu["a"], v);
      auto expected = std::vector<int>{arr.begin(), arr.end()};
      BOOST_TEST(v == expected);

      auto mu2 = forge::mutable_variant_object()("b", arr); // also test mutable_variant_object operator()
      forge::from_variant(mu2["b"], v);
      BOOST_TEST(v == expected);
   };

   check_variant_round_trip2(std::initializer_list<int>{});
   check_variant_round_trip2(std::initializer_list<int>{1, 2, 3});

   check_variant_round_trip2(std::vector<int>{});
   check_variant_round_trip2(std::vector<int>{1, 2, 3});

   check_variant_round_trip2(std::array<int, 0>{});
   check_variant_round_trip2(std::array{1, 2, 3});
}

BOOST_AUTO_TEST_CASE(unsigned_byte_vectors_use_the_byte_array_limit) {
   constexpr auto general_container_limit = std::size_t{1024U * 1024U};
   const auto bytes = std::vector<std::uint8_t>(general_container_limit + 1U, 0xa5U);

   auto encoded = forge::variant{};
   BOOST_CHECK_NO_THROW(forge::to_variant(bytes, encoded));

   auto decoded = std::vector<std::uint8_t>{};
   BOOST_CHECK_NO_THROW(forge::from_variant(encoded, decoded));
   BOOST_TEST(decoded == bytes, boost::test_tools::per_element());

   const auto ordinary = std::vector<std::uint16_t>(general_container_limit + 1U, 7U);
   BOOST_CHECK_THROW(forge::to_variant(ordinary, encoded), std::range_error);
}

BOOST_AUTO_TEST_SUITE_END()
