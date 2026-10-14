#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

import forge.api.wasm.codec;
import forge.raw.exceptions;

namespace {

namespace protocol = forge::api::wasm;
namespace errors = forge::api::wasm::exceptions;
using forge::raw::bytes;
using presence = protocol::capability::body_presence;

struct encode_probe {
   unsigned* pack_calls;
};

template <typename Stream> void raw_pack(Stream& stream, const encode_probe& value) {
   ++*value.pack_calls;
   forge::raw::pack(stream, std::uint64_t{1});
}

protocol::capability catalog() {
   return {.binding = "catalog.read", .request_type = "http_command", .response_type = "http_response",
           .max_delay_ms = 100,
           .http = protocol::capability::http_policy{
              .request_body = {presence::absent, ""},
              .responses = {{200, {presence::required, "catalog_item"}},
                            {202, {presence::required, "pending_job"}},
                            {204, {presence::absent, ""}},
                            {404, {presence::required, "catalog_not_found"}},
                            {429, {presence::status_only, ""}},
                            {206, {presence::optional, "catalog_item"}}}}};
}

protocol::effect request() {
   return {.binding = "catalog.read",
           .request = protocol::encode("http_command",
                                      protocol::http_command{forge::net::http::method::get, "/items", {}}),
           .response_type = "http_response", .after_ms = 100};
}

protocol::event response(std::uint16_t status, std::optional<protocol::payload> body = {}) {
   return {protocol::encode("http_response", protocol::http_response{status, std::move(body)})};
}

} // namespace

BOOST_AUTO_TEST_SUITE(wasm_protocol_validation)

BOOST_AUTO_TEST_CASE(structurally_valid_records_remain_plain_values) {
   const auto pending = protocol::awaiting{request(), {7, protocol::encode("cursor", std::uint32_t{3})}};
   BOOST_CHECK_NO_THROW(protocol::validate(protocol::step{pending}));
   BOOST_CHECK_NO_THROW(protocol::validate(protocol::step{protocol::completed{protocol::encode("done", true)}}));
   BOOST_CHECK_NO_THROW(protocol::validate(protocol::context{}));
   BOOST_CHECK_NO_THROW(protocol::validate(protocol::context{protocol::subject_view{"issuer", "subject"},
                                                            {protocol::encode("admission", true)}}));
}

BOOST_AUTO_TEST_CASE(identity_facts_preserve_uri_issuers_and_general_subject_strings) {
   const auto subject = protocol::subject_view{
      "https://issuer.test/realm", "user with spaces/alias:42 \xc3\xa9"};
   BOOST_CHECK_NO_THROW(protocol::validate(subject));
   const auto trusted = protocol::context{subject, {protocol::encode("admission", true)}};
   BOOST_CHECK_NO_THROW(protocol::validate(trusted));
   BOOST_CHECK(protocol::decode<protocol::context>(forge::raw::pack(trusted)) == trusted);
   auto bounds = protocol::limits{};
   bounds.name_bytes = static_cast<std::uint32_t>(subject.subject.size() - 1U);
   BOOST_CHECK_THROW(protocol::validate(subject, bounds), errors::limit_exceeded);
}

BOOST_AUTO_TEST_CASE(encode_serializes_product_values_once_and_checks_result_size) {
   auto calls = 0U;
   const auto value = encode_probe{&calls};
   auto bounds = protocol::limits{};
   bounds.payload_bytes = 7;
   BOOST_CHECK_THROW(protocol::encode("probe", value, bounds), errors::limit_exceeded);
   BOOST_CHECK_EQUAL(calls, 1U);
   calls = 0;
   bounds.payload_bytes = 8;
   const auto encoded = protocol::encode("probe", value, bounds);
   BOOST_CHECK_EQUAL(encoded.data.size(), 8U);
   BOOST_CHECK_EQUAL(calls, 1U);
}

BOOST_AUTO_TEST_CASE(type_names_are_bounded_nonempty_tokens_and_match_exactly) {
   BOOST_CHECK_THROW(protocol::validate(protocol::payload{"", {}}, "item"), errors::invalid_value);
   BOOST_CHECK_THROW(protocol::validate(protocol::payload{"bad name", {}}, "bad name"), errors::invalid_value);
   BOOST_CHECK_THROW(protocol::validate(protocol::payload{std::string{"a\0b", 3}, {}}, "a"), errors::invalid_value);
   auto bounds = protocol::limits{};
   bounds.name_bytes = 3;
   BOOST_CHECK_THROW(protocol::validate(protocol::payload{"four", {}}, "four", bounds), errors::limit_exceeded);
   BOOST_CHECK_THROW(protocol::validate(protocol::payload{"item", {}}, "other"), errors::wrong_type);
   BOOST_CHECK_THROW(protocol::decode<std::uint32_t>(protocol::encode("item", std::uint32_t{1}), "Item"),
                     errors::wrong_type);
   BOOST_CHECK_THROW(protocol::encode("", std::uint32_t{1}), errors::invalid_value);
}

BOOST_AUTO_TEST_CASE(payload_and_continuation_bounds_have_inclusive_edges) {
   auto bounds = protocol::limits{};
   bounds.payload_bytes = 4;
   BOOST_CHECK_NO_THROW(protocol::validate(protocol::payload{"p", bytes(4)}, bounds));
   BOOST_CHECK_THROW(protocol::validate(protocol::payload{"p", bytes(5)}, bounds), errors::limit_exceeded);
   BOOST_CHECK_THROW(protocol::encode("p", std::uint64_t{1}, bounds), errors::limit_exceeded);
   const auto state = protocol::continuation{7, {"s", bytes(4)}};
   bounds.continuation_bytes = static_cast<std::uint32_t>(forge::raw::pack_size(state));
   BOOST_CHECK_NO_THROW(protocol::validate(state, bounds));
   --bounds.continuation_bytes;
   BOOST_CHECK_THROW(protocol::validate(state, bounds), errors::limit_exceeded);
   BOOST_CHECK_THROW(protocol::validate(state, 8, "s"), errors::unsupported_version);
   BOOST_CHECK_THROW(protocol::validate(state, 7, "other"), errors::wrong_type);
}

BOOST_AUTO_TEST_CASE(raw_malformed_and_trailing_errors_keep_the_existing_typed_family) {
   BOOST_CHECK_THROW(protocol::decode<protocol::step>(bytes{0x03}), forge::raw::exceptions::codec_error);
   BOOST_CHECK_THROW(protocol::decode<protocol::step>(bytes{0x00}), forge::raw::exceptions::range_error);
   BOOST_CHECK_THROW(protocol::decode<protocol::context>(bytes{0x02, 0x00}), forge::raw::exceptions::codec_error);
   BOOST_CHECK_THROW(protocol::decode<protocol::context>(bytes{0x00, 0x00, 0x00}),
                     forge::raw::exceptions::codec_error);
   BOOST_CHECK_THROW(protocol::decode<std::uint32_t>(protocol::payload{"n", {0x01}}, "n"),
                     forge::raw::exceptions::range_error);
   BOOST_CHECK_THROW(protocol::decode<std::uint32_t>(protocol::payload{"n", {0x01, 0x00, 0x00, 0x00, 0x00}}, "n"),
                     forge::raw::exceptions::codec_error);
}

BOOST_AUTO_TEST_CASE(raw_decode_limits_bound_all_input_not_just_individual_fields) {
   const auto value = protocol::context{{}, {{"a", bytes(4)}, {"b", bytes(4)}}};
   const auto encoded = forge::raw::pack(value);
   auto bounds = protocol::limits{};
   bounds.wire.max_bytes = static_cast<std::uint32_t>(encoded.size());
   BOOST_CHECK(protocol::decode<protocol::context>(encoded, bounds) == value);
   --bounds.wire.max_bytes;
   BOOST_CHECK_THROW(protocol::decode<protocol::context>(encoded, bounds), forge::raw::exceptions::allocation_limit);
   BOOST_CHECK_THROW(protocol::validate(value, bounds), errors::limit_exceeded);
}

BOOST_AUTO_TEST_CASE(raw_allocation_limits_are_passed_without_a_parallel_limits_model) {
   auto bounds = protocol::limits{};
   bounds.wire.max_container_elements = 1;
   const auto values = protocol::context{{}, {{"a", {}}, {"b", {}}}};
   BOOST_CHECK_THROW(protocol::decode<protocol::context>(forge::raw::pack(values), bounds),
                     forge::raw::exceptions::allocation_limit);
   bounds = {};
   bounds.wire.max_total_container_elements = 1;
   BOOST_CHECK_THROW(protocol::decode<protocol::context>(forge::raw::pack(values), bounds),
                     forge::raw::exceptions::allocation_limit);
   bounds = {};
   bounds.wire.first_container_elements = 1;
   BOOST_CHECK_THROW(protocol::decode<protocol::context>(forge::raw::pack(values), bounds),
                     forge::raw::exceptions::allocation_limit);
   bounds = {};
   bounds.wire.max_bytes = 2;
   BOOST_CHECK_THROW(protocol::decode<std::string>(protocol::payload{"s", {0x03, 0x61, 0x62, 0x63}}, "s", bounds),
                     forge::raw::exceptions::allocation_limit);
}

BOOST_AUTO_TEST_CASE(host_selected_binding_request_full_response_and_delay_cannot_escalate) {
   const auto allowed = catalog();
   auto value = request();
   BOOST_CHECK_NO_THROW(protocol::validate(value, allowed));
   value.binding = "unknown";
   BOOST_CHECK_THROW(protocol::validate(value, allowed), errors::forbidden_effect);
   value = request();
   value.request.type = "arbitrary_request";
   BOOST_CHECK_THROW(protocol::validate(value, allowed), errors::wrong_type);
   value = request();
   value.response_type = "catalog_item";
   BOOST_CHECK_THROW(protocol::validate(value, allowed), errors::wrong_type);
   value = request();
   ++value.after_ms;
   BOOST_CHECK_THROW(protocol::validate(value, allowed), errors::limit_exceeded);
   value.after_ms = std::numeric_limits<std::uint32_t>::max();
   BOOST_CHECK_THROW(protocol::validate(value, allowed), errors::limit_exceeded);
   value = request();
   value.after_ms = 0;
   BOOST_CHECK_NO_THROW(protocol::validate(value, allowed));
}

BOOST_AUTO_TEST_CASE(capability_sets_reject_unknown_and_ambiguous_bindings) {
   const auto pending = request();
   const auto allowed = std::vector{catalog()};
   BOOST_CHECK_NO_THROW(protocol::validate(pending, std::span<const protocol::capability>{allowed}));
   BOOST_CHECK_THROW(protocol::validate(pending, std::span<const protocol::capability>{}), errors::forbidden_effect);
   const auto duplicate = std::vector{catalog(), catalog()};
   BOOST_CHECK_THROW(protocol::validate(pending, std::span<const protocol::capability>{duplicate}),
                     errors::invalid_capability);
   auto invalid = catalog();
   invalid.http->responses.push_back(invalid.http->responses.front());
   BOOST_CHECK_THROW(protocol::validate(invalid), errors::invalid_capability);
   invalid = catalog();
   invalid.http->responses.front().body.type.clear();
   BOOST_CHECK_THROW(protocol::validate(invalid), errors::invalid_capability);
   invalid = catalog();
   invalid.http->responses.front().body.presence = static_cast<presence>(255);
   BOOST_CHECK_THROW(protocol::validate(invalid), errors::invalid_capability);
}

BOOST_AUTO_TEST_CASE(generic_effects_and_capability_set_overloads_do_not_require_http) {
   const auto allowed = std::vector{protocol::capability{
      .binding = "catalog.lookup", .request_type = "query", .response_type = "answer"}};
   const auto capabilities = std::span<const protocol::capability>{allowed};
   const auto pending = protocol::effect{
      "catalog.lookup", protocol::encode("query", std::uint32_t{1}), "answer", 0};
   const auto waiting = protocol::step{protocol::awaiting{pending, {9, {"cursor", {0x01}}}}};
   BOOST_CHECK_NO_THROW(protocol::validate(waiting, capabilities));
   BOOST_CHECK_THROW(protocol::validate(waiting, std::span<const protocol::capability>{}), errors::forbidden_effect);
   const auto result = protocol::event{protocol::encode("answer", std::uint32_t{2})};
   BOOST_CHECK_NO_THROW(protocol::validate(result, pending, capabilities));
   const auto wrong = protocol::event{protocol::encode("query", std::uint32_t{2})};
   BOOST_CHECK_THROW(protocol::validate(wrong, pending, capabilities), errors::wrong_type);
   BOOST_CHECK_NO_THROW(protocol::validate(protocol::step{protocol::completed{{"answer", {0x02}}}}, capabilities));
}

BOOST_AUTO_TEST_CASE(http_success_event_is_full_result_not_body_schema) {
   const auto allowed = catalog();
   const auto pending = request();
   BOOST_CHECK_NO_THROW(protocol::validate(response(200, protocol::payload{"catalog_item", {1}}), pending, allowed));
   const auto wrong_full = protocol::event{protocol::payload{"catalog_item", {1}}};
   BOOST_CHECK_THROW(protocol::validate(wrong_full, pending, allowed), errors::wrong_type);
   BOOST_CHECK_THROW(protocol::validate(response(200, protocol::payload{"http_response", {1}}), pending, allowed),
                     errors::wrong_type);
   BOOST_CHECK_THROW(protocol::validate(response(202, protocol::payload{"catalog_item", {1}}), pending, allowed),
                     errors::wrong_type);
   BOOST_CHECK_NO_THROW(protocol::validate(response(202, protocol::payload{"pending_job", {1}}), pending, allowed));
   BOOST_CHECK_NO_THROW(protocol::validate(response(404, protocol::payload{"catalog_not_found", {1}}), pending, allowed));
}

BOOST_AUTO_TEST_CASE(http_body_presence_and_status_only_rules_are_explicit) {
   const auto allowed = catalog();
   const auto pending = request();
   BOOST_CHECK_THROW(protocol::validate(response(200), pending, allowed), errors::invalid_http_response);
   BOOST_CHECK_NO_THROW(protocol::validate(response(204), pending, allowed));
   BOOST_CHECK_THROW(protocol::validate(response(204, protocol::payload{"catalog_item", {}}), pending, allowed),
                     errors::invalid_http_response);
   BOOST_CHECK_NO_THROW(protocol::validate(response(429), pending, allowed));
   BOOST_CHECK_THROW(protocol::validate(response(429, protocol::payload{"catalog_item", {}}), pending, allowed),
                     errors::invalid_http_response);
   BOOST_CHECK_NO_THROW(protocol::validate(response(206), pending, allowed));
   BOOST_CHECK_NO_THROW(protocol::validate(response(206, protocol::payload{"catalog_item", {}}), pending, allowed));
   BOOST_CHECK_THROW(protocol::validate(response(500), pending, allowed), errors::invalid_http_response);
   BOOST_CHECK_THROW(protocol::validate(protocol::http_response{99, {}}, *allowed.http), errors::invalid_http_response);
}

BOOST_AUTO_TEST_CASE(typed_null_is_not_absence_and_request_body_is_pinned) {
   auto allowed = catalog();
   allowed.http->responses.front().body.type = "nullable_item";
   const auto null_value = protocol::encode("nullable_item", std::optional<std::uint32_t>{});
   BOOST_REQUIRE(null_value.data == bytes{0x00});
   BOOST_CHECK_NO_THROW(protocol::validate(response(200, null_value), request(), allowed));
   BOOST_CHECK_THROW(protocol::validate(response(200), request(), allowed), errors::invalid_http_response);
   auto command = protocol::http_command{forge::net::http::method::post, "/items", null_value};
   BOOST_CHECK_THROW(protocol::validate(command, *allowed.http), errors::forbidden_effect);
   allowed.http->request_body = {presence::required, "nullable_item"};
   BOOST_CHECK_NO_THROW(protocol::validate(command, *allowed.http));
   command.body->type = "other";
   BOOST_CHECK_THROW(protocol::validate(command, *allowed.http), errors::wrong_type);
}

BOOST_AUTO_TEST_CASE(canonical_error_branch_does_not_impersonate_success_or_authorize_a_retry) {
   const auto problem = forge::api::core::error_payload{
      .error = "unavailable", .message = "bounded", .retryable = true,
      .status_code = forge::api::core::status::unavailable};
   BOOST_CHECK_NO_THROW(protocol::validate(protocol::event{problem}, request(), catalog()));
   BOOST_CHECK_NO_THROW(protocol::validate(protocol::step{problem}));
   auto forged = protocol::event{protocol::payload{"error_payload", forge::raw::pack(problem)}};
   BOOST_CHECK_THROW(protocol::validate(forged, request(), catalog()), errors::wrong_type);
   auto invalid_pending = request();
   invalid_pending.binding = "unknown";
   BOOST_CHECK_THROW(protocol::validate(protocol::event{problem}, invalid_pending, catalog()), errors::forbidden_effect);
}

BOOST_AUTO_TEST_CASE(http_intent_is_bounded_but_is_not_url_authorization) {
   BOOST_CHECK_NO_THROW(protocol::validate(protocol::http_command{forge::net::http::method::get, "/items?q=1", {}}));
   for (const auto target : {"", "https://other.test/x", "//other.test/x", "/x\r\nInjected: yes"}) {
      BOOST_CHECK_THROW(protocol::validate(protocol::http_command{forge::net::http::method::get, target, {}}),
                        errors::invalid_value);
   }
   BOOST_CHECK_THROW(protocol::validate(protocol::http_command{forge::net::http::method::unknown, "/x", {}}),
                     errors::invalid_value);
   auto bounds = protocol::limits{};
   bounds.target_bytes = 2;
   BOOST_CHECK_NO_THROW(protocol::validate(protocol::http_command{forge::net::http::method::get, "/x", {}}, bounds));
   BOOST_CHECK_THROW(protocol::validate(protocol::http_command{forge::net::http::method::get, "/xx", {}}, bounds),
                     errors::limit_exceeded);
}

BOOST_AUTO_TEST_CASE(validation_diagnostics_do_not_echo_untrusted_names_or_bytes) {
   const auto secret = std::string{"PRIVATE-TOKEN-DO-NOT-ECHO"};
   try {
      protocol::validate(protocol::payload{secret, bytes{0xde, 0xad}}, "expected");
      BOOST_FAIL("Expected a typed schema mismatch");
   } catch (const errors::wrong_type& error) {
      BOOST_CHECK(std::string_view{error.what()}.find(secret) == std::string_view::npos);
   }
}

BOOST_AUTO_TEST_SUITE_END()
