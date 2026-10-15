#include "secrets_faults.hxx"

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/exceptions/macros.hpp>

#include <array>
#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <functional>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <thread>
#include <tuple>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

import forge.api.core.registry;
import forge.api.core.exceptions;
import forge.api.core.types;
import forge.app.application_builder;
import forge.app.application_shell;
import forge.app.plugin;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.config.core.component;
import forge.config.core.document;
import forge.config.core.value;
import forge.codec.json;
import forge.crypto.core.secret_bytes;
import forge.crypto.core.types;
import forge.crypto.core.secret_string;
import forge.crypto.keystore.encrypted_file;
import forge.exceptions;
import forge.plugins.crypto.secrets.api;
import forge.plugins.crypto.secrets.exceptions;
import forge.plugins.crypto.secrets.plugin;
import forge.plugins.crypto.secrets.version_api;
import forge.plugins.crypto.secrets.version_metadata_api;
import forge.plugins.crypto.secrets.version_types;
import forge.plugins.crypto.secrets.version_write_api;
import forge.raw.raw;

static_assert(!std::is_copy_constructible_v<forge::plugins::crypto::secrets::create_version_request>);
static_assert(std::is_move_constructible_v<forge::plugins::crypto::secrets::create_version_request>);
static_assert(forge::api::core::local_interface<forge::plugins::crypto::secrets::version_write_api>);
static_assert(!forge::api::core::remote_interface<forge::plugins::crypto::secrets::version_write_api>);
static_assert(forge::api::core::local_interface<forge::plugins::crypto::secrets::version_metadata_api>);
static_assert(!forge::api::core::remote_interface<forge::plugins::crypto::secrets::version_metadata_api>);
static_assert(forge::api::core::local_interface<forge::plugins::crypto::secrets::version_api>);
static_assert(!forge::api::core::remote_interface<forge::plugins::crypto::secrets::version_api>);

namespace {

struct managed_fixture {
   managed_fixture() {
      auto pattern = (std::filesystem::temp_directory_path() / "forge-secret-versions-XXXXXX").string();
      const auto* created = ::mkdtemp(pattern.data());
      if (!created) {
         FORGE_THROW_EXCEPTION(forge::exceptions::context_error, "Cannot create managed secrets test directory");
      }

      directory = created;
      std::filesystem::create_directory(directory / "store");
      std::filesystem::permissions(directory / "store", std::filesystem::perms::owner_all,
                                   std::filesystem::perm_options::replace);
      auto output = std::ofstream{directory / "bootstrap", std::ios::binary};
      output << "version-store-test-bootstrap\n";
      output.close();
      std::filesystem::permissions(directory / "bootstrap",
                                   std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                   std::filesystem::perm_options::replace);

      document.set("plugins.crypto.secrets.versions.enabled", true);
      document.set("plugins.crypto.secrets.versions.path", store_path().string());
      document.set("plugins.crypto.secrets.versions.bootstrap-passphrase-file", (directory / "bootstrap").string());
      document.set("plugins.crypto.secrets.versions.ceiling.purposes",
                   forge::config::core::value::array_type{forge::config::core::value{"fixture.read"}});
      document.set("plugins.crypto.secrets.versions.ceiling.operations",
                   forge::config::core::value::array_type{forge::config::core::value{"get_bytes"}});
      document.set("plugins.crypto.secrets.versions.ceiling.allow-raw-export", true);
      document.set("plugins.crypto.secrets.versions.ceiling.max-plaintext-bytes", std::uint64_t{4096});
      document.set("plugins.crypto.secrets.versions.ceiling.max-ciphertext-bytes", std::uint64_t{8192});
      document.set("plugins.crypto.secrets.versions.ceiling.max-aad-bytes", std::uint64_t{4096});
   }

   ~managed_fixture() {
      try {
         stop();
      } catch (...) {
         BOOST_ERROR("Managed secrets fixture shutdown failed");
      }

      auto error = std::error_code{};
      std::filesystem::remove_all(directory, error);
   }

   std::filesystem::path store_path() const {
      return directory / "store" / "versions.fks";
   }

   void start(std::string_view mode = "create_new") {
      document.set("plugins.crypto.secrets.versions.mode", std::string{mode});
      auto descriptor = forge::plugins::crypto::secrets::plugin::descriptor();
      descriptor.enabled_by_default = true;
      auto builder = forge::app::application_builder{};
      builder.compute({.worker_threads = 2}).plugin(std::move(descriptor));
      app = std::move(builder).build();
      app->configure(document);
      forge::asio::blocking::run(app->runtime(), app->startup());
   }

   void stop() {
      if (app) {
         app->request_stop();
         forge::asio::blocking::run(app->runtime(), app->shutdown());
         app.reset();
      }
   }

   forge::api::core::handle<forge::plugins::crypto::secrets::version_write_api> writer() const {
      return app->apis().get<forge::plugins::crypto::secrets::version_write_api>(
          forge::plugins::crypto::secrets::version_write_api::ref());
   }

   forge::api::core::handle<forge::plugins::crypto::secrets::version_metadata_api> metadata() const {
      return app->apis().get<forge::plugins::crypto::secrets::version_metadata_api>(
          forge::plugins::crypto::secrets::version_metadata_api::ref());
   }

   forge::api::core::handle<forge::plugins::crypto::secrets::version_api> reader() const {
      return app->apis().get<forge::plugins::crypto::secrets::version_api>(
          forge::plugins::crypto::secrets::version_api::ref());
   }

   std::filesystem::path directory;
   forge::config::core::document document;
   std::unique_ptr<forge::app::application_shell> app;
};

forge::plugins::crypto::secrets::create_version_request create_request(std::string mutation_id, std::uint64_t revision,
                                                                       std::string_view material) {
   return {.mutation_id = std::move(mutation_id),
           .secret_id = "native/http",
           .expected_revision = revision,
           .kind = forge::plugins::crypto::secrets::secret_kind::bytes,
           .policy = {.purposes = {"fixture.read"},
                      .operations = {forge::plugins::crypto::secrets::operation::get_bytes},
                      .allow_raw_export = true},
           .material = forge::crypto::core::secret_bytes{forge::crypto::core::bytes{material.begin(), material.end()}}};
}

forge::plugins::crypto::secrets::version_mutation_result
create_version(managed_fixture& fixture, std::string mutation_id, std::uint64_t revision, std::string_view material) {
   auto write = fixture.writer();
   return forge::asio::blocking::run(fixture.app->runtime(),
                                     write->create_version(create_request(std::move(mutation_id), revision, material)));
}

forge::crypto::core::bytes read_version(managed_fixture& fixture, std::uint64_t version) {
   auto read = fixture.reader();
   return forge::asio::blocking::run(fixture.app->runtime(),
                                     read->get_bytes({.version = version, .request = {"native/http", "fixture.read"}}))
       .result.bytes;
}

std::string read_file(const std::filesystem::path& path) {
   auto input = std::ifstream{path, std::ios::binary};
   return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void write_file(const std::filesystem::path& path, std::string_view bytes) {
   auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
   output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
   output.close();
}

// Persisted v1 wire fixture. Raw tuple codecs preserve field order without
// importing private declarations attached to the plugin's named module.
using stored_head = std::tuple<std::uint64_t, std::uint64_t>;
// Described enums use int64 on the established Raw wire, independently of
// their native underlying type. Keep the fixture wide for narrowing attacks.
using stored_policy =
    std::tuple<std::vector<std::string>, std::vector<std::int64_t>, bool, std::uint64_t, std::uint64_t, std::uint64_t>;
using stored_version =
    std::tuple<forge::plugins::crypto::secrets::version_ref, std::int64_t, std::int64_t, stored_policy>;
using stored_receipt =
    std::tuple<std::int64_t, std::array<std::uint8_t, 32>, forge::plugins::crypto::secrets::version_mutation_result>;
using stored_payload =
    std::tuple<std::string, std::uint32_t, forge::crypto::core::bytes, std::vector<std::pair<std::string, stored_head>>,
               std::vector<stored_version>, std::vector<forge::crypto::core::bytes>,
               std::vector<std::pair<std::string, stored_receipt>>>;

forge::crypto::core::secret_string fixture_password() {
   return forge::crypto::core::secret_string{"version-store-test-bootstrap"};
}

stored_payload decode_fixture_payload(const managed_fixture& fixture) {
   const auto bytes = read_file(fixture.store_path());
   const auto plaintext = forge::crypto::keystore::decrypt_file(forge::crypto::core::bytes{bytes.begin(), bytes.end()},
                                                                fixture_password());
   auto result = stored_payload{};
   forge::raw::unpack_exact(plaintext.span(), result);
   BOOST_REQUIRE(forge::raw::pack(result) == plaintext.copy());
   return result;
}

void encode_fixture_payload(const managed_fixture& fixture, const stored_payload& payload) {
   const auto bytes = forge::crypto::keystore::encrypt_file(
       {.plaintext = forge::crypto::core::secret_bytes{forge::raw::pack(payload)}, .password = fixture_password()});
   write_file(fixture.store_path(), {reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}

struct cancellation_probe_result {
   bool expected_outcome = false;
   bool throw_policy = false;
   bool partial_history = false;
   bool partial_mask = false;
   bool total_mask = false;
};

boost::asio::awaitable<cancellation_probe_result>
native_cancellation_probe(managed_fixture* fixture, boost::asio::cancellation_signal* cancellation, bool create,
                          bool fail, bool total, bool throw_policy, bool prior_history = false) {
   if (total) {
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::enable_total_cancellation{});
   } else {
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::enable_partial_cancellation{});
   }
   co_await boost::asio::this_coro::throw_if_cancelled(throw_policy);
   if (prior_history) {
      cancellation->emit(boost::asio::cancellation_type::partial);
   }
   auto result = cancellation_probe_result{};
   try {
      if (create) {
         auto writer = fixture->writer();
         co_await writer->create_version(create_request("mask-probe", 1, fail ? "" : "mask-material"));
      } else {
         auto reader = fixture->reader();
         co_await reader->get_bytes({.version = fail ? 0U : 1U, .request = {"native/http", "fixture.read"}});
      }
      result.expected_outcome = !fail && !prior_history;
   } catch (const forge::plugins::crypto::secrets::exceptions::invalid_request&) {
      result.expected_outcome = fail && !prior_history;
   } catch (const forge::api::core::exceptions::cancelled&) {
      result.expected_outcome = prior_history;
   }
   result.throw_policy = co_await boost::asio::this_coro::throw_if_cancelled();
   co_await boost::asio::this_coro::throw_if_cancelled(false);
   const auto after_call = co_await boost::asio::this_coro::cancellation_state;
   result.partial_history =
       (after_call.cancelled() & boost::asio::cancellation_type::partial) != boost::asio::cancellation_type::none;
   cancellation->emit(boost::asio::cancellation_type::partial);
   const auto after_partial = co_await boost::asio::this_coro::cancellation_state;
   result.partial_mask =
       (after_partial.cancelled() & boost::asio::cancellation_type::partial) != boost::asio::cancellation_type::none;
   cancellation->emit(boost::asio::cancellation_type::total);
   const auto after_total = co_await boost::asio::this_coro::cancellation_state;
   result.total_mask =
       (after_total.cancelled() & boost::asio::cancellation_type::total) != boost::asio::cancellation_type::none;
   co_return result;
}

struct cancelled_mutation_result {
   forge::plugins::crypto::secrets::version_mutation_result result;
   bool aborted = false;
   bool partial_history = false;
   bool throw_policy = true;
};

boost::asio::awaitable<cancelled_mutation_result> create_with_caller_history(managed_fixture* fixture) {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::enable_total_cancellation{});
   co_await boost::asio::this_coro::throw_if_cancelled(false);
   auto completed = cancelled_mutation_result{};
   try {
      auto writer = fixture->writer();
      completed.result =
          co_await writer->create_version(create_request("stopped-after-rename", 1, "published-material"));
   } catch (const boost::system::system_error& error) {
      if (error.code() != boost::asio::error::operation_aborted) {
         throw;
      }
      completed.aborted = true;
   }
   completed.throw_policy = co_await boost::asio::this_coro::throw_if_cancelled();
   const auto state = co_await boost::asio::this_coro::cancellation_state;
   completed.partial_history =
       (state.cancelled() & boost::asio::cancellation_type::partial) != boost::asio::cancellation_type::none;
   co_return completed;
}

} // namespace

BOOST_AUTO_TEST_SUITE(crypto_secrets_versions_tests)

BOOST_AUTO_TEST_CASE(legacy_request_raw_bytes_and_enum_values_remain_unchanged) {
   namespace secrets = forge::plugins::crypto::secrets;
   constexpr auto numeric_values = std::array{static_cast<int>(secrets::secret_kind::bytes),
                                              static_cast<int>(secrets::secret_kind::symmetric_key),
                                              static_cast<int>(secrets::source_type::value),
                                              static_cast<int>(secrets::source_type::file),
                                              static_cast<int>(secrets::source_type::encrypted_file),
                                              static_cast<int>(secrets::encoding::raw),
                                              static_cast<int>(secrets::encoding::hex),
                                              static_cast<int>(secrets::encoding::base64),
                                              static_cast<int>(secrets::operation::get_bytes),
                                              static_cast<int>(secrets::operation::derive_hkdf_sha256),
                                              static_cast<int>(secrets::operation::encrypt_aes_gcm),
                                              static_cast<int>(secrets::operation::decrypt_aes_gcm)};
   static_assert(numeric_values == std::array{0, 1, 0, 1, 2, 0, 1, 2, 0, 1, 2, 3});
   const auto request = secrets::derive_request{
       .secret_id = "id", .purpose = "p", .salt = {0x11, 0x22}, .info = {0x33}, .output_size = 64};
   const auto golden =
       forge::crypto::core::bytes{2, 'i', 'd', 1, 'p', 2, 0x11, 0x22, 1, 0x33, 0x40, 0, 0, 0, 0, 0, 0, 0};
   BOOST_CHECK(forge::raw::pack(request) == golden);
   auto decoded = secrets::derive_request{};
   forge::raw::unpack_exact(std::span<const std::uint8_t>{golden}, decoded);
   BOOST_TEST(decoded.secret_id == "id");
   BOOST_TEST(decoded.purpose == "p");
   BOOST_CHECK(decoded.salt == request.salt);
   BOOST_CHECK(decoded.info == request.info);
   BOOST_TEST(decoded.output_size == 64U);
}

BOOST_AUTO_TEST_CASE(version_capabilities_are_three_local_contracts_without_wire_codecs) {
   auto runtime = forge::asio::runtime{{.worker_threads = 1}};
   auto plugin = forge::plugins::crypto::secrets::plugin{};
   auto document = forge::config::core::document{};
   forge::asio::blocking::run(
       runtime, plugin.configure(forge::config::core::component_view{document, "plugins.crypto.secrets"}));
   auto registry = forge::api::core::registry{};
   auto installer = forge::api::core::installer{registry};
   forge::asio::blocking::run(runtime, plugin.provide(installer));

   for (const auto id : std::array<std::string_view, 3>{"forge.plugins.crypto.secrets.version_write",
                                                        "forge.plugins.crypto.secrets.version_metadata",
                                                        "forge.plugins.crypto.secrets.version"}) {
      const auto* descriptor = registry.describe({.id = {std::string{id}}, .major = 1});
      BOOST_REQUIRE_MESSAGE(descriptor != nullptr, "Missing versioned local capability: " << id);
      BOOST_TEST(static_cast<std::uint32_t>(descriptor->supported_surfaces) ==
                 static_cast<std::uint32_t>(forge::api::core::surface::local));
      BOOST_TEST(!descriptor->methods.empty());
      for (const auto& method : descriptor->methods) {
         BOOST_CHECK(!method.request_encoder);
         BOOST_CHECK(!method.response_encoder);
         BOOST_CHECK(!method.request_decoder);
         BOOST_CHECK(!method.response_decoder);
         BOOST_CHECK(!method.input_decoder);
         BOOST_CHECK(!method.output_decoder);
         BOOST_CHECK(!method.raw_invoker);
         const auto rejected = forge::asio::blocking::run(
             runtime, registry.dispatch({.api = {.id = {std::string{id}}, .major = 1}, .method = method.name}));
         BOOST_CHECK(rejected.kind == forge::api::core::frame_kind::error);
         auto error = forge::api::core::error_payload{};
         forge::raw::unpack_exact(std::span<const std::uint8_t>{rejected.payload}, error);
         BOOST_CHECK(error.status_code == forge::api::core::status::failed_precondition);
         BOOST_TEST(error.error == "protocol_error");
      }
   }
}

BOOST_AUTO_TEST_CASE(move_only_material_is_created_and_read_through_real_local_handles) {
   auto fixture = managed_fixture{};
   fixture.start();
   auto write = fixture.writer();
   const auto created = forge::asio::blocking::run(
       fixture.app->runtime(), write->create_version(create_request("create-first", 0, "retained-native-material")));
   BOOST_TEST(created.ref.secret_id == "native/http");
   BOOST_TEST(created.ref.version == 1U);
   BOOST_TEST(created.revision == 1U);

   auto metadata = fixture.metadata();
   const auto description =
       forge::asio::blocking::run(fixture.app->runtime(), metadata->describe_version({created.ref}));
   BOOST_TEST(description.ref.version == 1U);
   BOOST_TEST(description.revision == 1U);
   BOOST_CHECK(description.state == forge::plugins::crypto::secrets::version_state::available);

   auto read = fixture.reader();
   const auto result = forge::asio::blocking::run(
       fixture.app->runtime(), read->get_bytes({.version = 1, .request = {"native/http", "fixture.read"}}));
   BOOST_TEST(result.version == 1U);
   const auto expected = std::string_view{"retained-native-material"};
   BOOST_CHECK(result.result.bytes == forge::crypto::core::bytes(expected.begin(), expected.end()));
}

BOOST_AUTO_TEST_CASE(disabled_store_keeps_safe_metadata_but_rejects_move_only_material_mutation) {
   auto fixture = managed_fixture{};
   fixture.document.set("plugins.crypto.secrets.versions.enabled", false);
   fixture.start();
   auto metadata = fixture.metadata();
   const auto status = forge::asio::blocking::run(fixture.app->runtime(), metadata->status({}));
   BOOST_CHECK(!status.enabled);
   BOOST_CHECK(!status.ready);
   BOOST_TEST(status.versions == 0U);
   BOOST_TEST(status.receipts == 0U);
   auto write = fixture.writer();
   BOOST_CHECK_THROW(forge::asio::blocking::run(fixture.app->runtime(), write->create_version(create_request(
                                                                            "disabled", 0, "never-persisted"))),
                     forge::plugins::crypto::secrets::exceptions::store_unavailable);
   BOOST_CHECK(!std::filesystem::exists(fixture.store_path()));
}

BOOST_AUTO_TEST_CASE(explicit_versions_remain_immutable_and_do_not_enter_the_configured_source_api) {
   auto fixture = managed_fixture{};
   fixture.start();
   create_version(fixture, "first", 0, "first-material");
   create_version(fixture, "second", 1, "second-material");
   const auto first = std::string_view{"first-material"};
   const auto second = std::string_view{"second-material"};
   BOOST_CHECK(read_version(fixture, 1) == forge::crypto::core::bytes(first.begin(), first.end()));
   BOOST_CHECK(read_version(fixture, 2) == forge::crypto::core::bytes(second.begin(), second.end()));
   auto read = fixture.reader();
   BOOST_CHECK_THROW(
       forge::asio::blocking::run(fixture.app->runtime(),
                                  read->get_bytes({.version = 0, .request = {"native/http", "fixture.read"}})),
       forge::plugins::crypto::secrets::exceptions::invalid_request);
   BOOST_CHECK_THROW(
       forge::asio::blocking::run(fixture.app->runtime(),
                                  read->get_bytes({.version = 999, .request = {"native/http", "fixture.read"}})),
       forge::plugins::crypto::secrets::exceptions::version_not_found);
   auto legacy =
       fixture.app->apis().get<forge::plugins::crypto::secrets::api>(forge::plugins::crypto::secrets::api::ref());
   BOOST_CHECK_THROW(
       forge::asio::blocking::run(fixture.app->runtime(), legacy->get_bytes({"native/http", "fixture.read"})),
       forge::plugins::crypto::secrets::exceptions::secret_not_found);
}

BOOST_AUTO_TEST_CASE(versioned_hkdf_and_aes_gcm_use_the_requested_immutable_key) {
   auto fixture = managed_fixture{};
   fixture.document.set("plugins.crypto.secrets.versions.ceiling.operations",
                        forge::config::core::value::array_type{forge::config::core::value{"get_bytes"},
                                                               forge::config::core::value{"derive_hkdf_sha256"},
                                                               forge::config::core::value{"encrypt_aes_gcm"},
                                                               forge::config::core::value{"decrypt_aes_gcm"}});
   fixture.start();
   auto writer = fixture.writer();
   auto first = create_request("key-one", 0, std::string(32, 'a'));
   first.kind = forge::plugins::crypto::secrets::secret_kind::symmetric_key;
   first.policy.operations = {forge::plugins::crypto::secrets::operation::get_bytes,
                              forge::plugins::crypto::secrets::operation::derive_hkdf_sha256,
                              forge::plugins::crypto::secrets::operation::encrypt_aes_gcm,
                              forge::plugins::crypto::secrets::operation::decrypt_aes_gcm};
   auto second = create_request("key-two", 1, std::string(32, 'b'));
   second.kind = first.kind;
   second.policy = first.policy;
   forge::asio::blocking::run(fixture.app->runtime(), writer->create_version(std::move(first)));
   forge::asio::blocking::run(fixture.app->runtime(), writer->create_version(std::move(second)));
   auto reader = fixture.reader();
   const auto derived = forge::asio::blocking::run(
       fixture.app->runtime(), reader->derive_hkdf_sha256({.version = 1, .request = {"native/http", "fixture.read"}}));
   const auto other = forge::asio::blocking::run(
       fixture.app->runtime(), reader->derive_hkdf_sha256({.version = 2, .request = {"native/http", "fixture.read"}}));
   BOOST_TEST(derived.version == 1U);
   BOOST_TEST(other.version == 2U);
   BOOST_TEST(derived.result.bytes.size() == 32U);
   BOOST_CHECK(derived.result.bytes != other.result.bytes);
   const auto plaintext = forge::crypto::core::bytes{'f', 'i', 'x', 't', 'u', 'r', 'e'};
   const auto encrypted = forge::asio::blocking::run(
       fixture.app->runtime(), reader->encrypt_aes_gcm({.version = 1,
                                                        .request = {.secret_id = "native/http",
                                                                    .purpose = "fixture.read",
                                                                    .nonce = forge::crypto::core::bytes(12, 1),
                                                                    .plaintext = plaintext,
                                                                    .aad = {2, 3}}}));
   const auto decrypted = forge::asio::blocking::run(
       fixture.app->runtime(), reader->decrypt_aes_gcm({.version = 1,
                                                        .request = {.secret_id = "native/http",
                                                                    .purpose = "fixture.read",
                                                                    .nonce = encrypted.result.nonce,
                                                                    .tag = encrypted.result.tag,
                                                                    .ciphertext = encrypted.result.ciphertext,
                                                                    .aad = {2, 3}}}));
   BOOST_TEST(encrypted.version == 1U);
   BOOST_TEST(decrypted.version == 1U);
   BOOST_CHECK(decrypted.result.plaintext == plaintext);
}

BOOST_AUTO_TEST_CASE(metadata_pages_are_bounded_and_do_not_reveal_material_or_mutation_tokens) {
   auto fixture = managed_fixture{};
   fixture.start();
   create_version(fixture, "private-create-token", 0, "metadata-secret-sentinel");
   create_version(fixture, "next-private-token", 1, "another-secret-sentinel");
   auto metadata = fixture.metadata();
   const auto first = forge::asio::blocking::run(fixture.app->runtime(),
                                                 metadata->list_versions({.secret_id = "native/http", .limit = 1}));
   BOOST_REQUIRE_EQUAL(first.versions.size(), 1U);
   BOOST_TEST(first.versions.front().ref.version == 1U);
   BOOST_TEST(first.next_after_version == 1U);
   const auto second = forge::asio::blocking::run(
       fixture.app->runtime(), metadata->list_versions({.secret_id = "native/http", .after_version = 1, .limit = 1}));
   BOOST_REQUIRE_EQUAL(second.versions.size(), 1U);
   BOOST_TEST(second.versions.front().ref.version == 2U);
   BOOST_TEST(second.next_after_version == 0U);
   BOOST_CHECK_THROW(forge::asio::blocking::run(fixture.app->runtime(),
                                                metadata->list_versions({.secret_id = "native/http", .limit = 129})),
                     forge::plugins::crypto::secrets::exceptions::invalid_request);
   const auto encoded = forge::codec::json::write(first);
   BOOST_REQUIRE(encoded.ok());
   BOOST_CHECK(encoded.text.find("metadata-secret-sentinel") == std::string::npos);
   BOOST_CHECK(encoded.text.find("private-create-token") == std::string::npos);
   BOOST_CHECK(encoded.text.find("fingerprint") == std::string::npos);
   BOOST_CHECK(encoded.text.find("bootstrap") == std::string::npos);
   BOOST_CHECK(read_file(fixture.store_path()).find("metadata-secret-sentinel") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(version_reads_keep_purpose_operation_and_raw_export_gates) {
   auto fixture = managed_fixture{};
   fixture.start();
   create_version(fixture, "first", 0, "purpose-protected-material");
   auto read = fixture.reader();
   BOOST_CHECK_THROW(
       forge::asio::blocking::run(fixture.app->runtime(),
                                  read->get_bytes({.version = 1, .request = {"native/http", "other.purpose"}})),
       forge::plugins::crypto::secrets::exceptions::purpose_denied);
   BOOST_CHECK_THROW(forge::asio::blocking::run(
                         fixture.app->runtime(),
                         read->derive_hkdf_sha256(
                             {.version = 1, .request = {.secret_id = "native/http", .purpose = "fixture.read"}})),
                     forge::plugins::crypto::secrets::exceptions::operation_denied);
   auto request = create_request("no-export", 1, "non-exportable");
   request.policy.allow_raw_export = false;
   auto write = fixture.writer();
   forge::asio::blocking::run(fixture.app->runtime(), write->create_version(std::move(request)));
   BOOST_CHECK_THROW(
       forge::asio::blocking::run(fixture.app->runtime(),
                                  read->get_bytes({.version = 2, .request = {"native/http", "fixture.read"}})),
       forge::plugins::crypto::secrets::exceptions::operation_denied);
}

BOOST_AUTO_TEST_CASE(create_policy_cannot_expand_the_configured_ceiling) {
   auto fixture = managed_fixture{};
   fixture.start();
   auto write = fixture.writer();
   auto purpose = create_request("bad-purpose", 0, "material");
   purpose.policy.purposes = {"other.purpose"};
   BOOST_CHECK_THROW(forge::asio::blocking::run(fixture.app->runtime(), write->create_version(std::move(purpose))),
                     forge::plugins::crypto::secrets::exceptions::purpose_denied);
   auto operation = create_request("bad-operation", 0, "material");
   operation.policy.operations.push_back(forge::plugins::crypto::secrets::operation::derive_hkdf_sha256);
   BOOST_CHECK_THROW(forge::asio::blocking::run(fixture.app->runtime(), write->create_version(std::move(operation))),
                     forge::plugins::crypto::secrets::exceptions::operation_denied);
   auto duplicate = create_request("duplicate-purpose", 0, "material");
   duplicate.policy.purposes.push_back("fixture.read");
   BOOST_CHECK_THROW(forge::asio::blocking::run(fixture.app->runtime(), write->create_version(std::move(duplicate))),
                     forge::plugins::crypto::secrets::exceptions::invalid_request);
   auto metadata = fixture.metadata();
   const auto status = forge::asio::blocking::run(fixture.app->runtime(), metadata->status({}));
   BOOST_TEST(status.versions == 0U);
   BOOST_TEST(status.receipts == 0U);
}

BOOST_AUTO_TEST_CASE(denied_managed_purpose_uses_a_fixed_error_without_echoing_caller_input) {
   auto fixture = managed_fixture{};
   fixture.start();
   create_version(fixture, "original", 0, "original-material");
   auto reader = fixture.reader();
   const auto sentinel = std::string{"denied-purpose-redaction-sentinel"};
   auto denied = false;
   try {
      forge::asio::blocking::run(fixture.app->runtime(),
                                 reader->get_bytes({.version = 1, .request = {"native/http", sentinel}}));
   } catch (const forge::plugins::crypto::secrets::exceptions::purpose_denied& error) {
      denied = true;
      const auto rendered = forge::exceptions::format_exception_chain(error);
      BOOST_CHECK(rendered.find(sentinel) == std::string::npos);
   }
   BOOST_CHECK(denied);
}

BOOST_AUTO_TEST_CASE(replay_precedes_stale_cas_and_conflicts_on_every_changed_mutation_field) {
   auto fixture = managed_fixture{};
   fixture.start();
   const auto material = std::string(32, 'A');
   const auto first = create_version(fixture, "same-token", 0, material);
   const auto before = read_file(fixture.store_path());
   const auto replay = create_version(fixture, "same-token", 0, material);
   BOOST_CHECK(replay == first);
   BOOST_CHECK(read_file(fixture.store_path()) == before);
   BOOST_CHECK_THROW(create_version(fixture, "same-token", 0, std::string(32, 'B')),
                     forge::plugins::crypto::secrets::exceptions::idempotency_conflict);
   BOOST_CHECK_THROW(create_version(fixture, "same-token", 1, material),
                     forge::plugins::crypto::secrets::exceptions::idempotency_conflict);
   auto changed_policy = create_request("same-token", 0, material);
   changed_policy.policy.allow_raw_export = false;
   auto write = fixture.writer();
   BOOST_CHECK_THROW(
       forge::asio::blocking::run(fixture.app->runtime(), write->create_version(std::move(changed_policy))),
       forge::plugins::crypto::secrets::exceptions::idempotency_conflict);
   auto changed_id = create_request("same-token", 0, material);
   changed_id.secret_id = "another/native";
   BOOST_CHECK_THROW(forge::asio::blocking::run(fixture.app->runtime(), write->create_version(std::move(changed_id))),
                     forge::plugins::crypto::secrets::exceptions::idempotency_conflict);
   auto changed_kind = create_request("same-token", 0, material);
   changed_kind.kind = forge::plugins::crypto::secrets::secret_kind::symmetric_key;
   BOOST_CHECK_THROW(forge::asio::blocking::run(fixture.app->runtime(), write->create_version(std::move(changed_kind))),
                     forge::plugins::crypto::secrets::exceptions::idempotency_conflict);
   BOOST_CHECK_THROW(
       forge::asio::blocking::run(
           fixture.app->runtime(),
           write->revoke_version({.mutation_id = "same-token", .ref = first.ref, .expected_revision = 1})),
       forge::plugins::crypto::secrets::exceptions::idempotency_conflict);
   BOOST_CHECK_THROW(create_version(fixture, "fresh-stale-token", 0, material),
                     forge::plugins::crypto::secrets::exceptions::revision_conflict);
   auto metadata = fixture.metadata();
   const auto status = forge::asio::blocking::run(fixture.app->runtime(), metadata->status({}));
   BOOST_TEST(status.versions == 1U);
   BOOST_TEST(status.receipts == 1U);
}

BOOST_AUTO_TEST_CASE(concurrent_same_revision_creates_commit_exactly_one_version_and_receipt) {
   auto fixture = managed_fixture{};
   fixture.start();
   auto ready = std::barrier{3};
   auto succeeded = std::atomic<unsigned>{0};
   auto conflicted = std::atomic<unsigned>{0};
   auto unexpected = std::atomic<unsigned>{0};
   const auto create = [&](std::string token) {
      ready.arrive_and_wait();
      try {
         create_version(fixture, std::move(token), 0, "concurrent-material");
         ++succeeded;
      } catch (const forge::plugins::crypto::secrets::exceptions::revision_conflict&) {
         ++conflicted;
      } catch (...) {
         ++unexpected;
      }
   };
   auto first = std::jthread{create, "concurrent-first"};
   auto second = std::jthread{create, "concurrent-second"};
   ready.arrive_and_wait();
   first.join();
   second.join();
   BOOST_TEST(succeeded.load() == 1U);
   BOOST_TEST(conflicted.load() == 1U);
   BOOST_TEST(unexpected.load() == 0U);
   auto metadata = fixture.metadata();
   const auto status = forge::asio::blocking::run(fixture.app->runtime(), metadata->status({}));
   BOOST_TEST(status.versions == 1U);
   BOOST_TEST(status.receipts == 1U);
}

BOOST_AUTO_TEST_CASE(revoke_and_its_receipt_survive_restart_without_reactivating_a_create_replay) {
   auto fixture = managed_fixture{};
   fixture.start();
   const auto first = create_version(fixture, "original-create", 0, "retained-material");
   auto write = fixture.writer();
   const auto revoked = forge::asio::blocking::run(
       fixture.app->runtime(),
       write->revoke_version({.mutation_id = "first-revoke", .ref = first.ref, .expected_revision = 1}));
   BOOST_TEST(revoked.revision == 2U);
   BOOST_CHECK_THROW(read_version(fixture, 1), forge::plugins::crypto::secrets::exceptions::version_revoked);
   const auto before = read_file(fixture.store_path());
   const auto repeated = forge::asio::blocking::run(
       fixture.app->runtime(),
       write->revoke_version({.mutation_id = "first-revoke", .ref = first.ref, .expected_revision = 1}));
   BOOST_CHECK(repeated == revoked);
   BOOST_CHECK(read_file(fixture.store_path()) == before);
   fixture.stop();
   fixture.start("open_existing");
   BOOST_CHECK(create_version(fixture, "original-create", 0, "retained-material") == first);
   auto metadata = fixture.metadata();
   const auto current = forge::asio::blocking::run(fixture.app->runtime(), metadata->describe_version({first.ref}));
   BOOST_CHECK(current.state == forge::plugins::crypto::secrets::version_state::revoked);
   BOOST_TEST(current.revision == 2U);
   BOOST_CHECK_THROW(read_version(fixture, 1), forge::plugins::crypto::secrets::exceptions::version_revoked);
   const auto next = create_version(fixture, "fresh-create", 2, "new-material");
   BOOST_TEST(next.ref.version == 2U);
   BOOST_TEST(next.revision == 3U);
}

BOOST_AUTO_TEST_CASE(zero_default_retry_after_ceiling_expansion_uses_the_original_receipt_and_policy) {
   auto fixture = managed_fixture{};
   fixture.start();
   const auto created = create_version(fixture, "zero-defaults", 0, "zero-default-material");
   fixture.stop();
   fixture.document.set("plugins.crypto.secrets.versions.ceiling.max-plaintext-bytes", std::uint64_t{8192});
   fixture.document.set("plugins.crypto.secrets.versions.ceiling.max-ciphertext-bytes", std::uint64_t{16384});
   fixture.document.set("plugins.crypto.secrets.versions.ceiling.max-aad-bytes", std::uint64_t{8192});
   fixture.start("open_existing");
   const auto before = read_file(fixture.store_path());
   BOOST_CHECK(create_version(fixture, "zero-defaults", 0, "zero-default-material") == created);
   BOOST_CHECK(read_file(fixture.store_path()) == before);
   auto metadata = fixture.metadata();
   const auto description =
       forge::asio::blocking::run(fixture.app->runtime(), metadata->describe_version({created.ref}));
   BOOST_TEST(description.policy.max_plaintext_bytes == 4096U);
   BOOST_TEST(description.policy.max_ciphertext_bytes == 8192U);
   BOOST_TEST(description.policy.max_aad_bytes == 4096U);
}

BOOST_AUTO_TEST_CASE(narrowing_an_available_policy_fails_startup_without_rewriting_the_store) {
   auto fixture = managed_fixture{};
   fixture.start();
   create_version(fixture, "first", 0, "small-material-with-large-policy");
   fixture.stop();
   const auto before = read_file(fixture.store_path());
   fixture.document.set("plugins.crypto.secrets.versions.ceiling.max-plaintext-bytes", std::uint64_t{2048});
   BOOST_CHECK_THROW(fixture.start("open_existing"), forge::plugins::crypto::secrets::exceptions::invalid_config);
   BOOST_CHECK(read_file(fixture.store_path()) == before);
}

BOOST_AUTO_TEST_CASE(revoked_policy_allows_ceiling_narrowing_without_rewriting_historical_policy) {
   auto fixture = managed_fixture{};
   fixture.start();
   const auto created = create_version(fixture, "first", 0, "revoked-material");
   auto write = fixture.writer();
   forge::asio::blocking::run(
       fixture.app->runtime(),
       write->revoke_version({.mutation_id = "revoke", .ref = created.ref, .expected_revision = 1}));
   fixture.stop();
   fixture.document.set("plugins.crypto.secrets.versions.ceiling.max-plaintext-bytes", std::uint64_t{2048});
   fixture.start("open_existing");
   auto metadata = fixture.metadata();
   const auto description =
       forge::asio::blocking::run(fixture.app->runtime(), metadata->describe_version({created.ref}));
   BOOST_CHECK(description.state == forge::plugins::crypto::secrets::version_state::revoked);
   BOOST_TEST(description.policy.max_plaintext_bytes == 4096U);
   BOOST_CHECK_THROW(read_version(fixture, 1), forge::plugins::crypto::secrets::exceptions::version_revoked);
}

BOOST_AUTO_TEST_CASE(full_version_history_still_accepts_the_first_revoke_with_a_maximum_token) {
   auto fixture = managed_fixture{};
   fixture.document.set("plugins.crypto.secrets.versions.max-versions", std::uint64_t{1});
   fixture.document.set("plugins.crypto.secrets.versions.max-versions-per-secret", std::uint64_t{1});
   fixture.start();
   const auto created = create_version(fixture, "first", 0, "one-version-material");
   BOOST_CHECK_THROW(create_version(fixture, "second", 1, "another-material"),
                     forge::plugins::crypto::secrets::exceptions::capacity_exceeded);
   auto write = fixture.writer();
   const auto revoked = forge::asio::blocking::run(
       fixture.app->runtime(),
       write->revoke_version({.mutation_id = std::string(128, 'r'), .ref = created.ref, .expected_revision = 1}));
   BOOST_TEST(revoked.revision == 2U);
   BOOST_CHECK_THROW(create_version(fixture, "after-revoke", 2, "another-material"),
                     forge::plugins::crypto::secrets::exceptions::capacity_exceeded);
   fixture.stop();
   fixture.start("open_existing");
   BOOST_CHECK_THROW(read_version(fixture, 1), forge::plugins::crypto::secrets::exceptions::version_revoked);
}

BOOST_AUTO_TEST_CASE(single_version_store_reopens_a_full_policy_with_independent_list_limits) {
   auto fixture = managed_fixture{};
   fixture.document.set("plugins.crypto.secrets.versions.max-secret-ids", std::uint64_t{1});
   fixture.document.set("plugins.crypto.secrets.versions.max-versions", std::uint64_t{1});
   auto purposes = std::vector<std::string>{"fixture.read"};
   for (auto index = 1; index < 32; ++index) {
      purposes.push_back("fixture.purpose." + std::to_string(index));
   }
   auto configured_purposes = forge::config::core::value::array_type{};
   for (const auto& purpose : purposes) {
      configured_purposes.emplace_back(purpose);
   }
   fixture.document.set("plugins.crypto.secrets.versions.ceiling.purposes", std::move(configured_purposes));
   fixture.document.set("plugins.crypto.secrets.versions.ceiling.operations",
                        forge::config::core::value::array_type{forge::config::core::value{"get_bytes"},
                                                               forge::config::core::value{"derive_hkdf_sha256"},
                                                               forge::config::core::value{"encrypt_aes_gcm"},
                                                               forge::config::core::value{"decrypt_aes_gcm"}});
   fixture.start();
   const auto material = std::string(32, 'k');
   auto request = create_request("full-policy", 0, material);
   request.kind = forge::plugins::crypto::secrets::secret_kind::symmetric_key;
   request.policy.purposes = purposes;
   request.policy.operations = {forge::plugins::crypto::secrets::operation::get_bytes,
                                forge::plugins::crypto::secrets::operation::derive_hkdf_sha256,
                                forge::plugins::crypto::secrets::operation::encrypt_aes_gcm,
                                forge::plugins::crypto::secrets::operation::decrypt_aes_gcm};
   auto writer = fixture.writer();
   const auto created = forge::asio::blocking::run(fixture.app->runtime(), writer->create_version(std::move(request)));
   fixture.stop();
   const auto before = read_file(fixture.store_path());
   fixture.start("open_existing");
   auto metadata = fixture.metadata();
   const auto reopened = forge::asio::blocking::run(fixture.app->runtime(), metadata->describe_version({created.ref}));
   std::sort(purposes.begin(), purposes.end());
   BOOST_CHECK(reopened.policy.purposes == purposes);
   BOOST_TEST(reopened.policy.operations.size() == 4U);
   BOOST_CHECK(read_version(fixture, created.ref.version) ==
               forge::crypto::core::bytes(material.begin(), material.end()));
   BOOST_CHECK(read_file(fixture.store_path()) == before);
}

BOOST_AUTO_TEST_CASE(byte_full_store_reserves_every_maximum_token_revoke_across_receipt_prefix_growth) {
   auto fixture = managed_fixture{};
   fixture.document.set("plugins.crypto.secrets.versions.max-versions", std::uint64_t{256});
   fixture.document.set("plugins.crypto.secrets.versions.max-versions-per-secret", std::uint64_t{256});
   fixture.document.set("plugins.crypto.secrets.versions.max-store-plaintext-bytes", std::uint64_t{96 * 1024});
   fixture.start();
   auto created = std::vector<forge::plugins::crypto::secrets::version_ref>{};
   auto full = false;
   for (auto index = std::uint64_t{0}; index < 256; ++index) {
      auto token = "c" + std::to_string(index);
      token.resize(128, 'c');
      try {
         created.push_back(create_version(fixture, token, index, "small-material").ref);
      } catch (const forge::plugins::crypto::secrets::exceptions::capacity_exceeded&) {
         full = true;
         break;
      }
   }

   BOOST_REQUIRE(full);
   BOOST_REQUIRE_GE(created.size(), 64U);
   fixture.stop();
   fixture.start("open_existing");
   auto write = fixture.writer();
   auto revision = static_cast<std::uint64_t>(created.size());
   for (auto index = std::size_t{0}; index < created.size(); ++index) {
      auto token = "r" + std::to_string(index);
      token.resize(128, 'r');
      const auto revoked = forge::asio::blocking::run(
          fixture.app->runtime(),
          write->revoke_version({.mutation_id = token, .ref = created[index], .expected_revision = revision}));
      ++revision;
      BOOST_TEST(revoked.revision == revision);
   }

   fixture.stop();
   fixture.start("open_existing");
   auto metadata = fixture.metadata();
   const auto status = forge::asio::blocking::run(fixture.app->runtime(), metadata->status({}));
   BOOST_TEST(status.versions == created.size());
   BOOST_TEST(status.receipts == 2U * created.size());
   for (const auto& ref : created) {
      BOOST_CHECK_THROW(read_version(fixture, ref.version),
                        forge::plugins::crypto::secrets::exceptions::version_revoked);
   }
}

BOOST_AUTO_TEST_CASE(truncated_and_corrupt_snapshots_never_reset_or_promote_a_leftover_file) {
   for (const auto truncate : {true, false}) {
      auto fixture = managed_fixture{};
      fixture.start();
      create_version(fixture, "first", 0, "restart-material");
      fixture.stop();
      auto bytes = read_file(fixture.store_path());
      BOOST_REQUIRE_GT(bytes.size(), 16U);
      write_file(fixture.store_path().string() + ".tmp.leftover", bytes);
      if (truncate) {
         bytes.resize(9);
      } else {
         bytes.back() ^= 1;
      }

      write_file(fixture.store_path(), bytes);
      BOOST_CHECK_THROW(fixture.start("open_existing"), forge::plugins::crypto::secrets::exceptions::invalid_store);
      BOOST_CHECK(read_file(fixture.store_path()) == bytes);
   }
}

BOOST_AUTO_TEST_CASE(missing_existing_store_and_wrong_bootstrap_never_create_a_replacement) {
   for (const auto remove_store : {true, false}) {
      auto fixture = managed_fixture{};
      fixture.start();
      create_version(fixture, "first", 0, "restart-material");
      fixture.stop();
      const auto before = read_file(fixture.store_path());
      if (remove_store) {
         std::filesystem::remove(fixture.store_path());
      } else {
         write_file(fixture.directory / "bootstrap", "different-test-bootstrap");
      }

      BOOST_CHECK_THROW(fixture.start("open_existing"), forge::plugins::crypto::secrets::exceptions::invalid_store);
      if (remove_store) {
         BOOST_CHECK(!std::filesystem::exists(fixture.store_path()));
      } else {
         BOOST_CHECK(read_file(fixture.store_path()) == before);
      }
   }
}

BOOST_AUTO_TEST_CASE(missing_insecure_and_symlink_bootstrap_are_rejected_before_store_creation) {
   for (const auto kind : {0, 1, 2}) {
      auto fixture = managed_fixture{};
      const auto bootstrap = fixture.directory / "bootstrap";
      if (kind == 0) {
         std::filesystem::remove(bootstrap);
      } else if (kind == 1) {
         std::filesystem::permissions(bootstrap, std::filesystem::perms::others_read,
                                      std::filesystem::perm_options::add);
      } else {
         std::filesystem::rename(bootstrap, fixture.directory / "bootstrap-target");
         std::filesystem::create_symlink(fixture.directory / "bootstrap-target", bootstrap);
      }

      BOOST_CHECK_THROW(fixture.start(), forge::plugins::crypto::secrets::exceptions::invalid_source);
      BOOST_CHECK(!std::filesystem::exists(fixture.store_path()));
   }
}

BOOST_AUTO_TEST_CASE(canonical_wire_fixture_roundtrips_valid_enum_boundaries_without_changing_meaning) {
   auto fixture = managed_fixture{};
   fixture.document.set("plugins.crypto.secrets.versions.ceiling.operations",
                        forge::config::core::value::array_type{forge::config::core::value{"get_bytes"},
                                                               forge::config::core::value{"derive_hkdf_sha256"},
                                                               forge::config::core::value{"encrypt_aes_gcm"},
                                                               forge::config::core::value{"decrypt_aes_gcm"}});
   fixture.start();
   auto writer = fixture.writer();
   auto request = create_request("a-create", 0, std::string(32, 'k'));
   request.kind = forge::plugins::crypto::secrets::secret_kind::symmetric_key;
   request.policy.operations = {forge::plugins::crypto::secrets::operation::get_bytes,
                                forge::plugins::crypto::secrets::operation::derive_hkdf_sha256,
                                forge::plugins::crypto::secrets::operation::encrypt_aes_gcm,
                                forge::plugins::crypto::secrets::operation::decrypt_aes_gcm};
   const auto first = forge::asio::blocking::run(fixture.app->runtime(), writer->create_version(std::move(request)));
   forge::asio::blocking::run(
       fixture.app->runtime(),
       writer->revoke_version({.mutation_id = "b-revoke", .ref = first.ref, .expected_revision = 1}));
   create_version(fixture, "c-create", 2, "available-material");
   fixture.stop();
   const auto payload = decode_fixture_payload(fixture);
   const auto& versions = std::get<4>(payload);
   BOOST_REQUIRE_EQUAL(versions.size(), 2U);
   BOOST_TEST(std::get<1>(versions[0]) == 1);
   BOOST_TEST(std::get<2>(versions[0]) == 1);
   BOOST_TEST(std::get<1>(versions[1]) == 0);
   BOOST_TEST(std::get<2>(versions[1]) == 0);
   BOOST_CHECK(std::get<1>(std::get<3>(versions[0])) == std::vector<std::int64_t>({0, 1, 2, 3}));
   const auto& receipts = std::get<6>(payload);
   BOOST_REQUIRE_EQUAL(receipts.size(), 3U);
   BOOST_TEST(std::get<0>(receipts[0].second) == 0);
   BOOST_TEST(std::get<0>(receipts[1].second) == 1);
   encode_fixture_payload(fixture, payload);
   fixture.start("open_existing");
   BOOST_CHECK_THROW(read_version(fixture, 1), forge::plugins::crypto::secrets::exceptions::version_revoked);
   const auto material = std::string_view{"available-material"};
   BOOST_CHECK(read_version(fixture, 2) == forge::crypto::core::bytes(material.begin(), material.end()));
}

BOOST_AUTO_TEST_CASE(authenticated_payloads_with_inconsistent_versions_heads_or_receipts_fail_closed) {
   const auto mutations = std::vector<std::pair<std::string_view, std::function<void(stored_payload&)>>>{
       {"schema", [](auto& value) { std::get<0>(value) = "different.schema"; }},
       {"schema version", [](auto& value) { std::get<1>(value) = 2; }},
       {"receipt key", [](auto& value) { std::get<2>(value).resize(31); }},
       {"empty id", [](auto& value) { std::get<0>(std::get<4>(value)[0]).secret_id.clear(); }},
       {"zero version", [](auto& value) { std::get<0>(std::get<4>(value)[0]).version = 0; }},
       {"duplicate version", [](auto& value) { std::get<4>(value)[1] = std::get<4>(value)[0]; }},
       {"outside head", [](auto& value) { std::get<0>(std::get<4>(value)[1]).version = 3; }},
       {"missing head", [](auto& value) { std::get<3>(value).clear(); }},
       {"orphan head", [](auto& value) { std::get<3>(value).push_back({"orphan", {1, 1}}); }},
       {"latest counter", [](auto& value) { std::get<0>(std::get<3>(value)[0].second) = 9; }},
       {"revision counter", [](auto& value) { std::get<1>(std::get<3>(value)[0].second) = 9; }},
       {"unknown kind", [](auto& value) { std::get<1>(std::get<4>(value)[0]) = 99; }},
       {"wrapped kind", [](auto& value) { std::get<1>(std::get<4>(value)[0]) = std::int64_t{1} << 32; }},
       {"unknown state", [](auto& value) { std::get<2>(std::get<4>(value)[0]) = 99; }},
       {"wrapped state", [](auto& value) { std::get<2>(std::get<4>(value)[1]) = std::int64_t{1} << 32; }},
       {"unknown operation", [](auto& value) { std::get<1>(std::get<3>(std::get<4>(value)[0]))[0] = 99; }},
       {"wrapped operation",
        [](auto& value) { std::get<1>(std::get<3>(std::get<4>(value)[0]))[0] = std::int64_t{1} << 32; }},
       {"missing create", [](auto& value) { std::get<6>(value).erase(std::get<6>(value).begin()); }},
       {"missing revoke", [](auto& value) { std::get<6>(value).erase(std::get<6>(value).begin() + 1); }},
       {"dangling receipt", [](auto& value) { std::get<2>(std::get<6>(value)[2].second).ref.version = 99; }},
       {"unknown mutation", [](auto& value) { std::get<0>(std::get<6>(value)[0].second) = 2; }},
       {"wrapped mutation", [](auto& value) { std::get<0>(std::get<6>(value)[0].second) = 256; }},
       {"duplicate create",
        [](auto& value) { std::get<6>(value).push_back({"duplicate", std::get<6>(value)[0].second}); }},
       {"duplicate revision", [](auto& value) { std::get<2>(std::get<6>(value)[1].second).revision = 1; }},
       {"skipped revision", [](auto& value) { std::get<2>(std::get<6>(value)[1].second).revision = 9; }},
       {"available revoked version", [](auto& value) { std::get<2>(std::get<4>(value)[0]) = 0; }},
       {"revoked without revoke", [](auto& value) { std::get<2>(std::get<4>(value)[1]) = 1; }},
       {"missing material", [](auto& value) { std::get<5>(value).pop_back(); }},
       {"empty material", [](auto& value) { std::get<5>(value)[0].clear(); }},
       {"empty policy", [](auto& value) { std::get<0>(std::get<3>(std::get<4>(value)[0])).clear(); }},
   };
   for (const auto& [name, mutate] : mutations) {
      BOOST_TEST_CONTEXT(name) {
         auto fixture = managed_fixture{};
         fixture.start();
         const auto first = create_version(fixture, "a-create", 0, "first-material");
         auto writer = fixture.writer();
         forge::asio::blocking::run(
             fixture.app->runtime(),
             writer->revoke_version({.mutation_id = "b-revoke", .ref = first.ref, .expected_revision = 1}));
         create_version(fixture, "c-create", 2, "second-material");
         fixture.stop();
         auto payload = decode_fixture_payload(fixture);
         BOOST_REQUIRE_EQUAL(std::get<4>(payload).size(), 2U);
         BOOST_REQUIRE_EQUAL(std::get<6>(payload).size(), 3U);
         mutate(payload);
         encode_fixture_payload(fixture, payload);
         const auto before = read_file(fixture.store_path());
         BOOST_CHECK_THROW(fixture.start("open_existing"), forge::plugins::crypto::secrets::exceptions::invalid_store);
         BOOST_CHECK(read_file(fixture.store_path()) == before);
      }
   }
}

BOOST_AUTO_TEST_CASE(authenticated_create_receipts_cannot_reverse_the_immutable_version_order) {
   auto fixture = managed_fixture{};
   fixture.start();
   create_version(fixture, "a-create", 0, "first-material");
   create_version(fixture, "b-create", 1, "second-material");
   fixture.stop();
   auto payload = decode_fixture_payload(fixture);
   BOOST_REQUIRE_EQUAL(std::get<6>(payload).size(), 2U);
   auto& first = std::get<2>(std::get<6>(payload)[0].second);
   auto& second = std::get<2>(std::get<6>(payload)[1].second);
   BOOST_REQUIRE_EQUAL(first.ref.version, 1U);
   BOOST_REQUIRE_EQUAL(second.ref.version, 2U);
   std::swap(first.revision, second.revision);
   encode_fixture_payload(fixture, payload);
   const auto before = read_file(fixture.store_path());
   BOOST_CHECK_THROW(fixture.start("open_existing"), forge::plugins::crypto::secrets::exceptions::invalid_store);
   BOOST_CHECK(read_file(fixture.store_path()) == before);
}

BOOST_AUTO_TEST_CASE(abandoned_native_create_does_not_persist_or_consume_pending_admission) {
   auto fixture = managed_fixture{};
   fixture.document.set("plugins.crypto.secrets.versions.max-pending-calls", std::uint64_t{1});
   fixture.start();
   const auto before = read_file(fixture.store_path());
   {
      auto write = fixture.writer();
      auto never_awaited = write->create_version(create_request("abandoned", 0, "unconsumed-material"));
      static_cast<void>(never_awaited);
   }

   BOOST_CHECK(read_file(fixture.store_path()) == before);
   const auto created = create_version(fixture, "real-create", 0, "consumed-material");
   BOOST_TEST(created.ref.version == 1U);
   auto metadata = fixture.metadata();
   const auto status = forge::asio::blocking::run(fixture.app->runtime(), metadata->status({}));
   BOOST_TEST(status.versions == 1U);
   BOOST_TEST(status.receipts == 1U);
}

BOOST_AUTO_TEST_CASE(prepublication_io_failures_keep_the_old_authority_and_do_not_consume_a_receipt) {
   using forge::tests::secrets::failure_point;
   for (const auto point :
        {failure_point::write, failure_point::file_sync, failure_point::file_close, failure_point::publish}) {
      auto fixture = managed_fixture{};
      fixture.start();
      create_version(fixture, "original", 0, "original-material");
      const auto before = read_file(fixture.store_path());
      {
         auto fault = forge::tests::secrets::syscall_fault{fixture.store_path(), point};
         BOOST_CHECK_THROW(create_version(fixture, "retry-after-io", 1, "candidate-material"),
                           forge::plugins::crypto::secrets::exceptions::store_unavailable);
         BOOST_REQUIRE(fault.fired());
      }
      BOOST_CHECK(read_file(fixture.store_path()) == before);
      const auto original = std::string_view{"original-material"};
      BOOST_CHECK(read_version(fixture, 1) == forge::crypto::core::bytes(original.begin(), original.end()));
      auto metadata = fixture.metadata();
      const auto status = forge::asio::blocking::run(fixture.app->runtime(), metadata->status({}));
      BOOST_CHECK(status.ready);
      BOOST_CHECK(!status.fault);
      BOOST_TEST(status.versions == 1U);
      BOOST_TEST(status.receipts == 1U);
      const auto retry = create_version(fixture, "retry-after-io", 1, "candidate-material");
      BOOST_TEST(retry.ref.version == 2U);
      BOOST_TEST(retry.revision == 2U);
   }
}

BOOST_AUTO_TEST_CASE(postpublication_sync_failure_fences_reads_and_writes_and_preserves_the_receipt) {
   auto fixture = managed_fixture{};
   fixture.start();
   create_version(fixture, "original", 0, "original-material");
   const auto before = read_file(fixture.store_path());
   {
      auto fault = forge::tests::secrets::syscall_fault{fixture.store_path(),
                                                        forge::tests::secrets::failure_point::directory_sync};
      BOOST_CHECK_THROW(create_version(fixture, "published-but-uncertain", 1, "published-material"),
                        forge::plugins::crypto::secrets::exceptions::durability_unknown);
      BOOST_REQUIRE(fault.fired());
   }
   BOOST_CHECK(read_file(fixture.store_path()) != before);
   auto metadata = fixture.metadata();
   const auto status = forge::asio::blocking::run(fixture.app->runtime(), metadata->status({}));
   BOOST_CHECK(status.fault);
   BOOST_CHECK(!status.ready);
   BOOST_CHECK_THROW(read_version(fixture, 1), forge::plugins::crypto::secrets::exceptions::store_unavailable);
   BOOST_CHECK_THROW(create_version(fixture, "fenced-write", 2, "never-admitted"),
                     forge::plugins::crypto::secrets::exceptions::store_unavailable);
   const auto published = read_file(fixture.store_path());
   fixture.stop();
   fixture.start("open_existing");
   const auto replay = create_version(fixture, "published-but-uncertain", 1, "published-material");
   BOOST_TEST(replay.ref.version == 2U);
   BOOST_TEST(replay.revision == 2U);
   BOOST_CHECK(read_file(fixture.store_path()) == published);
   const auto material = std::string_view{"published-material"};
   BOOST_CHECK(read_version(fixture, 2) == forge::crypto::core::bytes(material.begin(), material.end()));
}

BOOST_AUTO_TEST_CASE(admitted_hkdf_retains_its_resource_while_revoke_denies_new_calls) {
   auto fixture = managed_fixture{};
   fixture.document.set("plugins.crypto.secrets.versions.ceiling.operations",
                        forge::config::core::value::array_type{forge::config::core::value{"get_bytes"},
                                                               forge::config::core::value{"derive_hkdf_sha256"}});
   fixture.start();
   auto writer = fixture.writer();
   auto request = create_request("key", 0, "retained-material");
   request.policy.operations.push_back(forge::plugins::crypto::secrets::operation::derive_hkdf_sha256);
   const auto created = forge::asio::blocking::run(fixture.app->runtime(), writer->create_version(std::move(request)));
   auto reader = fixture.reader();
   auto fault =
       forge::tests::secrets::syscall_fault{fixture.store_path(), forge::tests::secrets::failure_point::pause_hkdf};
   auto admitted =
       boost::asio::co_spawn(fixture.app->runtime().context(),
                             reader->derive_hkdf_sha256({.version = 1,
                                                         .request = {.secret_id = "native/http",
                                                                     .purpose = "fixture.read",
                                                                     .salt = forge::crypto::core::bytes(32, 0x77)}}),
                             boost::asio::use_future);
   BOOST_REQUIRE(fault.wait_until_paused());
   const auto revoked = forge::asio::blocking::run(
       fixture.app->runtime(),
       writer->revoke_version({.mutation_id = "while-admitted", .ref = created.ref, .expected_revision = 1}));
   BOOST_TEST(revoked.revision == 2U);
   auto metadata = fixture.metadata();
   const auto current = forge::asio::blocking::run(fixture.app->runtime(), metadata->describe_version({created.ref}));
   BOOST_CHECK(current.state == forge::plugins::crypto::secrets::version_state::revoked);
   BOOST_CHECK_THROW(forge::asio::blocking::run(
                         fixture.app->runtime(),
                         reader->derive_hkdf_sha256({.version = 1, .request = {"native/http", "fixture.read"}})),
                     forge::plugins::crypto::secrets::exceptions::version_revoked);
   fault.release();
   BOOST_REQUIRE(admitted.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   const auto completed = admitted.get();
   BOOST_TEST(completed.version == 1U);
   BOOST_TEST(completed.result.bytes.size() == 32U);
   BOOST_CHECK(!fault.pause_timed_out());
}

BOOST_AUTO_TEST_CASE(inherited_native_handle_rejects_forked_use_before_store_mutex_and_parent_stays_ready) {
   auto fixture = managed_fixture{};
   fixture.start();
   create_version(fixture, "original", 0, "parent-material");
   auto metadata = fixture.metadata();
   const auto child = ::fork();
   BOOST_REQUIRE(child >= 0);
   if (child == 0) {
      // The inherited application runtime owns vanished threads. Run the local
      // handle on a fresh child executor; the store must reject before touching
      // its inherited mutex, compute executor or lifecycle state.
      auto result = 2;
      try {
         auto runtime = forge::asio::runtime{{.worker_threads = 1}};
         forge::asio::blocking::run(runtime, metadata->status({}));
      } catch (const forge::plugins::crypto::secrets::exceptions::ownership_lost&) {
         result = 0;
      } catch (...) {
         result = 3;
      }
      ::_exit(result);
   }
   auto status = int{};
   auto waited = pid_t{};
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
   while (std::chrono::steady_clock::now() < deadline) {
      waited = ::waitpid(child, &status, WNOHANG);
      if (waited == child || (waited < 0 && errno != EINTR)) {
         break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
   }
   if (waited != child) {
      ::kill(child, SIGKILL);
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      BOOST_FAIL("Inherited managed handle hung in the forked child");
   }
   BOOST_REQUIRE(waited == child);
   BOOST_REQUIRE(WIFEXITED(status));
   BOOST_TEST(WEXITSTATUS(status) == 0);
   const auto parent = forge::asio::blocking::run(fixture.app->runtime(), metadata->status({}));
   BOOST_CHECK(parent.ready);
   BOOST_CHECK(!parent.fault);
   const auto material = std::string_view{"parent-material"};
   BOOST_CHECK(read_version(fixture, 1) == forge::crypto::core::bytes(material.begin(), material.end()));
}

BOOST_AUTO_TEST_CASE(ownership_replacement_fences_the_live_store_before_further_material_admission) {
   auto fixture = managed_fixture{};
   fixture.start();
   create_version(fixture, "original", 0, "original-material");
   const auto before = read_file(fixture.store_path());
   const auto lock = std::filesystem::path{fixture.store_path().string() + ".lock"};
   std::filesystem::rename(lock, fixture.directory / "original.lock");
   write_file(lock, {});
   std::filesystem::permissions(lock, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                std::filesystem::perm_options::replace);
   BOOST_CHECK_THROW(read_version(fixture, 1), forge::plugins::crypto::secrets::exceptions::ownership_lost);
   BOOST_CHECK_THROW(create_version(fixture, "after-owner-loss", 1, "never-admitted"),
                     forge::plugins::crypto::secrets::exceptions::store_unavailable);
   auto metadata = fixture.metadata();
   const auto status = forge::asio::blocking::run(fixture.app->runtime(), metadata->status({}));
   BOOST_CHECK(status.fault);
   BOOST_CHECK(!status.ready);
   BOOST_CHECK(read_file(fixture.store_path()) == before);
}

BOOST_AUTO_TEST_CASE(pending_count_and_input_byte_limits_reject_immediately_during_an_owned_commit) {
   for (const auto bytes_limited : {false, true}) {
      auto fixture = managed_fixture{};
      fixture.document.set("plugins.crypto.secrets.versions.max-pending-calls",
                           std::uint64_t{bytes_limited ? 16U : 1U});
      fixture.document.set("plugins.crypto.secrets.versions.max-pending-bytes",
                           std::uint64_t{bytes_limited ? 2048U : 1U << 20});
      fixture.start();
      auto writer = fixture.writer();
      auto fault = forge::tests::secrets::syscall_fault{fixture.store_path(),
                                                        forge::tests::secrets::failure_point::pause_after_publish};
      auto first = boost::asio::co_spawn(
          fixture.app->runtime().context(),
          writer->create_version(create_request("pending-first", 0, std::string(1200, 'p'))), boost::asio::use_future);
      BOOST_REQUIRE(fault.wait_until_published());
      auto second = boost::asio::co_spawn(
          fixture.app->runtime().context(),
          writer->create_version(create_request("pending-second", 1, std::string(1200, 'q'))), boost::asio::use_future);
      BOOST_REQUIRE(second.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
      BOOST_CHECK_THROW(second.get(), forge::plugins::crypto::secrets::exceptions::busy);
      fault.release();
      BOOST_REQUIRE(first.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
      BOOST_TEST(first.get().revision == 1U);
      BOOST_CHECK(!fault.pause_timed_out());
      const auto retry = create_version(fixture, "pending-second", 1, "admitted-after-drain");
      BOOST_TEST(retry.ref.version == 2U);
      auto metadata = fixture.metadata();
      const auto status = forge::asio::blocking::run(fixture.app->runtime(), metadata->status({}));
      BOOST_TEST(status.versions == 2U);
      BOOST_TEST(status.receipts == 2U);
   }
}

BOOST_AUTO_TEST_CASE(native_calls_preserve_caller_cancellation_filters_history_and_throw_policy) {
   for (const auto create : {false, true}) {
      for (const auto fail : {false, true}) {
         for (const auto total : {false, true}) {
            for (const auto throw_policy : {false, true}) {
               for (const auto prior_history : {false, true}) {
                  if (throw_policy && prior_history) {
                     continue;
                  }
                  auto fixture = managed_fixture{};
                  fixture.start();
                  create_version(fixture, "original", 0, "original-material");
                  auto cancellation = boost::asio::cancellation_signal{};
                  auto probe = boost::asio::co_spawn(
                      fixture.app->runtime().context(),
                      native_cancellation_probe(&fixture, &cancellation, create, fail, total, throw_policy,
                                                prior_history),
                      boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
                  BOOST_REQUIRE(probe.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
                  const auto result = probe.get();
                  BOOST_CHECK(result.expected_outcome);
                  BOOST_CHECK(result.throw_policy == throw_policy);
                  BOOST_CHECK(result.partial_history == prior_history);
                  BOOST_CHECK(result.partial_mask);
                  BOOST_CHECK(result.total_mask == total);
               }
            }
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(stop_and_caller_cancellation_after_rename_wait_for_commit_reconciliation) {
   auto fixture = managed_fixture{};
   fixture.start();
   create_version(fixture, "original", 0, "original-material");
   auto cancellation = boost::asio::cancellation_signal{};
   auto fault = forge::tests::secrets::syscall_fault{fixture.store_path(),
                                                     forge::tests::secrets::failure_point::pause_after_publish};
   auto mutation =
       boost::asio::co_spawn(fixture.app->runtime().context(), create_with_caller_history(&fixture),
                             boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
   BOOST_REQUIRE(fault.wait_until_published());
   auto cancelled = std::promise<void>{};
   auto cancellation_delivered = cancelled.get_future();
   boost::asio::post(fixture.app->runtime().context(), [&] {
      cancellation.emit(boost::asio::cancellation_type::partial);
      cancelled.set_value();
   });
   BOOST_REQUIRE(cancellation_delivered.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   fixture.app->request_stop();
   auto shutdown =
       boost::asio::co_spawn(fixture.app->runtime().context(), fixture.app->shutdown(), boost::asio::use_future);
   BOOST_CHECK(shutdown.wait_for(std::chrono::milliseconds{100}) == std::future_status::timeout);
   {
      auto competitor = managed_fixture{};
      competitor.document.set("plugins.crypto.secrets.versions.path", fixture.store_path().string());
      competitor.document.set("plugins.crypto.secrets.versions.bootstrap-passphrase-file",
                              (fixture.directory / "bootstrap").string());
      BOOST_CHECK_THROW(competitor.start("open_existing"),
                        forge::plugins::crypto::secrets::exceptions::store_unavailable);
   }
   BOOST_CHECK_THROW(create_version(fixture, "late-after-stop", 2, "never-admitted"),
                     forge::plugins::crypto::secrets::exceptions::stopped);
   fault.release();
   BOOST_REQUIRE(shutdown.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(shutdown.get());
   BOOST_REQUIRE(mutation.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   const auto completed = mutation.get();
   BOOST_CHECK(completed.partial_history);
   BOOST_CHECK(!completed.throw_policy);
   if (!completed.aborted) {
      BOOST_TEST(completed.result.ref.version == 2U);
      BOOST_TEST(completed.result.revision == 2U);
   }
   BOOST_CHECK(!fault.pause_timed_out());
   fixture.app.reset();
   fixture.start("open_existing");
   const auto before_replay = read_file(fixture.store_path());
   const auto replay = create_version(fixture, "stopped-after-rename", 1, "published-material");
   BOOST_TEST(replay.ref.version == 2U);
   BOOST_TEST(replay.revision == 2U);
   BOOST_CHECK(read_file(fixture.store_path()) == before_replay);
   const auto material = std::string_view{"published-material"};
   BOOST_CHECK(read_version(fixture, 2) == forge::crypto::core::bytes(material.begin(), material.end()));
}

BOOST_AUTO_TEST_SUITE_END()
