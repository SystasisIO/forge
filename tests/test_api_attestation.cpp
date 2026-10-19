#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>

import forge.api.auth.authenticated_caller;
import forge.api.core.binding;
import forge.api.core.connection;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.exceptions;
import forge.api.core.registry;
import forge.api.core.trusted_invocation;
import forge.auth.api.attestation_signer;
import forge.auth.api.exceptions;
import forge.auth.attestation.document_schema;
import forge.crypto.digest.sha256;
import forge.raw.raw;
import forge.variant.value;

namespace {

namespace core = forge::api::core;
namespace att = forge::auth::attestation;
using signer = forge::auth::api::attestation_signer;
using caller = forge::api::auth::authenticated_caller;
using request_type = att::document<forge::variant_object>;

static_assert(core::local_interface<signer>);
static_assert(core::remote_interface<signer>);
static_assert(core::server_supplied<caller>::required);
static_assert(std::is_same_v<core::method_argument_t<&signer::sign, 0>, request_type>);
static_assert(std::is_same_v<core::method_argument_t<&signer::sign, 1>, caller>);
static_assert(std::is_same_v<core::method_response_t<&signer::sign>, att::signed_document>);

template <typename T> T run(boost::asio::awaitable<T> operation) {
   auto context = boost::asio::io_context{};
   auto result = boost::asio::co_spawn(context, std::move(operation), boost::asio::use_future);
   context.run();
   return result.get();
}

request_type document() {
   return {.iss = "urn:fixture:issuer",
           .aud = "urn:fixture:audience",
           .jti = "g_0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
           .iat = 100,
           .nbf = 100,
           .exp = 160,
           .att = {.v = 1, .purpose = "fixture.read", .schema = "urn:fixture:claims:v1",
                   .claims = forge::mutable_variant_object{}("counter", "18446744073709551615")}};
}

caller principal(const char* identity) {
   return {forge::api::auth::caller_source::tls_certificate, forge::crypto::digest::sha256::hash(std::string{identity})};
}

// A contract mock, not a signer implementation or a crypto qualification fixture.
class recording_service final : public signer {
 public:
   boost::asio::awaitable<att::signed_document> sign(request_type request, caller authenticated) override {
      ++calls;
      last_request = std::move(request);
      last_caller = std::move(authenticated);
      co_return att::signed_document{"fixture.payload.signature"};
   }

   std::size_t calls = 0;
   request_type last_request;
   caller last_caller;
};

core::frame request_frame(caller supplied) {
   return {.kind = core::frame_kind::request,
           .id = {.value = 1},
           .api = signer::ref(),
           .method = "sign",
           .codec = {.value = "forge.raw"},
           .payload = core::pack_body(std::tuple{document(), std::move(supplied)})};
}

class loopback_invoker final : public core::remote_invoker {
 public:
   loopback_invoker(const core::registry& services, core::trusted_invocation trusted)
       : services_{services}, trusted_{std::move(trusted)} {}

   boost::asio::awaitable<core::response> async_call(core::request value) override {
      wire_caller = std::get<1>(core::unpack_body<std::tuple<request_type, caller>>(value.body));
      auto result = co_await services_.dispatch_contextual(
          {.kind = core::frame_kind::request,
           .id = {.value = 1},
           .api = value.api,
           .method = value.method,
           .codec = value.codec,
           .payload = std::move(value.body)}, trusted_);
      auto response = core::response{.api = value.api, .method = value.method, .codec = value.codec};
      if (result.kind == core::frame_kind::error) {
         response.error = core::unpack_body<core::error_payload>(result.payload);
      } else {
         response.body = std::move(result.payload);
      }
      co_return response;
   }

   caller wire_caller;

 private:
   const core::registry& services_;
   core::trusted_invocation trusted_;
};

} // namespace

BOOST_AUTO_TEST_SUITE(api_attestation)

BOOST_AUTO_TEST_CASE(versioned_descriptor_has_only_typed_signing_and_declared_errors) {
   const auto descriptor = signer::describe();
   BOOST_TEST(descriptor.id.value == "forge.auth.api.attestation_signer");
   BOOST_TEST(descriptor.version.major == 1);
   BOOST_TEST(descriptor.version.revision == 0);
   BOOST_REQUIRE_EQUAL(descriptor.methods.size(), 1);
   const auto& method = descriptor.methods.front();
   BOOST_TEST(method.name == "sign");
   BOOST_REQUIRE_EQUAL(method.argument_names.size(), 2);
   BOOST_TEST(method.argument_names[0] == "request");
   BOOST_TEST(method.argument_names[1] == "caller");
   BOOST_CHECK(method.response_type == typeid(att::signed_document));
   BOOST_CHECK(static_cast<bool>(method.request_encoder));
   BOOST_CHECK(static_cast<bool>(method.response_encoder));
   for (const auto name : {"invalid_document", "invalid_profile", "admission_denied", "ambiguous_profile",
                           "key_unavailable", "invalid_backend_signature", "busy", "canceled", "stopped"}) {
      BOOST_CHECK(std::any_of(method.errors.begin(), method.errors.end(),
                              [&](const auto& error) { return error.name == name; }));
   }
}

BOOST_AUTO_TEST_CASE(local_binding_keeps_document_and_replaces_spoofed_caller) {
   auto service = std::make_shared<recording_service>();
   auto registry = core::registry{};
   registry.install<signer>(signer::describe(), service);
   const auto expected = principal("trusted transport");
   const auto trusted = core::trusted_invocation_builder{}.set(expected).build();
   const auto plan = core::binding().serve(registry).build();
   const auto response = run(plan.dispatch_contextual(request_frame(principal("client supplied")), trusted));
   BOOST_REQUIRE(response.kind == core::frame_kind::response);
   BOOST_TEST(service->calls == 1);
   BOOST_CHECK(service->last_request == document());
   BOOST_CHECK(service->last_caller == expected);
   BOOST_CHECK(service->last_caller.transport_authenticated());
   BOOST_TEST(core::unpack_body<att::signed_document>(response.payload).compact() == "fixture.payload.signature");
}

BOOST_AUTO_TEST_CASE(missing_trusted_caller_rejects_before_service_on_both_dispatch_paths) {
   auto service = std::make_shared<recording_service>();
   auto registry = core::registry{};
   registry.install<signer>(signer::describe(), service);
   const auto check = [&](const core::frame& response) {
      BOOST_REQUIRE(response.kind == core::frame_kind::error);
      const auto error = core::unpack_body<core::error_payload>(response.payload);
      BOOST_TEST(error.identity.code == static_cast<std::uint32_t>(core::exceptions::code::server_supplied_unavailable));
      BOOST_TEST(service->calls == 0);
   };
   check(run(registry.dispatch(request_frame(principal("spoof")))));
   check(run(registry.dispatch_contextual(request_frame(principal("spoof")), {})));
}

BOOST_AUTO_TEST_CASE(generated_remote_proxy_clears_client_identity_and_uses_same_contract) {
   auto service = std::make_shared<recording_service>();
   auto registry = core::registry{};
   registry.install<signer>(signer::describe(), service);
   const auto expected = principal("trusted remote");
   auto invoker = std::make_shared<loopback_invoker>(registry, core::trusted_invocation_builder{}.set(expected).build());
   auto proxy = core::proxy<signer>{invoker};
   const auto value = run(proxy.sign(document(), principal("spoof")));
   BOOST_TEST(value.compact() == "fixture.payload.signature");
   BOOST_CHECK(service->last_request == document());
   BOOST_CHECK(service->last_caller == expected);
   BOOST_CHECK(service->last_caller.transport_authenticated());
   BOOST_CHECK(invoker->wire_caller == caller{});
   BOOST_CHECK(!invoker->wire_caller.transport_authenticated());
}

BOOST_AUTO_TEST_CASE(typed_api_error_projection_and_restoration_use_existing_machinery) {
   const auto descriptor = signer::describe();
   const auto& method = descriptor.methods.front();
   try {
      FORGE_THROW_EXCEPTION(forge::auth::api::exceptions::busy, "attestation signer is busy");
   } catch (const forge::auth::api::exceptions::busy& error) {
      const auto projected = core::project_error(method, error);
      BOOST_TEST(projected.error == "busy");
      BOOST_CHECK(projected.status_code == core::status::resource_exhausted);
      BOOST_CHECK(projected.retryable);
      BOOST_CHECK_THROW(core::raise_remote_error(projected, &method), forge::auth::api::exceptions::busy);
   }
}

BOOST_AUTO_TEST_SUITE_END()
