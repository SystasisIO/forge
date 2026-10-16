#include <forge/exceptions/macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

import forge.api.core.registry;
import forge.api.core.types;
import forge.app.application_builder;
import forge.app.application_shell;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.config.core.document;
import forge.config.core.value;
import forge.crypto.core.secret_bytes;
import forge.crypto.core.types;
import forge.exceptions;
import forge.plugins.crypto.secrets.exceptions;
import forge.plugins.crypto.secrets.plugin;
import forge.plugins.crypto.secrets.version_api;
import forge.plugins.crypto.secrets.version_metadata_api;
import forge.plugins.crypto.secrets.version_types;
import forge.plugins.crypto.secrets.version_write_api;

static_assert(!std::is_copy_constructible_v<forge::plugins::crypto::secrets::create_version_request>);
static_assert(forge::api::core::local_interface<forge::plugins::crypto::secrets::version_write_api>);
static_assert(!forge::api::core::remote_interface<forge::plugins::crypto::secrets::version_write_api>);

namespace {

struct consumer_fixture {
   consumer_fixture() {
      directory =
          std::filesystem::temp_directory_path() /
          ("forge-secrets-package-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      if (!std::filesystem::create_directory(directory)) {
         FORGE_THROW_EXCEPTION(forge::exceptions::context_error, "Cannot create package consumer directory");
      }

      std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                                   std::filesystem::perm_options::replace);
      std::filesystem::create_directory(directory / "store");
      std::filesystem::permissions(directory / "store", std::filesystem::perms::owner_all,
                                   std::filesystem::perm_options::replace);
      auto output = std::ofstream{directory / "bootstrap", std::ios::binary};
      output << "package-consumer-test-bootstrap\n";
      output.close();
      std::filesystem::permissions(directory / "bootstrap",
                                   std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                   std::filesystem::perm_options::replace);
      document.set("plugins.crypto.secrets.versions.enabled", true);
      document.set("plugins.crypto.secrets.versions.path", (directory / "store" / "versions.fks").string());
      document.set("plugins.crypto.secrets.versions.bootstrap-passphrase-file", (directory / "bootstrap").string());
      document.set("plugins.crypto.secrets.versions.ceiling.purposes",
                   forge::config::core::value::array_type{forge::config::core::value{"consumer.read"}});
      document.set("plugins.crypto.secrets.versions.ceiling.operations",
                   forge::config::core::value::array_type{forge::config::core::value{"get_bytes"}});
      document.set("plugins.crypto.secrets.versions.ceiling.allow-raw-export", true);
      document.set("plugins.crypto.secrets.versions.ceiling.max-plaintext-bytes", std::uint64_t{4096});
      document.set("plugins.crypto.secrets.versions.ceiling.max-ciphertext-bytes", std::uint64_t{8192});
      document.set("plugins.crypto.secrets.versions.ceiling.max-aad-bytes", std::uint64_t{4096});
   }

   ~consumer_fixture() {
      try {
         stop();
      } catch (...) {
      }

      auto error = std::error_code{};
      std::filesystem::remove_all(directory, error);
   }

   void start(std::string_view mode) {
      document.set("plugins.crypto.secrets.versions.mode", std::string{mode});
      auto descriptor = forge::plugins::crypto::secrets::plugin::descriptor();
      descriptor.enabled_by_default = true;
      auto builder = forge::app::application_builder{};
      builder.compute({.worker_threads = 1}).plugin(std::move(descriptor));
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

   std::filesystem::path directory;
   forge::config::core::document document;
   std::unique_ptr<forge::app::application_shell> app;
};

} // namespace

int main() {
   try {
      // Preserve the old installed-component descriptor smoke as well as the new behavior.
      const auto descriptor = forge::plugins::crypto::secrets::plugin::descriptor();
      if (descriptor.id.value != "forge.plugins.crypto.secrets") {
         return 1;
      }

      auto fixture = consumer_fixture{};
      fixture.start("create_new");
      auto writer = fixture.app->apis().get<forge::plugins::crypto::secrets::version_write_api>(
          forge::plugins::crypto::secrets::version_write_api::ref());
      auto metadata = fixture.app->apis().get<forge::plugins::crypto::secrets::version_metadata_api>(
          forge::plugins::crypto::secrets::version_metadata_api::ref());
      auto reader = fixture.app->apis().get<forge::plugins::crypto::secrets::version_api>(
          forge::plugins::crypto::secrets::version_api::ref());
      const auto material = std::string_view{"installed-consumer-material"};
      auto request = forge::plugins::crypto::secrets::create_version_request{
          .mutation_id = "consumer-create",
          .secret_id = "consumer/native",
          .policy = {.purposes = {"consumer.read"},
                     .operations = {forge::plugins::crypto::secrets::operation::get_bytes},
                     .allow_raw_export = true},
          .material = forge::crypto::core::secret_bytes{forge::crypto::core::bytes{material.begin(), material.end()}}};
      const auto created =
          forge::asio::blocking::run(fixture.app->runtime(), writer->create_version(std::move(request)));
      const auto description =
          forge::asio::blocking::run(fixture.app->runtime(), metadata->describe_version({created.ref}));
      const auto read = forge::asio::blocking::run(
          fixture.app->runtime(),
          reader->get_bytes({.version = created.ref.version, .request = {"consumer/native", "consumer.read"}}));
      if (created.ref.version != 1 || created.revision != 1 ||
          description.state != forge::plugins::crypto::secrets::version_state::available ||
          read.result.bytes != forge::crypto::core::bytes(material.begin(), material.end())) {
         return 1;
      }

      const auto revoked = forge::asio::blocking::run(
          fixture.app->runtime(),
          writer->revoke_version({.mutation_id = "consumer-revoke", .ref = created.ref, .expected_revision = 1}));
      if (revoked.revision != 2) {
         return 1;
      }

      fixture.stop();
      fixture.start("open_existing");
      auto reopened = fixture.app->apis().get<forge::plugins::crypto::secrets::version_api>(
          forge::plugins::crypto::secrets::version_api::ref());
      try {
         forge::asio::blocking::run(
             fixture.app->runtime(),
             reopened->get_bytes({.version = 1, .request = {"consumer/native", "consumer.read"}}));
         return 1;
      } catch (const forge::plugins::crypto::secrets::exceptions::version_revoked&) {
      }

      fixture.stop();
      return 0;
   } catch (...) {
      return 1;
   }
}
