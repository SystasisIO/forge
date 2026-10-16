#include <boost/test/unit_test.hpp>

#include <string>

import forge.chain.api.abi;
import forge.codec.json;
import forge.raw.raw;

namespace {

forge::chain::protocol::abi_def document_abi()
{
   auto abi = forge::chain::protocol::abi_def{};
   abi.version = "eosio::abi/1.2";
   abi.types.push_back({"json_document", "string"});
   abi.structs.push_back({"tool", {}, {{"parameters", "json_document"}, {"choice", "json_document?"}}});
   return abi;
}

forge::chain::protocol::abi_metadata document_metadata()
{
   const auto parsed = forge::codec::json::read<forge::chain::protocol::abi_metadata>(
      R"({"version":"forge::abi-metadata/1.1","roots":[],"enums":[],"json_types":["json_document"]})");
   BOOST_REQUIRE(parsed.ok());
   return parsed.value;
}

}

BOOST_AUTO_TEST_CASE(abi_json_document_marker_roundtrips_nested_json_and_raw_string)
{
   const auto abi = document_abi();
   const auto metadata = document_metadata();
   const auto json = forge::codec::json::read_value(R"({"type":"object","required":["city"],"additionalProperties":false})");
   BOOST_REQUIRE(json.ok());
   const auto bytes = forge::chain::api::abi_json_to_bin(abi, metadata, "json_document", json.value);
   const auto restored = forge::chain::api::abi_bin_to_json(abi, metadata, "json_document", bytes);
   BOOST_CHECK(restored == json.value);
   BOOST_CHECK(forge::raw::unpack_exact<forge::codec::json::document>(bytes).text ==
               forge::codec::json::write_value(json.value).text);
   const auto nested = forge::codec::json::read_value(R"({"parameters":{"x":[null,true,1]},"choice":"auto"})");
   BOOST_REQUIRE(nested.ok());
   const auto packed = forge::chain::api::abi_json_to_bin(abi, metadata, "tool", nested.value);
   BOOST_CHECK(forge::chain::api::abi_bin_to_json(abi, metadata, "tool", packed) == nested.value);
}

BOOST_AUTO_TEST_CASE(abi_unmarked_string_alias_remains_a_string)
{
   const auto abi = document_abi();
   const auto text = std::string{R"({"x":1})"};
   const auto bytes = forge::raw::pack(text);
   BOOST_CHECK(forge::chain::api::abi_json_to_bin(abi, "json_document", forge::variant{text}) == bytes);
   BOOST_CHECK(forge::chain::api::abi_bin_to_json(abi, "json_document", bytes) == forge::variant{text});
   BOOST_CHECK(forge::chain::api::abi_json_to_bin(abi, forge::chain::protocol::abi_metadata{},
                                               "json_document", forge::variant{text}) == bytes);
}

BOOST_AUTO_TEST_CASE(abi_json_document_rejects_invalid_wire_depth_bytes_and_marker)
{
   const auto abi = document_abi();
   const auto metadata = document_metadata();
   BOOST_CHECK_THROW(forge::chain::api::abi_bin_to_json(abi, metadata, "json_document", forge::raw::pack(std::string{"{"})),
                     forge::chain::api::abi_serialization_error);
   BOOST_CHECK_THROW(forge::chain::api::abi_bin_to_json(abi, metadata, "json_document", forge::raw::pack(std::string{"[[[0]]]"}),
                                                      {.max_recursion_depth = 2}),
                     forge::chain::api::abi_serialization_error);
   BOOST_CHECK_THROW(forge::chain::api::abi_bin_to_json(abi, metadata, "json_document", forge::raw::pack(std::string{"[123]"}),
                                                      {.max_string_bytes = 4}),
                     forge::chain::api::abi_serialization_error);
   auto invalid = abi;
   invalid.types.front().type = "uint32";
   BOOST_CHECK_THROW(forge::chain::api::abi_bin_to_json(invalid, metadata, "json_document", forge::raw::pack(1U)),
                     forge::chain::api::abi_serialization_error);
}

BOOST_AUTO_TEST_CASE(abi_json_document_rejects_trailing_wire_content)
{
   const auto abi = document_abi();
   const auto metadata = document_metadata();
   for (const auto& input : {std::string{"null garbage"}, std::string{"{}{}"}, std::string{"null\0garbage", 12}})
   {
      BOOST_CHECK_THROW(static_cast<void>(forge::chain::api::abi_bin_to_json(
         abi, metadata, "json_document", forge::raw::pack(input))), forge::chain::api::abi_serialization_error);
   }
}
