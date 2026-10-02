#include <boost/describe.hpp>
#include <boost/pfr/traits.hpp>
#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace forge_diagnostic_tests {

inline auto secret_serializer_calls = 0;
inline auto custom_serializer_calls = 0;
inline auto c_string_conversion_calls = 0;
inline auto wide_string_conversion_calls = 0;

struct throwing_secret {
   std::string value;

   throwing_secret() = default;
   explicit throwing_secret(std::string text) : value{std::move(text)} {}

   [[nodiscard]] std::string to_string() const {
      ++secret_serializer_calls;
      throw std::runtime_error{value};
   }
};

struct record {
   std::string id;
   throwing_secret token;
   std::optional<std::string> note;
};
BOOST_DESCRIBE_STRUCT(record, (), (id, token, note))

struct described_parent {
   std::vector<record> children;
   std::map<std::string, record> keyed;
   std::optional<record> maybe;
   std::variant<int, record> choice;
};
BOOST_DESCRIBE_STRUCT(described_parent, (), (children, keyed, maybe, choice))

struct aggregate_record {
   std::string label;
   std::string token;
};

struct aggregate_parent {
   std::string title;
   aggregate_record child;
   std::vector<aggregate_record> children;
};

struct empty_schema_record {
   std::string token;
};
BOOST_DESCRIBE_STRUCT(empty_schema_record, (), (token))

struct convertible_schema_record {
   std::string token;
   operator const char*() const {
      ++c_string_conversion_calls;
      return token.c_str();
   }
};
BOOST_DESCRIBE_STRUCT(convertible_schema_record, (), (token))

struct convertible_schema_parent {
   convertible_schema_record child;
};
BOOST_DESCRIBE_STRUCT(convertible_schema_parent, (), (child))

struct wide_convertible_schema_record {
   std::string token;
   operator const wchar_t*() const {
      ++wide_string_conversion_calls;
      return L"unexposed";
   }
};
BOOST_DESCRIBE_STRUCT(wide_convertible_schema_record, (), (token))

struct described_custom {
   std::string text;
};
BOOST_DESCRIBE_STRUCT(described_custom, (), (text))

struct aggregate_custom {
   std::string text;
};

struct custom_parent {
   record child;
};
BOOST_DESCRIBE_STRUCT(custom_parent, (), (child))

struct custom_list_parent {
   std::initializer_list<record> children;
};
BOOST_DESCRIBE_STRUCT(custom_list_parent, (), (children))

struct failed_custom {
   std::string text;
};

struct sealed {
   std::string text;
};

struct bitfields {
   unsigned value : 3;
};

struct base {
   int value;
};
struct inherited : base {};

struct referenced {
   int& value;
};

union union_value {
   int number;
   double decimal;
};

class opaque {
 public:
   explicit opaque(int value) : value_{value} {}

 private:
   [[maybe_unused]] int value_;
};

struct recursive {
   std::shared_ptr<recursive> next;
   std::string label;
};
BOOST_DESCRIBE_STRUCT(recursive, (), (next, label))

struct branching {
   std::shared_ptr<branching> first;
   std::shared_ptr<branching> second;
   std::string label;
};
BOOST_DESCRIBE_STRUCT(branching, (), (first, second, label))

struct alias_inner {
   int value;
};
BOOST_DESCRIBE_STRUCT(alias_inner, (), (value))

struct alias_outer {
   alias_inner first;
   std::shared_ptr<alias_inner> alias;
};
BOOST_DESCRIBE_STRUCT(alias_outer, (), (first, alias))

struct budgeted_node {
   std::shared_ptr<budgeted_node> first;
   std::shared_ptr<budgeted_node> second;
};
BOOST_DESCRIBE_STRUCT(budgeted_node, (), (first, second))

} // namespace forge_diagnostic_tests

template <typename Tag> struct boost::pfr::is_reflectable<forge_diagnostic_tests::bitfields, Tag> : std::false_type {};
template <typename Tag> struct boost::pfr::is_reflectable<forge_diagnostic_tests::inherited, Tag> : std::false_type {};
template <typename Tag> struct boost::pfr::is_reflectable<forge_diagnostic_tests::referenced, Tag> : std::false_type {};

import forge.reflect.reflect;
import forge.schema.object;
import forge.schema.value_kind;
import forge.variant.value;
import forge.variant.containers;
import forge.variant.schema;
import forge.variant.described;

template <> struct forge::schema::member_kind<forge_diagnostic_tests::throwing_secret> {
   static constexpr auto value = forge::schema::value_kind::string;
};

namespace forge_diagnostic_tests {

void to_variant(const throwing_secret& value, forge::variant&) {
   ++secret_serializer_calls;
   throw std::runtime_error{value.value};
}

void to_variant(const described_custom& value, forge::variant& output) {
   ++custom_serializer_calls;
   output = "custom:" + value.text;
}

void to_variant(const aggregate_custom& value, forge::variant& output) {
   ++custom_serializer_calls;
   output = "aggregate-custom:" + value.text;
}

void to_variant(const custom_parent& value, forge::variant& output) {
   ++custom_serializer_calls;
   output = forge::variant{value.child};
}

void to_variant(const custom_list_parent& value, forge::variant& output) {
   ++custom_serializer_calls;
   output = forge::variant{value.children};
}

void to_variant(const convertible_schema_parent& value, forge::variant& output) {
   ++custom_serializer_calls;
   output = static_cast<const char*>(value.child);
}

void to_variant(const failed_custom& value, forge::variant&) {
   ++custom_serializer_calls;
   throw std::runtime_error{value.text};
}

void to_variant(const budgeted_node&, forge::variant& output) {
   ++custom_serializer_calls;
   output = "ordinary-dag";
}

void to_variant(const sealed& value, forge::variant&) {
   ++secret_serializer_calls;
   throw std::runtime_error{value.text};
}

[[nodiscard]] constexpr bool diagnostic_is_secret(const sealed&) noexcept {
   return true;
}

class schema_only {
 public:
   [[nodiscard]] static auto schema() {
      auto output = forge::schema::object<schema_only>();
      static_cast<void>(output.field<&schema_only::value_>("public-value"));
      output.field<&schema_only::token_>("private-token").secret();
      return output;
   }

 private:
   std::string value_ = "public";
   throwing_secret token_{"never serialize"};
};

} // namespace forge_diagnostic_tests

template <> struct forge::schema::rules<forge_diagnostic_tests::record> {
   [[nodiscard]] static auto define() {
      auto output = forge::schema::object<forge_diagnostic_tests::record>();
      static_cast<void>(output.field<&forge_diagnostic_tests::record::id>("record-id").alias("old-id"));
      output.field<&forge_diagnostic_tests::record::token>("auth.token").alias("old-token").secret();
      static_cast<void>(output.field<&forge_diagnostic_tests::record::note>("note"));
      return output;
   }
};

template <> struct forge::schema::rules<forge_diagnostic_tests::aggregate_record> {
   [[nodiscard]] static auto define() {
      auto output = forge::schema::object<forge_diagnostic_tests::aggregate_record>();
      static_cast<void>(output.field<&forge_diagnostic_tests::aggregate_record::label>("label-name"));
      output.field<&forge_diagnostic_tests::aggregate_record::token>("auth-token").alias("legacy-token").secret();
      return output;
   }
};

template <> struct forge::schema::rules<forge_diagnostic_tests::empty_schema_record> {
   [[nodiscard]] static auto define() {
      return forge::schema::object<forge_diagnostic_tests::empty_schema_record>();
   }
};

template <> struct forge::schema::rules<forge_diagnostic_tests::convertible_schema_record> {
   [[nodiscard]] static auto define() {
      auto output = forge::schema::object<forge_diagnostic_tests::convertible_schema_record>();
      output.field<&forge_diagnostic_tests::convertible_schema_record::token>("access-token").secret();
      return output;
   }
};

template <> struct forge::schema::rules<forge_diagnostic_tests::schema_only> {
   [[nodiscard]] static auto define() {
      return forge_diagnostic_tests::schema_only::schema();
   }
};

template <> struct forge::schema::rules<forge_diagnostic_tests::wide_convertible_schema_record> {
   [[nodiscard]] static auto define() {
      auto output = forge::schema::object<forge_diagnostic_tests::wide_convertible_schema_record>();
      output.field<&forge_diagnostic_tests::wide_convertible_schema_record::token>("access-token").secret();
      return output;
   }
};

static_assert(forge::reflect::is_diagnostic_aggregate_v<forge_diagnostic_tests::aggregate_parent>);
static_assert(!forge::reflect::is_diagnostic_aggregate_v<forge_diagnostic_tests::bitfields>);
static_assert(!forge::reflect::is_diagnostic_aggregate_v<forge_diagnostic_tests::inherited>);
static_assert(!forge::reflect::is_diagnostic_aggregate_v<forge_diagnostic_tests::referenced>);
static_assert(!forge::reflect::is_diagnostic_aggregate_v<forge_diagnostic_tests::union_value>);
static_assert(forge::schema::has_explicit_rules_v<forge_diagnostic_tests::empty_schema_record>);
static_assert(!forge::schema::has_explicit_rules_v<forge_diagnostic_tests::aggregate_parent>);

BOOST_AUTO_TEST_SUITE(diagnostic_test_suite)

BOOST_AUTO_TEST_CASE(schema_redacts_before_secret_serializer_and_uses_canonical_paths) {
   using namespace forge_diagnostic_tests;
   secret_serializer_calls = 0;
   const auto input = record{"one", throwing_secret{"unexposed"}, std::nullopt};
   const auto output = forge::variant_schema::encode_diagnostic(input);
   BOOST_REQUIRE(output.is_object());
   const auto& object = output.get_object();
   BOOST_TEST(object["record-id"].as_string() == "one");
   BOOST_TEST(object["auth"]["token"].as_string() == "<redacted>");
   BOOST_TEST(!object.contains("old-id"));
   BOOST_TEST(!object.contains("old-token"));
   BOOST_TEST(!object.contains("note"));
   BOOST_TEST(secret_serializer_calls == 0);
   BOOST_CHECK_THROW(static_cast<void>(forge::variant_schema::encode(input)), std::runtime_error);
   BOOST_TEST(secret_serializer_calls == 1);
}

BOOST_AUTO_TEST_CASE(schema_less_described_parent_redacts_nested_containers) {
   using namespace forge_diagnostic_tests;
   secret_serializer_calls = 0;
   const auto child = record{"child", throwing_secret{"unexposed"}, "visible"};
   const auto input = described_parent{{child}, {{"entry", child}}, child, child};
   const auto output = forge::variant_schema::encode_diagnostic(input);
   BOOST_REQUIRE(output.is_object());
   BOOST_TEST(output["children"][std::size_t{0}]["auth"]["token"].as_string() == "<redacted>");
   BOOST_TEST(output["keyed"][std::size_t{0}][1]["auth"]["token"].as_string() == "<redacted>");
   BOOST_TEST(output["maybe"]["record-id"].as_string() == "child");
   BOOST_TEST(output["choice"][1]["auth"]["token"].as_string() == "<redacted>");
   BOOST_TEST(secret_serializer_calls == 0);
}

BOOST_AUTO_TEST_CASE(pfr_string_aggregate_and_nested_schema_preserve_readable_fields) {
   using namespace forge_diagnostic_tests;
   const auto input = aggregate_parent{"plain", {"child", "unexposed"}, {{"nested", "unexposed"}}};
   const auto output = forge::variant_schema::encode_diagnostic(input);
   BOOST_REQUIRE(output.is_object());
   BOOST_TEST(output["title"].as_string() == "plain");
   BOOST_TEST(output["child"]["label-name"].as_string() == "child");
   BOOST_TEST(output["child"]["auth-token"].as_string() == "<redacted>");
   BOOST_TEST(output["children"][std::size_t{0}]["auth-token"].as_string() == "<redacted>");
}

BOOST_AUTO_TEST_CASE(initializer_lists_redact_directly_and_before_parent_custom_conversion) {
   using namespace forge_diagnostic_tests;
   secret_serializer_calls = 0;
   custom_serializer_calls = 0;
   const auto children = std::initializer_list<record>{{"child", throwing_secret{"unexposed"}, {}}};
   const auto direct = forge::variant_schema::encode_diagnostic(children);
   BOOST_REQUIRE(direct.is_array());
   BOOST_TEST(direct[std::size_t{0}]["auth"]["token"].as_string() == "<redacted>");
   const auto parent = forge::variant_schema::encode_diagnostic(custom_list_parent{children});
   BOOST_REQUIRE(parent.is_object());
   BOOST_TEST(parent["children"][std::size_t{0}]["auth"]["token"].as_string() == "<redacted>");
   BOOST_TEST(secret_serializer_calls == 0);
   BOOST_TEST(custom_serializer_calls == 0);
}

BOOST_AUTO_TEST_CASE(custom_conversion_precedes_describe_and_pfr_unless_redaction_requires_traversal) {
   using namespace forge_diagnostic_tests;
   custom_serializer_calls = 0;
   secret_serializer_calls = 0;
   BOOST_TEST(forge::variant_schema::encode_diagnostic(described_custom{"visible"}).as_string() == "custom:visible");
   BOOST_TEST(forge::variant_schema::encode_diagnostic(aggregate_custom{"visible"}).as_string() ==
              "aggregate-custom:visible");
   BOOST_TEST(custom_serializer_calls == 2);
   const auto output =
       forge::variant_schema::encode_diagnostic(custom_parent{{"id", throwing_secret{"unexposed"}, {}}});
   BOOST_REQUIRE(output.is_object());
   BOOST_TEST(output["child"]["auth"]["token"].as_string() == "<redacted>");
   BOOST_TEST(custom_serializer_calls == 2);
   BOOST_TEST(secret_serializer_calls == 0);
}

BOOST_AUTO_TEST_CASE(explicit_empty_schema_has_no_describe_fallback) {
   const auto output =
       forge::variant_schema::encode_diagnostic(forge_diagnostic_tests::empty_schema_record{"unexposed"});
   BOOST_REQUIRE(output.is_object());
   BOOST_TEST(output.get_object().size() == 0U);
}

BOOST_AUTO_TEST_CASE(class_c_string_conversion_cannot_bypass_schema_secret_policy) {
   using namespace forge_diagnostic_tests;
   custom_serializer_calls = 0;
   c_string_conversion_calls = 0;
   const auto direct = forge::variant_schema::encode_diagnostic(convertible_schema_record{"unexposed"});
   BOOST_REQUIRE(direct.is_object());
   BOOST_TEST(direct["access-token"].as_string() == "<redacted>");
   const auto parent = forge::variant_schema::encode_diagnostic(convertible_schema_parent{{"unexposed"}});
   BOOST_REQUIRE(parent.is_object());
   BOOST_TEST(parent["child"]["access-token"].as_string() == "<redacted>");
   BOOST_TEST(custom_serializer_calls == 0);
   BOOST_TEST(c_string_conversion_calls == 0);
   const char* text = "visible";
   BOOST_TEST(forge::variant_schema::encode_diagnostic(text).as_string() == "visible");
   BOOST_TEST(forge::variant_schema::encode_diagnostic("literal").as_string() == "literal");
}

BOOST_AUTO_TEST_CASE(ordinary_initializer_list_encoding_keeps_baseline_describe_field_names) {
   const auto input = std::initializer_list<forge_diagnostic_tests::convertible_schema_record>{{"plain"}};
   const auto ordinary = forge::variant_schema::encode(input);
   BOOST_REQUIRE(ordinary.is_array());
   const auto& object = ordinary[std::size_t{0}].get_object();
   BOOST_TEST(object["token"].as_string() == "plain");
   BOOST_TEST(!object.contains("access-token"));
   const auto diagnostic = forge::variant_schema::encode_diagnostic(input);
   BOOST_TEST(diagnostic[std::size_t{0}]["access-token"].as_string() == "<redacted>");
}

BOOST_AUTO_TEST_CASE(diagnostic_wide_strings_reuse_existing_variant_constructors) {
   wchar_t mutable_text[] = L"visible";
   wchar_t* mutable_pointer = mutable_text;
   const wchar_t* const_pointer = mutable_text;
   BOOST_CHECK(forge::variant_schema::encode_diagnostic(mutable_text) == forge::variant{mutable_text});
   BOOST_CHECK(forge::variant_schema::encode_diagnostic(mutable_pointer) == forge::variant{mutable_pointer});
   BOOST_CHECK(forge::variant_schema::encode_diagnostic(const_pointer) == forge::variant{const_pointer});
   BOOST_CHECK(forge::variant_schema::encode_diagnostic(L"literal") == forge::variant{L"literal"});
   BOOST_CHECK(forge::variant_schema::encode_diagnostic(L"\u041F\u0440\u0438\u0432\u0435\u0442") ==
               forge::variant{L"\u041F\u0440\u0438\u0432\u0435\u0442"});
   const_pointer = nullptr;
   mutable_pointer = nullptr;
   BOOST_TEST(forge::variant_schema::encode_diagnostic(const_pointer).is_null());
   BOOST_TEST(forge::variant_schema::encode_diagnostic(mutable_pointer).is_null());
}

BOOST_AUTO_TEST_CASE(class_wide_string_conversion_cannot_bypass_schema_secret_policy) {
   using namespace forge_diagnostic_tests;
   wide_string_conversion_calls = 0;
   const auto input = wide_convertible_schema_record{"unexposed"};
   const auto direct = forge::variant_schema::encode_diagnostic(input);
   BOOST_REQUIRE(direct.is_object());
   BOOST_TEST(direct["access-token"].as_string() == "<redacted>");
   const auto optional = forge::variant_schema::encode_diagnostic(std::optional{input});
   BOOST_REQUIRE(optional.is_object());
   BOOST_TEST(optional["access-token"].as_string() == "<redacted>");
   BOOST_TEST(wide_string_conversion_calls == 0);
}

BOOST_AUTO_TEST_CASE(schema_only_opaque_members_are_explicitly_unsupported_and_secrets_redacted) {
   forge_diagnostic_tests::secret_serializer_calls = 0;
   const auto output = forge::variant_schema::encode_diagnostic(forge_diagnostic_tests::schema_only{});
   BOOST_REQUIRE(output.is_object());
   BOOST_TEST(output["public-value"].as_string() == "<unsupported>");
   BOOST_TEST(output["private-token"].as_string() == "<redacted>");
   BOOST_TEST(forge_diagnostic_tests::secret_serializer_calls == 0);
}

BOOST_AUTO_TEST_CASE(custom_failures_and_unknown_opaque_values_have_fixed_markers) {
   using namespace forge_diagnostic_tests;
   BOOST_TEST(forge::variant_schema::encode_diagnostic(failed_custom{"unexposed"}).as_string() == "<diagnostic-error>");
   BOOST_TEST(forge::variant_schema::encode_diagnostic(opaque{1}).as_string() == "<unsupported>");
   BOOST_TEST(forge::variant_schema::encode_diagnostic(bitfields{1}).as_string() == "<unsupported>");
   BOOST_TEST(forge::variant_schema::encode_diagnostic(inherited{{1}}).as_string() == "<unsupported>");
   auto value = 1;
   BOOST_TEST(forge::variant_schema::encode_diagnostic(referenced{value}).as_string() == "<unsupported>");
}

BOOST_AUTO_TEST_CASE(neutral_secret_customization_runs_before_custom_conversion) {
   forge_diagnostic_tests::secret_serializer_calls = 0;
   const auto output = forge::variant_schema::encode_diagnostic(forge_diagnostic_tests::sealed{"unexposed"});
   BOOST_TEST(output.as_string() == "<redacted>");
   BOOST_TEST(forge_diagnostic_tests::secret_serializer_calls == 0);
   BOOST_TEST(forge::variant_schema::encode_diagnostic(std::string{"ordinary token string"}).as_string() ==
              "ordinary token string");
}

BOOST_AUTO_TEST_CASE(diagnostic_binary_values_reuse_existing_variant_conversion) {
   const auto bytes = std::vector<std::uint8_t>{1, 2, 3};
   BOOST_CHECK(forge::variant_schema::encode_diagnostic(bytes) == forge::variant{bytes});
   const auto characters = std::vector<char>{'a', 'b'};
   BOOST_CHECK(forge::variant_schema::encode_diagnostic(characters) == forge::variant{characters});
   const auto binary = forge::blob{{1, 2}};
   auto converted = forge::variant{};
   forge::to_variant(binary, converted);
   BOOST_CHECK(forge::variant_schema::encode_diagnostic(binary) == converted);
   const auto object = forge::variant{forge::mutable_variant_object{}("payload", forge::blob{{1, 2}})};
   BOOST_CHECK(forge::variant_schema::encode_diagnostic(object) == object);
}

BOOST_AUTO_TEST_CASE(diagnostic_wide_integers_reuse_full_precision_variant_conversion) {
   const auto unsigned_value = (static_cast<unsigned __int128>(1) << 100) + 7;
   const auto signed_value = -static_cast<__int128>(unsigned_value);
   BOOST_CHECK(forge::variant_schema::encode_diagnostic(unsigned_value) == forge::variant{unsigned_value});
   BOOST_CHECK(forge::variant_schema::encode_diagnostic(signed_value) == forge::variant{signed_value});
}

BOOST_AUTO_TEST_CASE(diagnostic_long_double_uses_the_existing_double_value_storage) {
   const auto input = 1.0L + std::numeric_limits<long double>::epsilon();
   auto converted = forge::variant{};
   forge::to_variant(input, converted);
   BOOST_REQUIRE(converted.is_double());
   BOOST_TEST(converted.as_double() == static_cast<double>(input));
   BOOST_CHECK(forge::variant_schema::encode_diagnostic(input) == converted);
}

BOOST_AUTO_TEST_CASE(recursive_shared_pointer_has_a_bounded_diagnostic_walk) {
   auto input = std::make_shared<forge_diagnostic_tests::recursive>();
   input->label = "visible";
   input->next = input;
   const auto output = forge::variant_schema::encode_diagnostic(input);
   input->next.reset();
   const auto* current = &output;
   for (auto depth = 0U; depth < 64U && current->is_object(); ++depth) {
      current = &(*current)["next"];
   }
   BOOST_REQUIRE(current->is_string());
   BOOST_TEST(current->as_string() == "<diagnostic-depth-limit>");
}

BOOST_AUTO_TEST_CASE(branching_shared_pointer_cycle_stops_each_branch_on_the_current_path) {
   auto input = std::make_shared<forge_diagnostic_tests::branching>();
   input->label = "visible";
   input->first = input;
   input->second = input;
   const auto output = forge::variant_schema::encode_diagnostic(input);
   input->first.reset();
   input->second.reset();
   BOOST_REQUIRE(output.is_object());
   BOOST_TEST(output["first"].as_string() == "<diagnostic-depth-limit>");
   BOOST_TEST(output["second"].as_string() == "<diagnostic-depth-limit>");
   BOOST_TEST(output["label"].as_string() == "visible");
}

BOOST_AUTO_TEST_CASE(shared_acyclic_children_are_encoded_in_each_branch) {
   auto child = std::make_shared<forge_diagnostic_tests::branching>();
   child->label = "shared child";
   auto input = std::make_shared<forge_diagnostic_tests::branching>();
   input->first = child;
   input->second = child;
   input->label = "parent";
   const auto output = forge::variant_schema::encode_diagnostic(input);
   BOOST_REQUIRE(output.is_object());
   BOOST_TEST(output["first"]["label"].as_string() == "shared child");
   BOOST_TEST(output["second"]["label"].as_string() == "shared child");
   BOOST_CHECK(output == forge::variant_schema::encode(input));
}

BOOST_AUTO_TEST_CASE(aliasing_pointer_identity_includes_the_pointed_to_type) {
   auto input = std::make_shared<forge_diagnostic_tests::alias_outer>();
   input->first.value = 7;
   input->alias = std::shared_ptr<forge_diagnostic_tests::alias_inner>{input, &input->first};
   BOOST_CHECK(static_cast<const void*>(input.get()) == static_cast<const void*>(input->alias.get()));
   const auto output = forge::variant_schema::encode_diagnostic(input);
   input->alias.reset();
   BOOST_REQUIRE(output.is_object());
   BOOST_TEST(output["first"]["value"].as_int64() == 7);
   BOOST_TEST(output["alias"]["value"].as_int64() == 7);
}

BOOST_AUTO_TEST_CASE(traversal_budget_blocks_custom_conversion_and_stops_remaining_container_entries) {
   using namespace forge_diagnostic_tests;
   auto input = std::make_shared<budgeted_node>();
   for (auto layer = 0U; layer < 24U; ++layer) {
      auto parent = std::make_shared<budgeted_node>();
      parent->first = input;
      parent->second = input;
      input = std::move(parent);
   }
   custom_serializer_calls = 0;
   const auto output = forge::variant_schema::encode_diagnostic(input);
   BOOST_TEST(output.as_string() == "<diagnostic-depth-limit>");
   BOOST_TEST(custom_serializer_calls == 0);
   const auto container =
       forge::variant_schema::encode_diagnostic(std::vector<std::shared_ptr<budgeted_node>>{input, input});
   BOOST_REQUIRE(container.is_array());
   BOOST_REQUIRE(container.get_array().size() == 1U);
   BOOST_TEST(container[std::size_t{0}].as_string() == "<diagnostic-depth-limit>");
   BOOST_TEST(custom_serializer_calls == 0);
   BOOST_TEST(forge::variant_schema::encode(input).as_string() == "ordinary-dag");
   BOOST_TEST(custom_serializer_calls == 1);
}

BOOST_AUTO_TEST_SUITE_END()
