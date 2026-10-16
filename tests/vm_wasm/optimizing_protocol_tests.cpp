#include "test_prelude.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

import forge.core.version;
import forge.core.git_revision;
import forge.raw.raw;
import forge.vm.wasm.optimizing.compiler_protocol;

namespace {
namespace oc = forge::vm::wasm::optimizing;
namespace protocol = oc::compiler_protocol;

protocol::hello hello(bool darwin) {
   auto value = protocol::hello{};
   value.forge_version = {forge::core::version_major, forge::core::version_minor, forge::core::version_patch};
   value.build_revision = forge::git_revision_sha;
   value.build_timestamp = forge::git_revision_unix_timestamp;
   value.limits = {2, 2ULL * 1024 * 1024 * 1024, 8ULL * 1024 * 1024};
   if (darwin) {
      value.target.triple = "aarch64-apple-macosx11.0.0-elf";
      value.target.cpu = "generic";
      value.target.features = "+fp-armv8,+neon";
   }
   return value;
}
} // namespace

// Pure SAME Raw codec controls only. These do not establish process order,
// actual LLVM capabilities, resource limits or prepared-artifact acceptance.
BOOST_AUTO_TEST_SUITE(optimizing_protocol)

BOOST_AUTO_TEST_CASE(same_raw_header_roundtrip_and_ack_is_the_only_empty_body) {
   for (const auto kind :
        {protocol::message_kind::hello, protocol::message_kind::request, protocol::message_kind::reply,
         protocol::message_kind::hello_ack, protocol::message_kind::preflight}) {
      auto value = protocol::header{};
      value.kind = kind;
      value.exchange = {1, 2, 1};
      value.body_bytes = kind == protocol::message_kind::hello_ack ? 0 : 1;
      const auto encoded = protocol::encode(value);
      BOOST_TEST(encoded == forge::raw::pack(value));
      BOOST_TEST(encoded.size() == protocol::header_size());
      const auto decoded = protocol::decode_header(encoded);
      BOOST_CHECK(decoded.kind == kind);
      BOOST_CHECK(decoded.exchange == value.exchange);
      BOOST_TEST(decoded.body_bytes == value.body_bytes);
      if (kind != protocol::message_kind::hello_ack) {
         value.body_bytes = 0;
         BOOST_CHECK_THROW(protocol::encode(value), oc::exceptions::compiler_unavailable);
      } else {
         value.body_bytes = 1;
         BOOST_CHECK_THROW(protocol::decode_header(forge::raw::pack(value)), oc::exceptions::compiler_unavailable);
      }
      if (kind == protocol::message_kind::preflight || kind == protocol::message_kind::hello) {
         value.body_bytes = 1;
         value.identity = forge::crypto::digest::sha256::hash("identity", 8);
         BOOST_CHECK_THROW(protocol::encode(value), oc::exceptions::compiler_unavailable);
      }
   }
}

BOOST_AUTO_TEST_CASE(header_version_generation_and_exact_frame_fail_closed) {
   auto valid = protocol::header{};
   valid.kind = protocol::message_kind::request;
   valid.exchange = {1, 2, 1};
   valid.body_bytes = 1;
   for (unsigned mode = 0; mode < 7; ++mode) {
      auto bad = valid;
      if (mode == 0)
         bad.magic = 0;
      if (mode == 1)
         bad.version = 0;
      if (mode == 2)
         bad.exchange.session = 0;
      if (mode == 3)
         bad.exchange.job = 0;
      if (mode == 4)
         bad.exchange.attempt = 0;
      if (mode == 5)
         bad.body_bytes = forge::raw::max_byte_array_size + std::uint64_t{1};
      if (mode == 6)
         bad.kind = static_cast<protocol::message_kind>(255);
      BOOST_CHECK_THROW(protocol::decode_header(forge::raw::pack(bad)), oc::exceptions::compiler_unavailable);
   }
   auto encoded = protocol::encode(valid);
   encoded.pop_back();
   BOOST_CHECK_THROW(protocol::decode_header(encoded), oc::exceptions::compiler_unavailable);
   encoded = protocol::encode(valid);
   encoded.push_back(0);
   BOOST_CHECK_THROW(protocol::decode_header(encoded), oc::exceptions::compiler_unavailable);
}

BOOST_AUTO_TEST_CASE(hello_uses_same_target_versions_build_and_resource_records) {
   for (const auto darwin : {false, true}) {
      const auto valid = hello(darwin);
      const auto encoded = protocol::encode(valid);
      BOOST_TEST(encoded == forge::raw::pack(valid));
      BOOST_CHECK(protocol::decode_hello(encoded, encoded.size()) == valid);
      for (unsigned mode = 0; mode < 7; ++mode) {
         auto bad = valid;
         if (mode == 0)
            bad.target.codegen_version = 2;
         if (mode == 1)
            bad.target.host_abi_version = 1;
         if (mode == 2)
            bad.artifact_version = 0;
         if (mode == 3)
            bad.configuration_version = 0;
         if (mode == 4)
            bad.limits.address_space_bytes = 0;
         if (mode == 5)
            bad.build_revision = "not-a-build-revision";
         if (mode == 6)
            bad.target.cpu = "incompatible";
         const auto raw = forge::raw::pack(bad);
         BOOST_CHECK_THROW(protocol::decode_hello(raw, raw.size()), oc::exceptions::compiler_unavailable);
      }
   }
}

BOOST_AUTO_TEST_CASE(preflight_uses_same_target_raw_and_rejects_stale_profiles) {
   const auto target = oc::target_profile::native();
   auto configuration = oc::compile_configuration{};
   configuration.target = target;
   BOOST_CHECK(oc::canonicalize(configuration).target == target);
   BOOST_TEST(target.matches_host());
   const auto encoded = protocol::encode(target);
   BOOST_TEST(encoded == forge::raw::pack(target));
   BOOST_CHECK(protocol::decode_target(encoded, encoded.size()) == target);
   for (unsigned mode = 0; mode < 4; ++mode) {
      auto bad = target;
      if (mode == 0)
         bad.cpu = "not-canonical";
      if (mode == 1)
         bad.features += ",unknown";
      if (mode == 2)
         bad.codegen_version = 2;
      if (mode == 3)
         bad.host_abi_version = 1;
      BOOST_TEST(!bad.matches_host());
      const auto bytes = forge::raw::pack(bad);
      BOOST_CHECK_THROW(protocol::decode_target(bytes, bytes.size()), oc::exceptions::compiler_unavailable);
   }
   auto trailing = encoded;
   trailing.push_back(0);
   BOOST_CHECK_THROW(protocol::decode_target(trailing, trailing.size()), oc::exceptions::compiler_unavailable);
}

BOOST_AUTO_TEST_CASE(reply_keeps_compiler_failure_closed_and_image_validation_separate) {
   for (const auto failure : {oc::exceptions::code::invalid_module, oc::exceptions::code::compile_limit,
                              oc::exceptions::code::compiler_unavailable, oc::exceptions::code::cache_corruption}) {
      auto value = protocol::reply{failure, {}};
      const auto encoded = protocol::encode(value);
      const auto decoded = protocol::decode_reply(encoded, encoded.size());
      BOOST_REQUIRE(decoded.failure.has_value());
      BOOST_CHECK(*decoded.failure == failure);
      BOOST_TEST(decoded.image.empty());
      value.image = {1};
      BOOST_CHECK_THROW(protocol::decode_reply(forge::raw::pack(value), 128), oc::exceptions::compiler_unavailable);
   }
   for (const auto failure : {oc::exceptions::code::shutdown_incomplete, static_cast<oc::exceptions::code>(0)}) {
      const auto raw = forge::raw::pack(protocol::reply{failure, {}});
      BOOST_CHECK_THROW(protocol::decode_reply(raw, raw.size()), oc::exceptions::compiler_unavailable);
   }
   BOOST_CHECK_THROW(protocol::encode(protocol::reply{}), oc::exceptions::compiler_unavailable);
   // The transport does not pretend to validate native code. The session must
   // still use artifact::decode/checked identity/fullconfiguration/prepare.
   const auto bytes = protocol::encode(protocol::reply{{}, {1}});
   BOOST_TEST(protocol::decode_reply(bytes, bytes.size()).image == std::vector<std::uint8_t>{1});
}

BOOST_AUTO_TEST_SUITE_END()
