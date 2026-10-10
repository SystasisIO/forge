#include <boost/test/unit_test.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

import forge.chain.api.abi;
import forge.codec.json;
import forge.crypto.asymmetric;
import forge.raw.raw;

namespace {

namespace chain_api = forge::chain::api;
namespace protocol = forge::chain::protocol;
namespace asymmetric = forge::crypto::asymmetric;

// Pinned donor text remains an oracle, not an accepted ABI input format.
constexpr auto donor_public_key = std::string_view{"EOS6MRyAjQq8ud7hVNYcfnVPJqcVpscN5So8BhtHuGYqET5GDW5CV"};
constexpr auto donor_signature = std::string_view{
    "SIG_K1_Jzdpi5RCzHLGsQbpGhndXBzcFs8vT5LHAtWLMxPzBdwRHSmJkcCdVu6oqPUQn1hbGUdErHvxtdSTS1YA73BThQFwV1v4G5"};

template <typename T, std::size_t Size> std::array<T, Size> sequence(std::uint8_t first) {
   auto result = std::array<T, Size>{};
   for (auto index = std::size_t{}; index < result.size(); ++index) {
      result[index] = static_cast<T>(first + index);
   }
   return result;
}

forge::variant object(std::initializer_list<std::pair<std::string, forge::variant>> fields) {
   auto value = forge::mutable_variant_object{};
   value.reserve(fields.size());
   for (const auto& [name, field] : fields) {
      value.set(name, field);
   }
   return forge::variant{std::move(value)};
}

template <typename... Values> forge::variant array(Values&&... values) {
   auto result = forge::variants{};
   result.reserve(sizeof...(Values));
   (result.emplace_back(std::forward<Values>(values)), ...);
   return forge::variant{std::move(result)};
}

protocol::abi_def empty_abi(std::string version = "eosio::abi/1.2") {
   auto abi = protocol::abi_def{};
   abi.version = std::move(version);
   return abi;
}

protocol::abi_def spring_shape_abi() {
   auto abi = empty_abi();
   abi.types = {
       protocol::type_def{.new_type_name = "small", .type = "uint8"},
   };
   abi.structs = {
       protocol::struct_def{
           .name = "base",
           .fields =
               {
                   protocol::field_def{.name = "base_value", .type = "uint16"},
               },
       },
       protocol::struct_def{
           .name = "record",
           .base = "base",
           .fields =
               {
                   protocol::field_def{.name = "fixed", .type = "small[3]"},
                   protocol::field_def{.name = "maybe", .type = "uint32?"},
                   protocol::field_def{.name = "tail", .type = "string$"},
               },
       },
       protocol::struct_def{
           .name = "extension_record",
           .fields =
               {
                   protocol::field_def{.name = "i0", .type = "int8"},
                   protocol::field_def{.name = "i1", .type = "int8"},
                   protocol::field_def{.name = "i2", .type = "int8$"},
                   protocol::field_def{.name = "a", .type = "int8[]$"},
                   protocol::field_def{.name = "o", .type = "int8?$"},
                   protocol::field_def{.name = "fa", .type = "int8[2]$"},
               },
       },
   };
   abi.variants.value = {
       protocol::variant_def{.name = "v1", .types = {"int8", "string", "int16"}},
   };
   return abi;
}

void require_diagnostic(const chain_api::abi_serialization_error& error, chain_api::abi_error_code code,
                        std::string_view type, std::string_view path, std::size_t offset) {
   const auto& diagnostic = error.diagnostic();
   BOOST_TEST(static_cast<int>(diagnostic.code) == static_cast<int>(code));
   BOOST_TEST(diagnostic.type == type);
   BOOST_TEST(diagnostic.path == path);
   BOOST_TEST(diagnostic.offset == offset);
   BOOST_TEST(!diagnostic.message.empty());
}

forge::variant recursive_node(std::size_t depth) {
   auto next = forge::variant{};
   for (auto index = depth; index > 0U; --index) {
      next = object({
          {"value", forge::variant{static_cast<std::uint64_t>(index)}},
          {"next", std::move(next)},
      });
   }
   return next;
}

} // namespace

BOOST_AUTO_TEST_CASE(chain_abi_spring_variant_goldens) {
   const auto abi = spring_shape_abi();

   const auto int8_binary = chain_api::abi_json_to_bin(abi, "v1", array("int8", 21));
   BOOST_TEST(int8_binary == protocol::bytes({0x00, 0x15}));
   BOOST_CHECK(chain_api::abi_bin_to_json(abi, "v1", int8_binary) == array("int8", 21));

   const auto string_binary = chain_api::abi_json_to_bin(abi, "v1", array("string", "abcd"));
   BOOST_TEST(string_binary == protocol::bytes({0x01, 0x04, 0x61, 0x62, 0x63, 0x64}));
   BOOST_CHECK(chain_api::abi_bin_to_json(abi, "v1", string_binary) == array("string", "abcd"));

   const auto int16_binary = chain_api::abi_json_to_bin(abi, "v1", array("int16", 3));
   BOOST_TEST(int16_binary == protocol::bytes({0x02, 0x03, 0x00}));
}

BOOST_AUTO_TEST_CASE(chain_abi_renders_spring_transaction_actions_with_data_and_hex_fallback) {
   auto abi = empty_abi();
   abi.structs = {
       protocol::struct_def{
           .name = "ping",
           .fields = {protocol::field_def{.name = "value", .type = "uint32"}},
       },
   };
   abi.actions = {
       protocol::action_def{.name = protocol::action_name{"ping"}, .type = "ping"},
   };

   auto action = protocol::action{};
   action.account = protocol::account_name{"tester"};
   action.name = protocol::action_name{"ping"};
   action.authorization = {
       protocol::permission_level{.actor = protocol::account_name{"tester"},
                                  .permission = protocol::permission_name{"active"}},
   };
   action.data = chain_api::abi_json_to_bin(abi, "ping", object({{"value", 7}}));

   auto transaction = protocol::transaction{};
   transaction.actions.push_back(action);
   const auto resolver = [abi](protocol::account_name account) -> std::optional<protocol::abi_def> {
      return account == protocol::account_name{"tester"} ? std::optional{abi} : std::nullopt;
   };

   const auto rendered = chain_api::transaction_to_variant(transaction, resolver);
   const auto& rendered_action = rendered["actions"][std::size_t{0U}];
   BOOST_CHECK(rendered_action["data"] == object({{"value", 7}}));
   BOOST_TEST(rendered_action["hex_data"].as_string() == "07000000");

   const auto raw =
       chain_api::action_to_variant(action, [](protocol::account_name) { return std::optional<protocol::abi_def>{}; });
   BOOST_TEST(raw["data"].as_string() == "07000000");
   BOOST_TEST(raw["hex_data"].as_string() == "07000000");

   transaction.transaction_extensions.emplace_back(9U, protocol::bytes{});
   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::transaction_to_variant(transaction, resolver)),
                         chain_api::abi_serialization_error, [](const auto& error) {
                            return error.diagnostic().code == chain_api::abi_error_code::invalid_binary &&
                                   error.diagnostic().path == "transaction_extensions";
                         });
}

BOOST_AUTO_TEST_CASE(chain_abi_translates_resolver_failures_to_typed_diagnostics) {
   auto action = protocol::action{};
   action.account = protocol::account_name{"tester"};
   action.name = protocol::action_name{"ping"};

   BOOST_CHECK_EXCEPTION(
       static_cast<void>(chain_api::action_to_variant(action,
                                                      [](protocol::account_name) -> std::optional<protocol::abi_def> {
                                                         throw std::runtime_error{"resolver unavailable"};
                                                      })),
       chain_api::abi_serialization_error, [](const auto& error) {
          return error.diagnostic().code == chain_api::abi_error_code::invalid_abi &&
                 error.diagnostic().path == "tester" &&
                 error.diagnostic().message.find("resolver unavailable") != std::string::npos;
       });
}

BOOST_AUTO_TEST_CASE(chain_abi_rejects_oversized_action_authorization_before_resolver) {
   auto action = protocol::action{};
   action.authorization.resize(2U);
   action.data = {0x00, 0x01};

   auto limits = chain_api::abi_serialization_limits{};
   limits.max_container_elements = 1U;
   limits.max_binary_bytes = 1U;
   auto resolver_calls = std::size_t{};
   const auto resolver = [&resolver_calls](protocol::account_name) {
      ++resolver_calls;
      return std::optional<protocol::abi_def>{};
   };

   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::action_to_variant(action, resolver, limits)),
                         chain_api::abi_serialization_error, [](const auto& error) {
                            return error.diagnostic().code == chain_api::abi_error_code::size_limit &&
                                   error.diagnostic().type == "action" && error.diagnostic().path == "authorization";
                         });
   BOOST_TEST(resolver_calls == 0U);
}

BOOST_AUTO_TEST_CASE(chain_abi_transaction_preflights_each_action_authorization_before_resolver) {
   for (const auto context_free : {true, false}) {
      auto action = protocol::action{};
      action.authorization.resize(3U);
      auto transaction = protocol::transaction{};
      if (context_free) {
         transaction.context_free_actions.emplace_back();
         transaction.context_free_actions.push_back(std::move(action));
      } else {
         transaction.actions.emplace_back();
         transaction.actions.push_back(std::move(action));
      }

      auto limits = chain_api::abi_serialization_limits{};
      limits.max_container_elements = 2U;
      auto resolver_calls = std::size_t{};
      const auto resolver = [&resolver_calls](protocol::account_name) {
         ++resolver_calls;
         return std::optional<protocol::abi_def>{};
      };

      const auto* context = context_free ? "context-free action" : "ordinary action";
      BOOST_TEST_CONTEXT(context) {
         BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::transaction_to_variant(transaction, resolver, limits)),
                               chain_api::abi_serialization_error, [](const auto& error) {
                                  return error.diagnostic().code == chain_api::abi_error_code::size_limit &&
                                         error.diagnostic().type == "action" &&
                                         error.diagnostic().path == "authorization";
                               });
         BOOST_TEST(resolver_calls == 0U);
      }
   }
}

BOOST_AUTO_TEST_CASE(chain_abi_rejects_oversized_action_data_before_no_abi_fallback) {
   auto action = protocol::action{};
   action.data = {0x00, 0x01};

   auto limits = chain_api::abi_serialization_limits{};
   limits.max_binary_bytes = 1U;
   auto resolver_calls = std::size_t{};
   const auto resolver = [&resolver_calls](protocol::account_name) {
      ++resolver_calls;
      return std::optional<protocol::abi_def>{};
   };

   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::action_to_variant(action, resolver, limits)),
                         chain_api::abi_serialization_error, [](const auto& error) {
                            return error.diagnostic().code == chain_api::abi_error_code::size_limit &&
                                   error.diagnostic().type == "action" && error.diagnostic().path == "data";
                         });
   BOOST_TEST(resolver_calls == 0U);
}

BOOST_AUTO_TEST_CASE(chain_abi_transaction_rejects_oversized_action_data_before_matching_resolver) {
   auto abi = empty_abi();
   abi.actions = {
       protocol::action_def{.name = protocol::action_name{"payload"}, .type = "bytes"},
   };

   auto action = protocol::action{};
   action.account = protocol::account_name{"tester"};
   action.name = protocol::action_name{"payload"};
   action.data = {0x00, 0x01};
   auto transaction = protocol::transaction{};
   transaction.actions.push_back(action);

   auto limits = chain_api::abi_serialization_limits{};
   limits.max_binary_bytes = 1U;
   auto resolver_calls = std::size_t{};
   const auto resolver = [&abi, &resolver_calls](protocol::account_name account) -> std::optional<protocol::abi_def> {
      ++resolver_calls;
      return account == protocol::account_name{"tester"} ? std::optional{abi} : std::nullopt;
   };

   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::transaction_to_variant(transaction, resolver, limits)),
                         chain_api::abi_serialization_error, [](const auto& error) {
                            return error.diagnostic().code == chain_api::abi_error_code::size_limit &&
                                   error.diagnostic().type == "action" && error.diagnostic().path == "data";
                         });
   BOOST_TEST(resolver_calls == 0U);
}

BOOST_AUTO_TEST_CASE(chain_abi_does_not_fallback_on_matching_abi_size_limit) {
   auto abi = empty_abi();
   abi.actions = {
       protocol::action_def{.name = protocol::action_name{"payload"}, .type = "bytes"},
   };

   auto action = protocol::action{};
   action.account = protocol::account_name{"tester"};
   action.name = protocol::action_name{"payload"};
   action.data = {0x02, 0xaa, 0xbb};

   auto limits = chain_api::abi_serialization_limits{};
   limits.max_binary_bytes = action.data.size();
   limits.max_string_bytes = 1U;
   auto resolver_calls = std::size_t{};
   const auto resolver = [&abi, &resolver_calls](protocol::account_name account) -> std::optional<protocol::abi_def> {
      ++resolver_calls;
      return account == protocol::account_name{"tester"} ? std::optional{abi} : std::nullopt;
   };

   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::action_to_variant(action, resolver, limits)),
                         chain_api::abi_serialization_error, [](const auto& error) {
                            return error.diagnostic().code == chain_api::abi_error_code::size_limit &&
                                   error.diagnostic().type == "bytes" && error.diagnostic().path == "bytes";
                         });
   BOOST_TEST(resolver_calls == 1U);
}

BOOST_AUTO_TEST_CASE(chain_abi_preserves_hex_fallback_for_matching_abi_decode_incompatibility) {
   auto abi = empty_abi();
   abi.actions = {
       protocol::action_def{.name = protocol::action_name{"payload"}, .type = "uint32"},
   };

   auto action = protocol::action{};
   action.account = protocol::account_name{"tester"};
   action.name = protocol::action_name{"payload"};
   action.data = {0x07};
   const auto resolver = [&abi](protocol::account_name account) -> std::optional<protocol::abi_def> {
      return account == protocol::account_name{"tester"} ? std::optional{abi} : std::nullopt;
   };

   const auto rendered = chain_api::action_to_variant(action, resolver);
   BOOST_TEST(rendered["data"].as_string() == "07");
   BOOST_TEST(rendered["hex_data"].as_string() == "07");
}

BOOST_AUTO_TEST_CASE(chain_abi_rejects_oversized_context_free_actions_before_resolver) {
   auto transaction = protocol::transaction{};
   transaction.context_free_actions.resize(2U);

   auto limits = chain_api::abi_serialization_limits{};
   limits.max_container_elements = 1U;
   auto resolver_calls = std::size_t{};
   const auto resolver = [&resolver_calls](protocol::account_name) {
      ++resolver_calls;
      return std::optional<protocol::abi_def>{};
   };

   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::transaction_to_variant(transaction, resolver, limits)),
                         chain_api::abi_serialization_error, [](const auto& error) {
                            return error.diagnostic().code == chain_api::abi_error_code::size_limit &&
                                   error.diagnostic().path == "context_free_actions";
                         });
   BOOST_TEST(resolver_calls == 0U);
}

BOOST_AUTO_TEST_CASE(chain_abi_rejects_oversized_actions_before_resolver) {
   auto transaction = protocol::transaction{};
   transaction.context_free_actions.resize(1U);
   transaction.actions.resize(2U);

   auto limits = chain_api::abi_serialization_limits{};
   limits.max_container_elements = 1U;
   auto resolver_calls = std::size_t{};
   const auto resolver = [&resolver_calls](protocol::account_name) {
      ++resolver_calls;
      return std::optional<protocol::abi_def>{};
   };

   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::transaction_to_variant(transaction, resolver, limits)),
                         chain_api::abi_serialization_error, [](const auto& error) {
                            return error.diagnostic().code == chain_api::abi_error_code::size_limit &&
                                   error.diagnostic().path == "actions";
                         });
   BOOST_TEST(resolver_calls == 0U);
}

BOOST_AUTO_TEST_CASE(chain_abi_spring_binary_extension_goldens) {
   const auto abi = spring_shape_abi();

   const auto prefix = object({{"i0", 5}, {"i1", 6}});
   BOOST_TEST(chain_api::abi_json_to_bin(abi, "extension_record", prefix) == protocol::bytes({0x05, 0x06}));

   const auto with_array = object({
       {"i0", 5},
       {"i1", 6},
       {"i2", 7},
       {"a", array(8, 9, 10)},
   });
   const auto array_binary = chain_api::abi_json_to_bin(abi, "extension_record", with_array);
   BOOST_TEST(array_binary == protocol::bytes({0x05, 0x06, 0x07, 0x03, 0x08, 0x09, 0x0a}));

   const auto complete = object({
       {"i0", 5},
       {"i1", 6},
       {"i2", 7},
       {"a", array(8, 9, 10)},
       {"o", 31},
       {"fa", array(1, 2)},
   });
   const auto complete_binary = chain_api::abi_json_to_bin(abi, "extension_record", complete);
   BOOST_TEST(complete_binary == protocol::bytes({0x05, 0x06, 0x07, 0x03, 0x08, 0x09, 0x0a, 0x01, 0x1f, 0x01, 0x02}));
   BOOST_TEST(chain_api::abi_json_to_bin(abi, "extension_record",
                                         chain_api::abi_bin_to_json(abi, "extension_record", complete_binary)) ==
              complete_binary);
}

BOOST_AUTO_TEST_CASE(chain_abi_struct_inheritance_arrays_optional_and_aliases) {
   const auto abi = spring_shape_abi();
   const auto value = object({
       {"base_value", 0x1234},
       {"fixed", array(1, 2, 3)},
       {"maybe", 0x01020304},
       {"tail", "ok"},
   });

   const auto binary = chain_api::abi_json_to_bin(abi, "record", value);
   BOOST_TEST(binary ==
              protocol::bytes({0x34, 0x12, 0x01, 0x02, 0x03, 0x01, 0x04, 0x03, 0x02, 0x01, 0x02, 0x6f, 0x6b}));
   BOOST_TEST(chain_api::abi_json_to_bin(abi, "record", chain_api::abi_bin_to_json(abi, "record", binary)) == binary);

   const auto without_tail = object({
       {"base_value", 0x1234},
       {"fixed", array(1, 2, 3)},
       {"maybe", forge::variant{}},
   });
   const auto prefix = chain_api::abi_json_to_bin(abi, "record", without_tail);
   const auto decoded = chain_api::abi_bin_to_json(abi, "record", prefix);
   BOOST_TEST(decoded.get_object().contains("maybe"));
   BOOST_TEST(decoded["maybe"].is_null());
   BOOST_TEST(!decoded.get_object().contains("tail"));
}

BOOST_AUTO_TEST_CASE(chain_abi_donor_builtins_round_trip) {
   const auto abi = empty_abi();
   const auto zero160 = std::string(40, '0');
   const auto zero256 = std::string(64, '0');
   const auto zero512 = std::string(128, '0');
   const auto public_key = asymmetric::encoding::antelope().parse_public(donor_public_key);
   const auto signature = asymmetric::encoding::antelope().parse_signature(donor_signature);

   const auto values = std::vector<std::pair<std::string, forge::variant>>{
       {"bool", true},
       {"int8", -7},
       {"uint8", 7},
       {"int16", -300},
       {"uint16", 300},
       {"int32", -70'000},
       {"uint32", 70'000},
       {"int64", -9'000'000},
       {"uint64", 9'000'000},
       {"int128", -42},
       {"uint128", "42"},
       {"varint32", -300},
       {"varuint32", 300},
       {"float32", 1.5},
       {"float64", -3.25},
       {"float128", "0x000102030405060708090a0b0c0d0e0f"},
       {"time_point", "2000-01-01T00:00:00"},
       {"time_point_sec", "2000-01-01T00:00:00"},
       {"block_timestamp_type", "2000-01-01T00:00:00"},
       {"name", "alice"},
       {"bytes", "00a5ff"},
       {"string", "spring fixture"},
       {"checksum160", zero160},
       {"checksum256", zero256},
       {"checksum512", zero512},
       {"public_key", asymmetric::encoding::forge().format(public_key)},
       {"signature", asymmetric::encoding::forge().format(signature)},
       {"symbol", "4,SYS"},
       {"symbol_code", "SYS"},
       {"asset", "100.0000 SYS"},
       {"extended_asset", object({{"quantity", "1.0000 SYS"}, {"contract", "eosio.token"}})},
   };

   for (const auto& [type, value] : values) {
      const auto binary = chain_api::abi_json_to_bin(abi, type, value);
      const auto decoded = chain_api::abi_bin_to_json(abi, type, binary);
      BOOST_TEST_CONTEXT("built-in " << type) {
         BOOST_TEST(chain_api::abi_json_to_bin(abi, type, decoded) == binary);
      }
   }
}

BOOST_AUTO_TEST_CASE(chain_abi_crypto_uses_forge_text_and_preserves_raw_binary) {
   const auto abi = empty_abi();
   const auto& codec = asymmetric::encoding::forge();
   const auto public_keys = std::vector<std::pair<asymmetric::public_key, std::string_view>>{
       {asymmetric::encoding::antelope().parse_public(donor_public_key), "PUB_SECP256K1_"},
       {asymmetric::r1_public_key{sequence<char, 33>(2U)}, "PUB_P256_"},
       {asymmetric::webauthn_public_key{sequence<char, 33>(3U),
                                        asymmetric::webauthn_public_key::user_presence_t::USER_PRESENCE_VERIFIED,
                                        "login.example"},
        "PUB_WEBAUTHN_"},
       {asymmetric::ed25519_public_key{sequence<std::uint8_t, 32>(4U)}, "PUB_ED25519_"},
       {asymmetric::rsa_public_key{{5U, 6U, 7U, 8U}}, "PUB_RSA_"},
   };
   const auto signatures = std::vector<std::pair<asymmetric::signature, std::string_view>>{
       {asymmetric::encoding::antelope().parse_signature(donor_signature), "SIG_SECP256K1_"},
       {asymmetric::r1_signature{sequence<char, 65>(12U)}, "SIG_P256_"},
       {asymmetric::webauthn_signature{sequence<char, 65>(13U), {14U, 15U, 16U}, R"({"type":"webauthn.get"})"},
        "SIG_WEBAUTHN_"},
       {asymmetric::ed25519_signature{sequence<std::uint8_t, 64>(17U)}, "SIG_ED25519_"},
       {asymmetric::rsa_signature{{18U, 19U, 20U, 21U}}, "SIG_RSA_"},
   };

   for (const auto& [key, prefix] : public_keys) {
      BOOST_TEST_CONTEXT("public-key family " << prefix) {
         const auto text = codec.format(key);
         BOOST_CHECK(text.starts_with(prefix));
         const auto binary = chain_api::abi_json_to_bin(abi, "public_key", forge::variant{text});
         BOOST_TEST(binary == forge::raw::pack(key));
         const auto decoded = chain_api::abi_bin_to_json(abi, "public_key", binary);
         BOOST_TEST(decoded.as_string() == text);
         BOOST_CHECK(codec.parse_public(decoded.as_string()) == key);
      }
   }
   for (const auto& [signature, prefix] : signatures) {
      BOOST_TEST_CONTEXT("signature family " << prefix) {
         const auto text = codec.format(signature);
         BOOST_CHECK(text.starts_with(prefix));
         const auto binary = chain_api::abi_json_to_bin(abi, "signature", forge::variant{text});
         BOOST_TEST(binary == forge::raw::pack(signature));
         const auto decoded = chain_api::abi_bin_to_json(abi, "signature", binary);
         BOOST_TEST(decoded.as_string() == text);
         BOOST_CHECK(codec.parse_signature(decoded.as_string()) == signature);
      }
   }

   // Captured through the unchanged Antelope ABI boundary before this text break.
   const auto donor_public_binary = protocol::bytes{
       0x00, 0x02, 0xc0, 0xde, 0xd2, 0xbc, 0x1f, 0x13, 0x05, 0xfb, 0x0f, 0xaa, 0xc5, 0xe6, 0xc0, 0x3e, 0xe3,
       0xa1, 0x92, 0x42, 0x34, 0x98, 0x54, 0x27, 0xb6, 0x16, 0x7c, 0xa5, 0x69, 0xd1, 0x3d, 0xf4, 0x35, 0xcf};
   const auto donor_signature_binary = protocol::bytes{
       0x00, 0x1f, 0x29, 0x1f, 0x9b, 0x80, 0x64, 0x2f, 0x6c, 0xd8, 0xed, 0x1c, 0x78, 0x89, 0xa0, 0x67, 0xb6,
       0xfa, 0xda, 0x4e, 0xec, 0xf9, 0x71, 0xfe, 0xbf, 0x3f, 0x67, 0x1f, 0x97, 0xb7, 0x09, 0xb7, 0x17, 0xc4,
       0x58, 0xa2, 0x6e, 0xad, 0x9a, 0x25, 0x5c, 0x69, 0xe9, 0x55, 0x0b, 0x1e, 0x13, 0xe9, 0xac, 0x0c, 0x6d,
       0x9d, 0x1d, 0x90, 0x0c, 0xf9, 0x83, 0x1a, 0x65, 0x66, 0xc1, 0xf0, 0x88, 0x9f, 0x0f, 0xb0};
   BOOST_TEST(forge::raw::pack(public_keys.front().first) == donor_public_binary);
   BOOST_TEST(forge::raw::pack(signatures.front().first) == donor_signature_binary);
   BOOST_TEST(chain_api::abi_json_to_bin(abi, "public_key", forge::variant{codec.format(public_keys.front().first)}) ==
              donor_public_binary);
   BOOST_TEST(chain_api::abi_json_to_bin(abi, "signature", forge::variant{codec.format(signatures.front().first)}) ==
              donor_signature_binary);
}

BOOST_AUTO_TEST_CASE(chain_abi_crypto_nested_record_preserves_donor_bytes) {
   auto abi = empty_abi();
   abi.structs = {
       protocol::struct_def{.name = "crypto",
                            .fields = {{.name = "key", .type = "public_key"}, {.name = "proof", .type = "signature"}}},
       protocol::struct_def{.name = "record",
                            .fields = {{.name = "tag", .type = "uint16"}, {.name = "crypto", .type = "crypto"}}},
   };
   const auto key = asymmetric::encoding::antelope().parse_public(donor_public_key);
   const auto signature = asymmetric::encoding::antelope().parse_signature(donor_signature);
   const auto value = object({
       {"tag", 0x1234},
       {"crypto", object({{"key", asymmetric::encoding::forge().format(key)},
                          {"proof", asymmetric::encoding::forge().format(signature)}})},
   });
   const auto expected = forge::raw::pack(std::uint16_t{0x1234}, key, signature);
   const auto binary = chain_api::abi_json_to_bin(abi, "record", value);
   BOOST_TEST(binary == expected);
   BOOST_CHECK(chain_api::abi_bin_to_json(abi, "record", binary) == value);
}

BOOST_AUTO_TEST_CASE(chain_abi_rejects_antelope_crypto_text_with_exact_diagnostics) {
   auto abi = empty_abi();
   abi.structs = {
       protocol::struct_def{.name = "crypto",
                            .fields = {{.name = "key", .type = "public_key"}, {.name = "proof", .type = "signature"}}},
       protocol::struct_def{.name = "record",
                            .fields = {{.name = "tag", .type = "uint16"}, {.name = "crypto", .type = "crypto"}}},
   };
   const auto key = asymmetric::encoding::antelope().parse_public(donor_public_key);
   const auto signature = asymmetric::encoding::antelope().parse_signature(donor_signature);
   const auto& legacy = asymmetric::encoding::antelope();
   const auto cases = std::vector<std::pair<std::string_view, std::string>>{
       {"public_key", legacy.format(key)},
       {"public_key", legacy.format(asymmetric::public_key{asymmetric::r1_public_key{sequence<char, 33>(2U)}})},
       {"public_key", legacy.format(asymmetric::public_key{asymmetric::webauthn_public_key{
                          sequence<char, 33>(3U),
                          asymmetric::webauthn_public_key::user_presence_t::USER_PRESENCE_VERIFIED, "login.example"}})},
       {"signature", legacy.format(signature)},
       {"signature", legacy.format(asymmetric::signature{asymmetric::r1_signature{sequence<char, 65>(12U)}})},
       {"signature", legacy.format(asymmetric::signature{asymmetric::webauthn_signature{
                         sequence<char, 65>(13U), {14U, 15U, 16U}, R"({"type":"webauthn.get"})"}})},
       // Existing parse-only prefix shape; this is not a checksum-valid donor oracle.
       {"public_key", "PUB_K1_TEST"},
   };

   for (const auto& [type, text] : cases) {
      BOOST_TEST_CONTEXT("unsupported Antelope scalar " << type) {
         for (const auto nested : {false, true}) {
            const auto value =
                nested
                    ? object(
                          {{"tag", 0x1234},
                           {"crypto",
                            object({
                                {"key", type == "public_key" ? text : asymmetric::encoding::forge().format(key)},
                                {"proof", type == "signature" ? text : asymmetric::encoding::forge().format(signature)},
                            })}})
                    : forge::variant{text};
            const auto path = nested ? (type == "public_key" ? "record.crypto.key" : "record.crypto.proof") : type;
            const auto offset = !nested ? 0U : type == "public_key" ? 2U : 2U + forge::raw::pack(key).size();
            try {
               (void)chain_api::abi_json_to_bin(abi, nested ? "record" : type, value);
               BOOST_FAIL("Antelope crypto text was accepted by the Forge ABI boundary");
            } catch (const chain_api::abi_serialization_error& error) {
               require_diagnostic(error, chain_api::abi_error_code::invalid_json, type, path, offset);
               BOOST_TEST(error.code().value() == static_cast<int>(chain_api::abi_error_code::invalid_json));
               BOOST_TEST(std::string{error.code().category().name()} == "forge.chain.api.abi");
            }
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(chain_abi_reports_exact_missing_and_trailing_diagnostics) {
   auto abi = empty_abi();
   abi.structs = {
       protocol::struct_def{
           .name = "record",
           .fields =
               {
                   protocol::field_def{.name = "first", .type = "uint8"},
                   protocol::field_def{.name = "required", .type = "uint16"},
               },
       },
   };

   try {
      (void)chain_api::abi_json_to_bin(abi, "record", object({{"first", 7}}));
      BOOST_FAIL("missing ABI field was accepted");
   } catch (const chain_api::abi_serialization_error& error) {
      require_diagnostic(error, chain_api::abi_error_code::missing_field, "uint16", "record.required", 1U);
      BOOST_TEST(error.diagnostic().message == "Missing field in ABI JSON object");
   }

   try {
      (void)chain_api::abi_bin_to_json(abi, "uint8", protocol::bytes{0x07, 0x08});
      BOOST_FAIL("trailing ABI bytes were accepted");
   } catch (const chain_api::abi_serialization_error& error) {
      require_diagnostic(error, chain_api::abi_error_code::trailing_bytes, "uint8", "uint8", 1U);
      BOOST_TEST(error.diagnostic().message == "ABI binary contains trailing bytes");
   }

   try {
      (void)chain_api::abi_bin_to_json(abi, "string", protocol::bytes{0x03, 0x61});
      BOOST_FAIL("truncated ABI string was accepted");
   } catch (const chain_api::abi_serialization_error& error) {
      require_diagnostic(error, chain_api::abi_error_code::invalid_binary, "string", "string", 1U);
      BOOST_TEST(error.diagnostic().message == "ABI binary ended inside a string");
   }
}

BOOST_AUTO_TEST_CASE(chain_abi_enforces_recursion_deadline_and_size_limits) {
   auto recursive_abi = empty_abi();
   recursive_abi.structs = {
       protocol::struct_def{
           .name = "node",
           .fields =
               {
                   protocol::field_def{.name = "value", .type = "uint8"},
                   protocol::field_def{.name = "next", .type = "node?"},
               },
       },
   };

   auto depth_limits = chain_api::abi_serialization_limits{};
   depth_limits.max_recursion_depth = 5;
   BOOST_CHECK_EXCEPTION(
       static_cast<void>(chain_api::abi_json_to_bin(recursive_abi, "node", recursive_node(6), depth_limits)),
       chain_api::abi_serialization_error, [](const auto& error) {
          return error.diagnostic().code == chain_api::abi_error_code::recursion_limit &&
                 error.diagnostic().path.starts_with("node.next");
       });

   auto deadline_limits = chain_api::abi_serialization_limits{};
   deadline_limits.max_serialization_time = std::chrono::microseconds{0};
   BOOST_CHECK_EXCEPTION(
       static_cast<void>(chain_api::abi_json_to_bin(empty_abi(), "uint8", forge::variant{1}, deadline_limits)),
       chain_api::abi_serialization_error,
       [](const auto& error) { return error.diagnostic().code == chain_api::abi_error_code::deadline_exceeded; });

   auto binary_limits = chain_api::abi_serialization_limits{};
   binary_limits.max_binary_bytes = 2;
   BOOST_CHECK_EXCEPTION(
       static_cast<void>(chain_api::abi_json_to_bin(empty_abi(), "string", forge::variant{"abc"}, binary_limits)),
       chain_api::abi_serialization_error,
       [](const auto& error) { return error.diagnostic().code == chain_api::abi_error_code::size_limit; });

   auto array_limits = chain_api::abi_serialization_limits{};
   array_limits.max_container_elements = 2;
   BOOST_CHECK_EXCEPTION(
       static_cast<void>(chain_api::abi_json_to_bin(empty_abi(), "uint8[]", array(1, 2, 3), array_limits)),
       chain_api::abi_serialization_error,
       [](const auto& error) { return error.diagnostic().code == chain_api::abi_error_code::size_limit; });

   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_bin_to_json(
                             empty_abi(), "uint8[]", protocol::bytes{0xff, 0xff, 0xff, 0xff, 0x0f}, array_limits)),
                         chain_api::abi_serialization_error, [](const auto& error) {
                            return error.diagnostic().code == chain_api::abi_error_code::size_limit &&
                                   error.diagnostic().offset == 5U;
                         });
}

BOOST_AUTO_TEST_CASE(chain_abi_rejects_oversized_json_bytes_before_hex_decode) {
   auto limits = chain_api::abi_serialization_limits{};
   limits.max_string_bytes = 1U;

   BOOST_TEST(chain_api::abi_json_to_bin(empty_abi(), "bytes", forge::variant{"ff"}, limits) ==
              protocol::bytes({0x01, 0xff}));
   BOOST_CHECK_EXCEPTION(
       static_cast<void>(chain_api::abi_json_to_bin(empty_abi(), "bytes", forge::variant{"zzzz"}, limits)),
       chain_api::abi_serialization_error, [](const auto& error) {
          return error.diagnostic().code == chain_api::abi_error_code::size_limit &&
                 error.diagnostic().type == "bytes" && error.diagnostic().path == "bytes" &&
                 error.diagnostic().offset == 0U;
       });
}

BOOST_AUTO_TEST_CASE(chain_abi_rejects_invalid_definition_shapes) {
   auto extension_abi = empty_abi();
   extension_abi.structs = {
       protocol::struct_def{
           .name = "bad",
           .fields =
               {
                   protocol::field_def{.name = "extension", .type = "uint8$"},
                   protocol::field_def{.name = "required", .type = "uint8"},
               },
       },
   };
   BOOST_CHECK_EXCEPTION(
       static_cast<void>(chain_api::abi_json_to_bin(extension_abi, "bad", object({{"extension", 1}, {"required", 2}}))),
       chain_api::abi_serialization_error, [](const auto& error) {
          return error.diagnostic().code == chain_api::abi_error_code::invalid_abi &&
                 error.diagnostic().path == "bad.required";
       });

   auto circular_abi = empty_abi();
   circular_abi.types = {
       protocol::type_def{.new_type_name = "a", .type = "b"},
       protocol::type_def{.new_type_name = "b", .type = "a"},
   };
   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_json_to_bin(circular_abi, "a", forge::variant{1})),
                         chain_api::abi_serialization_error, [](const auto& error) {
                            return error.diagnostic().code == chain_api::abi_error_code::circular_definition;
                         });
}

namespace {

protocol::abi_def enum_abi() {
   auto abi = empty_abi();
   abi.types = {{"shape", "uint8"}, {"shape_alias", "shape"}, {"shapes", "shape_alias[]"}};
   abi.structs = {
       {"nested", "", {{"shape", "shape_alias"}}},
       {"request", "", {{"child", "nested"}, {"maybe", "shape_alias?"}, {"list", "shapes"},
                          {"fixed", "shape[2]"}, {"selection", "choice"}}},
   };
   abi.variants.value = {{"choice", {"shape_alias", "nested"}}};
   return abi;
}

protocol::abi_metadata enum_metadata() {
   return {.roots = {{"example::request", "request"}},
           .enums = {{"shape", "uint8", {{"circle", "0"}, {"tall", "2"}}}}};
}

} // namespace

BOOST_AUTO_TEST_CASE(chain_abi_metadata_preserves_raw_bytes_for_nested_enum_shapes) {
   const auto abi = enum_abi();
   const auto metadata = enum_metadata();
   const auto input = object({{"child", object({{"shape", "tall"}})}, {"maybe", "circle"},
                              {"list", array("circle", "tall")}, {"fixed", array("tall", "circle")},
                              {"selection", array("shape_alias", "tall")}});
   const auto expected = std::vector<std::uint8_t>{2, 1, 0, 2, 0, 2, 2, 0, 0, 2};
   const auto bytes = chain_api::abi_json_to_bin(abi, metadata, "request", input);
   BOOST_TEST(bytes == expected, boost::test_tools::per_element());
   BOOST_CHECK(chain_api::abi_bin_to_json(abi, metadata, "request", bytes) == input);
   BOOST_TEST(chain_api::abi_bin_to_json(abi, "shape", std::array<std::uint8_t, 1>{2}).as_uint64() == 2U);
   BOOST_TEST(chain_api::abi_json_to_bin(abi, "shape", forge::variant{2U}) == std::vector<std::uint8_t>{2},
              boost::test_tools::per_element());
   BOOST_TEST(chain_api::abi_json_to_bin(abi, metadata, "shape?", forge::variant{}) == std::vector<std::uint8_t>{0},
              boost::test_tools::per_element());
   const auto nested_variant = array("nested", object({{"shape", "circle"}}));
   const auto nested_bytes = chain_api::abi_json_to_bin(abi, metadata, "choice", nested_variant);
   BOOST_CHECK(chain_api::abi_bin_to_json(abi, metadata, "choice", nested_bytes) == nested_variant);
}

BOOST_AUTO_TEST_CASE(chain_abi_metadata_rejects_unknown_names_numbers_and_paths) {
   const auto abi = enum_abi();
   const auto metadata = enum_metadata();
   for (const auto& value : {forge::variant{"unknown"}, forge::variant{255U}, forge::variant{2U}}) {
      BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_json_to_bin(abi, metadata, "shape", value)),
                            chain_api::abi_serialization_error, [](const auto& error) {
                               return error.diagnostic().code == chain_api::abi_error_code::invalid_json &&
                                      error.diagnostic().path == "shape";
                            });
   }
   BOOST_CHECK_EXCEPTION(
       static_cast<void>(chain_api::abi_json_to_bin(abi, metadata, "nested", object({{"shape", "unknown"}}))),
       chain_api::abi_serialization_error, [](const auto& error) { return error.diagnostic().path == "nested.shape"; });
   BOOST_CHECK_EXCEPTION(
       static_cast<void>(chain_api::abi_json_to_bin(abi, metadata, "shape", forge::variant{std::string(33, 'x')},
                                                   {.max_string_bytes = 32})),
       chain_api::abi_serialization_error, [](const auto& error) {
          return error.diagnostic().code == chain_api::abi_error_code::size_limit;
       });
   BOOST_CHECK_EXCEPTION(
       static_cast<void>(chain_api::abi_bin_to_json(abi, metadata, "shape", std::array<std::uint8_t, 1>{1})),
       chain_api::abi_serialization_error, [](const auto& error) {
          return error.diagnostic().code == chain_api::abi_error_code::invalid_binary && error.diagnostic().offset == 1U;
       });
}

BOOST_AUTO_TEST_CASE(chain_abi_metadata_validates_schema_and_limits_before_conversion) {
   const auto abi = enum_abi();
   const auto check = [&](const protocol::abi_metadata& metadata) {
      BOOST_CHECK_THROW(static_cast<void>(chain_api::abi_json_to_bin(abi, metadata, "uint8", forge::variant{0U})),
                        chain_api::abi_serialization_error);
   };
   auto metadata = enum_metadata();
   metadata.version = "future";
   check(metadata);
   metadata = enum_metadata();
   metadata.enums.push_back(metadata.enums.front());
   check(metadata);
   metadata = enum_metadata();
   metadata.enums.front().values.push_back({"other", "2"});
   check(metadata);
   metadata = enum_metadata();
   metadata.enums.front().values.push_back({"tall", "3"});
   check(metadata);
   for (const auto& invalid : {"-1", "256", "02", "", "18446744073709551616"}) {
      metadata = enum_metadata();
      metadata.enums.front().values.front().value = invalid;
      check(metadata);
   }
   metadata = enum_metadata();
   metadata.enums.front().type = "uint16";
   check(metadata);
   metadata = enum_metadata();
   metadata.enums.front().name = "missing";
   check(metadata);
   metadata = enum_metadata();
   metadata.enums.front().values.clear();
   check(metadata);
   metadata = enum_metadata();
   metadata.roots.front().type = "missing";
   check(metadata);
   metadata = enum_metadata();
   metadata.roots.push_back(metadata.roots.front());
   check(metadata);
   for (const auto limits : {chain_api::abi_serialization_limits{.max_metadata_bytes = 3},
                             chain_api::abi_serialization_limits{.max_metadata_entries = 2},
                             chain_api::abi_serialization_limits{.max_string_bytes = 3}}) {
      BOOST_CHECK_EXCEPTION(
          static_cast<void>(chain_api::abi_json_to_bin(abi, enum_metadata(), "shape", forge::variant{"circle"}, limits)),
          chain_api::abi_serialization_error, [](const auto& error) {
             return error.diagnostic().code == chain_api::abi_error_code::size_limit;
          });
   }
}

BOOST_AUTO_TEST_CASE(chain_abi_metadata_preserves_extreme_integer_enum_values) {
   auto abi = empty_abi();
   abi.types = {{"signed_enum", "int64"}, {"unsigned_enum", "uint64"}};
   const auto metadata = protocol::abi_metadata{
       .enums = {{"signed_enum", "int64", {{"minimum", "-9223372036854775808"}}},
                 {"unsigned_enum", "uint64", {{"maximum", "18446744073709551615"}}}}};
   for (const auto& [type, value] : {std::pair{"signed_enum", "minimum"}, std::pair{"unsigned_enum", "maximum"}}) {
      const auto bytes = chain_api::abi_json_to_bin(abi, metadata, type, forge::variant{value});
      BOOST_TEST(bytes.size() == 8U);
      BOOST_TEST(chain_api::abi_bin_to_json(abi, metadata, type, bytes).get_string() == value);
   }
}

BOOST_AUTO_TEST_CASE(chain_abi_metadata_uses_existing_exact_json_codec) {
   const auto encoded = forge::codec::json::write(enum_metadata());
   BOOST_REQUIRE(encoded.ok());
   const auto decoded = forge::codec::json::read<protocol::abi_metadata>(encoded.text, {
       .unknown_fields = forge::codec::json::unknown_field_policy::error,
       .described_records = forge::codec::json::described_record_policy::exact});
   BOOST_REQUIRE(decoded.ok());
   BOOST_TEST(decoded.value.enums.front().values.back().name == "tall");
   BOOST_TEST(decoded.value.roots.front().cpp_type == "example::request");
   const auto invalid = forge::codec::json::read<protocol::abi_metadata>(
       R"({"version":"forge::abi-metadata/1.0","roots":[],"enums":[],"unknown":1})", {
       .unknown_fields = forge::codec::json::unknown_field_policy::error,
       .described_records = forge::codec::json::described_record_policy::exact});
   BOOST_TEST(!invalid.ok());
}

BOOST_AUTO_TEST_CASE(chain_abi_exact_scalar_policy_is_opt_in_and_preserves_compatible_bytes) {
   const auto abi = empty_abi();
   const auto exact = chain_api::abi_json_scalar_policy::exact;
   const auto compatible = chain_api::abi_json_scalar_policy::compatible;
   for (const auto& value : {forge::variant{"1"}, forge::variant{1.75}, forge::variant{true},
                             forge::variant{std::int64_t{4'294'967'297}}}) {
      BOOST_TEST(chain_api::abi_json_to_bin(abi, "int32", value) == forge::raw::pack(std::int32_t{1}));
      BOOST_TEST(chain_api::abi_json_to_bin(abi, "int32", value, {}, compatible) ==
                 chain_api::abi_json_to_bin(abi, "int32", value));
      BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_json_to_bin(abi, "int32", value, {}, exact)),
                            chain_api::abi_serialization_error, [](const auto& error) {
                               return error.diagnostic().code == chain_api::abi_error_code::invalid_json &&
                                      error.diagnostic().type == "int32" && error.diagnostic().path == "int32";
                            });
   }
   for (const auto& value : {forge::variant{"false"}, forge::variant{0}, forge::variant{0.0}}) {
      BOOST_TEST(chain_api::abi_json_to_bin(abi, "bool", value) == forge::raw::pack(false));
      BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_json_to_bin(abi, "bool", value, {}, exact)),
                            chain_api::abi_serialization_error, [](const auto& error) {
                               return error.diagnostic().code == chain_api::abi_error_code::invalid_json &&
                                      error.diagnostic().path == "bool";
                            });
   }
   for (const auto& value : {forge::variant{false}, forge::variant{true}}) {
      BOOST_TEST(chain_api::abi_json_to_bin(abi, "bool", value, {}, exact) ==
                 chain_api::abi_json_to_bin(abi, "bool", value));
   }
}

BOOST_AUTO_TEST_CASE(chain_abi_exact_integer_scalars_and_varints_enforce_native_bounds) {
   const auto abi = empty_abi();
   const auto exact = chain_api::abi_json_scalar_policy::exact;
   const auto cases = std::vector<std::pair<std::string, std::pair<std::int64_t, std::int64_t>>>{
       {"int8", {-128, 127}},
       {"uint8", {0, 255}},
       {"int16", {-32'768, 32'767}},
       {"uint16", {0, 65'535}},
       {"int32", {-2'147'483'648, 2'147'483'647}},
       {"uint32", {0, 4'294'967'295}},
       {"varint32", {-2'147'483'648, 2'147'483'647}},
       {"varuint32", {0, 4'294'967'295}},
   };
   for (const auto& [type, bounds] : cases) {
      BOOST_TEST_CONTEXT(type) {
         for (const auto value : {bounds.first, bounds.second}) {
            BOOST_TEST(chain_api::abi_json_to_bin(abi, type, forge::variant{value}, {}, exact) ==
                       chain_api::abi_json_to_bin(abi, type, forge::variant{value}));
         }
         for (const auto& value : {forge::variant{bounds.first - 1}, forge::variant{bounds.second + 1},
                                   forge::variant{"1"}, forge::variant{1.0}, forge::variant{true}, forge::variant{}}) {
            BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_json_to_bin(abi, type, value, {}, exact)),
                                  chain_api::abi_serialization_error, [&type](const auto& error) {
                                     return error.diagnostic().code == chain_api::abi_error_code::invalid_json &&
                                            error.diagnostic().path == type;
                                  });
         }
      }
   }
   for (const auto value : {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()}) {
      BOOST_TEST(chain_api::abi_json_to_bin(abi, "int64", forge::variant{value}, {}, exact) == forge::raw::pack(value));
   }
   BOOST_TEST(chain_api::abi_json_to_bin(abi, "uint64", forge::variant{std::numeric_limits<std::uint64_t>::max()}, {},
                                         exact) == forge::raw::pack(std::numeric_limits<std::uint64_t>::max()));
   for (const auto& [type, value] : {std::pair{"int64", forge::variant{std::numeric_limits<std::uint64_t>::max()}},
                                     std::pair{"uint64", forge::variant{-1}}}) {
      BOOST_CHECK_THROW(static_cast<void>(chain_api::abi_json_to_bin(abi, type, value, {}, exact)),
                        chain_api::abi_serialization_error);
   }
}

BOOST_AUTO_TEST_CASE(chain_abi_exact_strings_reject_coercion_at_roots_and_optional_alias_fields) {
   auto abi = empty_abi();
   abi.types.push_back({"text", "string"});
   abi.structs.push_back({"holder", "", {{"prompt", "text?"}}});
   const auto exact = chain_api::abi_json_scalar_policy::exact;
   const auto scalar = forge::variant{std::int64_t{42}};
   BOOST_TEST(chain_api::abi_json_to_bin(abi, "string", scalar) == forge::raw::pack(std::string{"42"}));
   BOOST_TEST(chain_api::abi_json_to_bin(abi, "string", scalar, {}, chain_api::abi_json_scalar_policy::compatible) ==
              chain_api::abi_json_to_bin(abi, "string", scalar));
   BOOST_TEST(chain_api::abi_json_to_bin(abi, "string", forge::variant{"42"}, {}, exact) ==
              forge::raw::pack(std::string{"42"}));
   for (const auto& value :
        {scalar, forge::variant{1.25}, forge::variant{true}, forge::variant{}, array(42), object({{"value", 42}})}) {
      BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_json_to_bin(abi, "string", value, {}, exact)),
                            chain_api::abi_serialization_error, [](const auto& error) {
                               return error.diagnostic().code == chain_api::abi_error_code::invalid_json &&
                                      error.diagnostic().type == "string" && error.diagnostic().path == "string";
                            });
   }
   BOOST_CHECK_EXCEPTION(
       static_cast<void>(chain_api::abi_json_to_bin(abi, forge::chain::protocol::abi_metadata{}, "holder",
                                                    object({{"prompt", scalar}}), {}, exact)),
       chain_api::abi_serialization_error, [](const auto& error) {
          return error.diagnostic().code == chain_api::abi_error_code::invalid_json &&
                 error.diagnostic().path == "holder.prompt";
       });
}

BOOST_AUTO_TEST_CASE(chain_abi_exact_scalar_policy_follows_optional_alias_array_and_variant_paths) {
   auto abi = empty_abi();
   abi.types = {{"count", "int32"}, {"optional_count", "count?"}, {"stream_flag", "bool"}};
   abi.structs = {{.name = "request", .fields = {{"n", "count?"}, {"stream", "stream_flag?"}}}};
   abi.variants.value = {{"choice", {"request", "count"}}};
   const auto exact = chain_api::abi_json_scalar_policy::exact;
   const auto omitted = object({});
   BOOST_TEST(chain_api::abi_json_to_bin(abi, "request", omitted, {}, exact) == protocol::bytes({0, 0}));
   BOOST_TEST(chain_api::abi_json_to_bin(abi, "optional_count", forge::variant{}, {}, exact) == protocol::bytes({0}));
   const auto valid = object({{"n", 1}, {"stream", false}});
   BOOST_TEST(chain_api::abi_json_to_bin(abi, "request", valid, {}, exact) ==
              chain_api::abi_json_to_bin(abi, "request", valid));
   for (const auto& [type, value, path] :
        {std::tuple{"request", object({{"n", "1"}}), "request.n"},
         std::tuple{"request", object({{"stream", 0}}), "request.stream"},
         std::tuple{"request[]", array(valid, object({{"n", 1.5}})), "request[][1].n"},
         std::tuple{"count[2]", array(1, true), "count[2][1]"},
         std::tuple{"choice", array("request", object({{"stream", "false"}})), "choice<request>.stream"}}) {
      BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_json_to_bin(abi, type, value, {}, exact)),
                            chain_api::abi_serialization_error, [&path](const auto& error) {
                               return error.diagnostic().code == chain_api::abi_error_code::invalid_json &&
                                      error.diagnostic().path == path;
                            });
   }
   const auto metadata = protocol::abi_metadata{};
   BOOST_TEST(chain_api::abi_json_to_bin(abi, metadata, "request", valid, {}, exact) ==
              chain_api::abi_json_to_bin(abi, "request", valid, {}, exact));
   BOOST_CHECK_THROW(
       static_cast<void>(chain_api::abi_json_to_bin(abi, metadata, "request", object({{"n", "1"}}), {}, exact)),
       chain_api::abi_serialization_error);
}

BOOST_AUTO_TEST_CASE(chain_abi_exact_wide_integers_reuse_canonical_schema_spelling_and_bounds) {
   const auto abi = empty_abi();
   const auto exact = chain_api::abi_json_scalar_policy::exact;
   for (const auto& [type, text] : {std::pair{"int128", "-170141183460469231731687303715884105728"},
                                    std::pair{"int128", "170141183460469231731687303715884105727"},
                                    std::pair{"uint128", "340282366920938463463374607431768211455"}}) {
      BOOST_TEST(chain_api::abi_json_to_bin(abi, type, forge::variant{text}, {}, exact) ==
                 chain_api::abi_json_to_bin(abi, type, forge::variant{text}));
   }
   for (const auto& [type, text] :
        {std::pair{"int128", "-170141183460469231731687303715884105729"},
         std::pair{"int128", "170141183460469231731687303715884105728"},
         std::pair{"uint128", "340282366920938463463374607431768211456"}, std::pair{"uint128", "-1"},
         std::pair{"int128", "01"}, std::pair{"int128", "+1"}, std::pair{"int128", "-0"}}) {
      BOOST_CHECK_THROW(static_cast<void>(chain_api::abi_json_to_bin(abi, type, forge::variant{text}, {}, exact)),
                        chain_api::abi_serialization_error);
   }
   BOOST_CHECK_THROW(static_cast<void>(chain_api::abi_json_to_bin(abi, "int128", forge::variant{1}, {}, exact)),
                     chain_api::abi_serialization_error);
}

BOOST_AUTO_TEST_CASE(chain_abi_exact_float_scalars_reuse_schema_precision_and_range_checks) {
   const auto abi = empty_abi();
   const auto exact = chain_api::abi_json_scalar_policy::exact;
   BOOST_TEST(chain_api::abi_json_to_bin(abi, "float32", forge::variant{1.5}, {}, exact) == forge::raw::pack(1.5F));
   for (const auto& value : {forge::variant{"1.5"}, forge::variant{true}, forge::variant{0.1},
                             forge::variant{std::numeric_limits<double>::infinity()},
                             forge::variant{std::numeric_limits<double>::quiet_NaN()}}) {
      BOOST_CHECK_THROW(static_cast<void>(chain_api::abi_json_to_bin(abi, "float32", value, {}, exact)),
                        chain_api::abi_serialization_error);
   }

   BOOST_CHECK_THROW(static_cast<void>(chain_api::abi_json_to_bin(
                         abi, "float64", forge::variant{std::uint64_t{9'007'199'254'740'993}}, {}, exact)),
                     chain_api::abi_serialization_error);
}

BOOST_AUTO_TEST_CASE(chain_abi_total_element_budget_bounds_zero_width_fixed_array_fields) {
   auto abi = empty_abi();
   abi.structs = {
       {.name = "empty"},
       {.name = "action", .fields = {{"a", "empty[2]"}, {"b", "empty[2]"}, {"c", "empty[2]"}, {"tail", "uint8"}}}};
   const auto value = object({{"a", array(object({}), object({}))},
                              {"b", array(object({}), object({}))},
                              {"c", array(object({}), object({}))},
                              {"tail", 7}});
   const auto binary = protocol::bytes{7};
   BOOST_CHECK(chain_api::abi_bin_to_json(abi, "action", binary) == value);
   const auto exact = chain_api::abi_serialization_limits{.max_total_container_elements = 6};
   BOOST_CHECK(chain_api::abi_bin_to_json(abi, "action", binary, exact) == value);
   BOOST_TEST(chain_api::abi_json_to_bin(abi, "action", value, exact) == binary);
   const auto small = chain_api::abi_serialization_limits{.max_total_container_elements = 3};
   const auto check = [](const auto& error) {
      return error.diagnostic().code == chain_api::abi_error_code::size_limit &&
             error.diagnostic().path == "action.b" && error.diagnostic().offset == 0U;
   };
   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_bin_to_json(abi, "action", binary, small)),
                         chain_api::abi_serialization_error, check);
   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_json_to_bin(abi, "action", value, small)),
                         chain_api::abi_serialization_error, check);
}

BOOST_AUTO_TEST_CASE(chain_abi_total_element_budget_is_cumulative_for_variable_arrays) {
   auto abi = empty_abi();
   abi.types = {{"empty_list", "empty[]"}};
   abi.structs = {{.name = "empty"}, {.name = "record", .fields = {{"a", "empty_list"}, {"b", "empty_list"}}}};
   const auto value = object({{"a", array(object({}), object({}))}, {"b", array(object({}))}});
   const auto binary = protocol::bytes{2, 1};
   const auto exact = chain_api::abi_serialization_limits{.max_total_container_elements = 3};
   BOOST_TEST(chain_api::abi_json_to_bin(abi, "record", value, exact) == binary);
   BOOST_CHECK(chain_api::abi_bin_to_json(abi, "record", binary, exact) == value);
   const auto small = chain_api::abi_serialization_limits{.max_total_container_elements = 2};
   const auto check = [](const auto& error) {
      return error.diagnostic().code == chain_api::abi_error_code::size_limit && error.diagnostic().path == "record.b";
   };
   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_bin_to_json(abi, "record", binary, small)),
                         chain_api::abi_serialization_error, check);
   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_json_to_bin(abi, "record", value, small)),
                         chain_api::abi_serialization_error, check);
}

BOOST_AUTO_TEST_CASE(chain_abi_total_element_budget_rejects_nested_zero_width_alias_expansion_before_allocation) {
   auto abi = empty_abi();
   abi.structs = {{.name = "empty"}};
   abi.types = {{"inner", "empty[65536]"}, {"nested", "inner[65536]"}};
   const auto limits = chain_api::abi_serialization_limits{.max_total_container_elements = 65537};
   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_bin_to_json(abi, "nested", protocol::bytes{}, limits)),
                         chain_api::abi_serialization_error, [](const auto& error) {
                            return error.diagnostic().code == chain_api::abi_error_code::size_limit &&
                                   error.diagnostic().path == "nested[0]" && error.diagnostic().offset == 0U;
                         });
}

BOOST_AUTO_TEST_CASE(chain_abi_total_element_budget_accepts_empty_arrays_and_leaves_metadata_limits_independent) {
   auto abi = empty_abi();
   abi.structs = {{.name = "empty"}};
   const auto zero = chain_api::abi_serialization_limits{.max_total_container_elements = 0};
   BOOST_TEST(chain_api::abi_json_to_bin(abi, "empty[]", array(), zero) == protocol::bytes({0}));
   BOOST_CHECK(chain_api::abi_bin_to_json(abi, "empty[]", protocol::bytes{0}, zero) == array());
   BOOST_TEST(chain_api::abi_json_to_bin(abi, "empty[0]", array(), zero).empty());
   BOOST_CHECK(chain_api::abi_bin_to_json(abi, "empty[0]", protocol::bytes{}, zero) == array());
   BOOST_CHECK_THROW(static_cast<void>(chain_api::abi_bin_to_json(abi, "empty[1]", protocol::bytes{}, zero)),
                     chain_api::abi_serialization_error);
   const auto metadata_abi = enum_abi();
   const auto metadata = enum_metadata();
   const auto binary = chain_api::abi_json_to_bin(metadata_abi, metadata, "shape", forge::variant{"circle"}, zero,
                                                  chain_api::abi_json_scalar_policy::exact);
   BOOST_TEST(binary == protocol::bytes({0}));
   BOOST_TEST(chain_api::abi_bin_to_json(metadata_abi, metadata, "shape", binary, zero).get_string() == "circle");
}

BOOST_AUTO_TEST_CASE(chain_abi_nested_optional_record_arrays_preserve_presence_and_raw_bytes) {
   auto abi = empty_abi();
   abi.types = {{"nullable_items", "item[]?"}};
   abi.structs = {{"item", "", {{"value", "uint16"}}}, {"response", "", {{"items", "item[]?"}}}};
   const auto values = array(object({{"value", 7}}), object({{"value", 9}}));
   for (const auto& [input, bytes] :
        {std::pair{forge::variant{}, protocol::bytes{0}}, std::pair{array(), protocol::bytes{1, 0}},
         std::pair{values, protocol::bytes{1, 2, 7, 0, 9, 0}}}) {
      const auto native = input.is_null()             ? std::optional<std::vector<std::uint16_t>>{}
                          : input.get_array().empty() ? std::optional{std::vector<std::uint16_t>{}}
                                                      : std::optional{std::vector<std::uint16_t>{7, 9}};
      BOOST_TEST(forge::raw::pack(native) == bytes);
      BOOST_TEST(chain_api::abi_json_to_bin(abi, "nullable_items", input) == bytes);
      BOOST_CHECK(chain_api::abi_bin_to_json(abi, "nullable_items", bytes) == input);
      const auto response = object({{"items", input}});
      BOOST_TEST(chain_api::abi_json_to_bin(abi, "response", response) == bytes);
      BOOST_CHECK(chain_api::abi_bin_to_json(abi, "response", bytes) == response);
   }
   BOOST_TEST(chain_api::abi_json_to_bin(abi, "response", object({})) == protocol::bytes({0}));
   BOOST_CHECK(chain_api::abi_bin_to_json(abi, "response", protocol::bytes{0}) ==
               object({{"items", forge::variant{}}}));
}

BOOST_AUTO_TEST_CASE(chain_abi_nested_fixed_dynamic_optional_modifiers_round_trip) {
   auto abi = empty_abi();
   abi.types = {{"matrix", "uint8?[][2]?"}};
   const auto value = array(array(forge::variant{}, 7), array());
   const auto bytes = protocol::bytes{1, 2, 0, 1, 7, 0};
   const auto native = std::optional{std::array{std::vector<std::optional<std::uint8_t>>{std::nullopt, std::uint8_t{7}},
                                                std::vector<std::optional<std::uint8_t>>{}}};
   BOOST_TEST(forge::raw::pack(native) == bytes);
   BOOST_TEST(chain_api::abi_json_to_bin(abi, "matrix", value) == bytes);
   BOOST_CHECK(chain_api::abi_bin_to_json(abi, "matrix", bytes) == value);
   BOOST_TEST(chain_api::abi_json_to_bin(abi, "matrix", forge::variant{}) == protocol::bytes({0}));
   BOOST_CHECK(chain_api::abi_bin_to_json(abi, "matrix", protocol::bytes{0}).is_null());
   BOOST_TEST(chain_api::abi_json_to_bin(empty_abi(), "uint8[2]?", array(7, 9)) == protocol::bytes({1, 7, 9}));
   BOOST_CHECK(chain_api::abi_bin_to_json(empty_abi(), "uint8[2]?", protocol::bytes{1, 7, 9}) == array(7, 9));
}

BOOST_AUTO_TEST_CASE(chain_abi_nested_modifiers_keep_enum_metadata_and_exact_scalar_policy) {
   auto abi = enum_abi();
   abi.types.push_back({"nullable_shapes", "shape_alias[]?"});
   const auto metadata = enum_metadata();
   const auto exact = chain_api::abi_json_scalar_policy::exact;
   const auto input = array("circle", "tall");
   const auto bytes = protocol::bytes{1, 2, 0, 2};
   BOOST_TEST(chain_api::abi_json_to_bin(abi, metadata, "nullable_shapes", input, {}, exact) == bytes);
   BOOST_CHECK(chain_api::abi_bin_to_json(abi, metadata, "nullable_shapes", bytes) == input);
   BOOST_CHECK_EXCEPTION(
       static_cast<void>(chain_api::abi_json_to_bin(abi, metadata, "nullable_shapes", array("circle", 2), {}, exact)),
       chain_api::abi_serialization_error, [](const auto& error) {
          return error.diagnostic().code == chain_api::abi_error_code::invalid_json &&
                 error.diagnostic().path == "nullable_shapes[1]";
       });
   BOOST_CHECK_EXCEPTION(
       static_cast<void>(chain_api::abi_json_to_bin(empty_abi(), "uint8[]?", array(1, "2"), {}, exact)),
       chain_api::abi_serialization_error, [](const auto& error) {
          return error.diagnostic().code == chain_api::abi_error_code::invalid_json &&
                 error.diagnostic().path == "uint8[]?[1]";
       });
}

BOOST_AUTO_TEST_CASE(chain_abi_nested_modifiers_reject_invalid_types_json_and_truncated_binary) {
   for (const auto& type : {"missing[]?", "uint8[bad][]?", "uint8$[]?", "uint8[]$?", "[]?"}) {
      const auto check = [](const auto& error) {
         return error.diagnostic().code == chain_api::abi_error_code::unknown_type;
      };
      BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_json_to_bin(empty_abi(), type, forge::variant{})),
                            chain_api::abi_serialization_error, check);
      BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_bin_to_json(empty_abi(), type, protocol::bytes{0})),
                            chain_api::abi_serialization_error, check);
      auto abi = empty_abi();
      abi.structs = {{"response", "", {{"items", type}}}};
      BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_json_to_bin(abi, "response", object({}))),
                            chain_api::abi_serialization_error, check);
   }
   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_json_to_bin(empty_abi(), "uint8?[]$", array())),
                         chain_api::abi_serialization_error, [](const auto& error) {
                            return error.diagnostic().code == chain_api::abi_error_code::unknown_type;
                         });
   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_json_to_bin(empty_abi(), "uint8[]?", forge::variant{7})),
                         chain_api::abi_serialization_error, [](const auto& error) {
                            return error.diagnostic().code == chain_api::abi_error_code::invalid_json;
                         });
   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_json_to_bin(empty_abi(), "uint8[2]?", array())),
                         chain_api::abi_serialization_error, [](const auto& error) {
                            return error.diagnostic().code == chain_api::abi_error_code::invalid_json;
                         });
   BOOST_CHECK_EXCEPTION(
       static_cast<void>(chain_api::abi_bin_to_json(empty_abi(), "uint8[]?", protocol::bytes{1, 2, 7})),
       chain_api::abi_serialization_error, [](const auto& error) {
          return error.diagnostic().code == chain_api::abi_error_code::invalid_binary &&
                 error.diagnostic().path == "uint8[]?[1]";
       });
   for (const auto& [bytes, path] :
        {std::pair{protocol::bytes{2}, "uint8?[]?"}, std::pair{protocol::bytes{1, 1, 2}, "uint8?[]?[0]"}}) {
      BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_bin_to_json(empty_abi(), "uint8?[]?", bytes)),
                            chain_api::abi_serialization_error, [&path](const auto& error) {
                               return error.diagnostic().code == chain_api::abi_error_code::invalid_binary &&
                                      error.diagnostic().path == path;
                            });
   }
}

BOOST_AUTO_TEST_CASE(chain_abi_nested_modifiers_enforce_type_depth_and_container_budgets) {
   const auto type = std::string_view{"uint8[][]?"};
   const auto value = array(array(1, 2), array(3));
   const auto bytes = protocol::bytes{1, 2, 2, 1, 2, 1, 3};
   const auto enough = chain_api::abi_serialization_limits{.max_total_container_elements = 5};
   BOOST_TEST(chain_api::abi_json_to_bin(empty_abi(), type, value, enough) == bytes);
   BOOST_CHECK(chain_api::abi_bin_to_json(empty_abi(), type, bytes, enough) == value);
   const auto small = chain_api::abi_serialization_limits{.max_total_container_elements = 4};
   const auto total_limit = [](const auto& error) {
      return error.diagnostic().code == chain_api::abi_error_code::size_limit &&
             error.diagnostic().path == "uint8[][]?[1]";
   };
   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_json_to_bin(empty_abi(), type, value, small)),
                         chain_api::abi_serialization_error, total_limit);
   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_bin_to_json(empty_abi(), type, bytes, small)),
                         chain_api::abi_serialization_error, total_limit);
   const auto array_limit = chain_api::abi_serialization_limits{.max_container_elements = 2};
   const auto inner_limit = [](const auto& error) {
      return error.diagnostic().code == chain_api::abi_error_code::size_limit &&
             error.diagnostic().path == "uint8[][]?[0]";
   };
   BOOST_CHECK_EXCEPTION(
       static_cast<void>(chain_api::abi_json_to_bin(empty_abi(), type, array(array(1, 2, 3)), array_limit)),
       chain_api::abi_serialization_error, inner_limit);
   BOOST_CHECK_EXCEPTION(
       static_cast<void>(chain_api::abi_bin_to_json(empty_abi(), type, protocol::bytes{1, 1, 3}, array_limit)),
       chain_api::abi_serialization_error, inner_limit);
   const auto resource_limit = [](const auto& error) {
      return error.diagnostic().code == chain_api::abi_error_code::size_limit;
   };
   BOOST_CHECK_EXCEPTION(
       static_cast<void>(chain_api::abi_json_to_bin(empty_abi(), "uint8[3][]?", forge::variant{}, array_limit)),
       chain_api::abi_serialization_error, resource_limit);
   BOOST_CHECK_EXCEPTION(
       static_cast<void>(chain_api::abi_bin_to_json(empty_abi(), "uint8[3][]?", protocol::bytes{0}, array_limit)),
       chain_api::abi_serialization_error, resource_limit);
   const auto byte_limit = chain_api::abi_serialization_limits{.max_binary_bytes = 3};
   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_json_to_bin(empty_abi(), type, value, byte_limit)),
                         chain_api::abi_serialization_error, resource_limit);
   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_bin_to_json(empty_abi(), type, bytes, byte_limit)),
                         chain_api::abi_serialization_error, resource_limit);
   const auto depth_limit = chain_api::abi_serialization_limits{.max_recursion_depth = 3};
   const auto depth_error = [](const auto& error) {
      return error.diagnostic().code == chain_api::abi_error_code::recursion_limit;
   };
   BOOST_CHECK_EXCEPTION(
       static_cast<void>(chain_api::abi_json_to_bin(empty_abi(), type, forge::variant{}, depth_limit)),
       chain_api::abi_serialization_error, depth_error);
   BOOST_CHECK_EXCEPTION(
       static_cast<void>(chain_api::abi_bin_to_json(empty_abi(), type, protocol::bytes{0}, depth_limit)),
       chain_api::abi_serialization_error, depth_error);
   const auto deadline_limit =
       chain_api::abi_serialization_limits{.max_serialization_time = std::chrono::microseconds{0}};
   BOOST_CHECK_EXCEPTION(
       static_cast<void>(chain_api::abi_json_to_bin(empty_abi(), type, forge::variant{}, deadline_limit)),
       chain_api::abi_serialization_error,
       [](const auto& error) { return error.diagnostic().code == chain_api::abi_error_code::deadline_exceeded; });
}

BOOST_AUTO_TEST_CASE(chain_abi_nested_modifiers_reject_adjacent_optionals_after_alias_resolution) {
   const auto check = [](const auto& error) {
      return error.diagnostic().code == chain_api::abi_error_code::unknown_type;
   };
   for (const auto& type : {"uint8??", "uint8[]??", "uint8??[]"}) {
      BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_json_to_bin(empty_abi(), type, forge::variant{})),
                            chain_api::abi_serialization_error, check);
      BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_bin_to_json(empty_abi(), type, protocol::bytes{1, 0})),
                            chain_api::abi_serialization_error, check);
   }
   auto abi = empty_abi();
   abi.types = {{"maybe", "uint8?"}};
   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_json_to_bin(abi, "maybe?", forge::variant{})),
                         chain_api::abi_serialization_error, check);
   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_bin_to_json(abi, "maybe?", protocol::bytes{1, 0})),
                         chain_api::abi_serialization_error, check);
   abi.types.push_back({"nested", "maybe?"});
   BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_json_to_bin(abi, "uint8", forge::variant{0})),
                         chain_api::abi_serialization_error, check);
}

BOOST_AUTO_TEST_CASE(chain_abi_nested_modifiers_preserve_binary_extension_array_restrictions) {
   auto abi = empty_abi();
   abi.structs = {{"item", "", {{"value", "uint8"}, {"tail", "uint8$"}}}, {"response", "", {{"items", "item[]?$"}}}};
   BOOST_TEST(chain_api::abi_json_to_bin(abi, "response", object({})).empty());
   BOOST_CHECK(chain_api::abi_bin_to_json(abi, "response", protocol::bytes{}) == object({}));
   const auto complete = array(object({{"value", 7}, {"tail", 9}}));
   for (const auto& [type, bytes, truncated] :
        {std::tuple{"item[]?", protocol::bytes{1, 1, 7, 9}, protocol::bytes{1, 1, 7}},
         std::tuple{"item[1]?", protocol::bytes{1, 7, 9}, protocol::bytes{1, 7}}}) {
      BOOST_TEST(chain_api::abi_json_to_bin(abi, type, complete) == bytes);
      BOOST_CHECK(chain_api::abi_bin_to_json(abi, type, bytes) == complete);
      BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_json_to_bin(abi, type, array(object({{"value", 7}})))),
                            chain_api::abi_serialization_error, [](const auto& error) {
                               return error.diagnostic().code == chain_api::abi_error_code::missing_field &&
                                      error.diagnostic().path.ends_with("[0].tail");
                            });
      BOOST_CHECK_EXCEPTION(static_cast<void>(chain_api::abi_bin_to_json(abi, type, truncated)),
                            chain_api::abi_serialization_error, [](const auto& error) {
                               return error.diagnostic().code == chain_api::abi_error_code::invalid_binary &&
                                      error.diagnostic().path.ends_with("[0].tail");
                            });
   }
}
