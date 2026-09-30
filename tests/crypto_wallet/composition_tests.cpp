#include <boost/test/unit_test.hpp>
#include <unistd.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

import forge.api.core.registry;
import forge.api.core.connection;
import forge.api.core.types;
import forge.app.events;
import forge.app.application_builder;
import forge.app.application_shell;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.app.signals;
import forge.asio.blocking;
import forge.asio.compute;
import forge.asio.runtime;
import forge.asio.task;
import forge.chain.api.block_signer;
import forge.chain.protocol.block_signing;
import forge.crypto.asymmetric;
import forge.crypto.keystore.store;
import forge.crypto.wallet.exceptions;
import forge.crypto.wallet.protocol;
import forge.config.core.document;
import forge.plugins.chain.signer.plugin;
import forge.plugins.chain.signer.types;
import forge.plugins.crypto.wallet.plugin;
import forge.plugins.crypto.wallet.provider_source;

namespace wallet_plugin = forge::plugins::crypto::wallet;
namespace signer_plugin = forge::plugins::chain::signer;
namespace protocol = forge::chain::protocol;
using forge::asio::blocking::run;

static_assert(forge::api::core::local_interface<wallet_plugin::provider_source>);
static_assert(!forge::api::core::remote_interface<wallet_plugin::provider_source>);

BOOST_AUTO_TEST_CASE(host_can_prohibit_wallet_http_publication_independently_of_yaml) {
   auto descriptor = wallet_plugin::plugin::descriptor({.allow_http_management = false});
   descriptor.enabled_by_default = true;
   auto builder = forge::app::application_builder{};
   builder.compute({.worker_threads = 1}).plugin(std::move(descriptor));
   auto app = std::move(builder).build();
   forge::config::core::document document;
   document.set("plugins.crypto.wallet.publish-http", true);
   BOOST_CHECK_THROW(app->configure(document), forge::crypto::wallet::exceptions::permission_denied);
}

BOOST_AUTO_TEST_CASE(application_schema_discovery_does_not_stop_injected_wallet_providers) {
   auto pattern = (std::filesystem::temp_directory_path() / "forge-wallet-discovery-XXXXXX").string();
   const auto* created = ::mkdtemp(pattern.data());
   BOOST_REQUIRE(created);
   struct cleanup {
      std::filesystem::path path;
      ~cleanup() {
         std::error_code error;
         std::filesystem::remove_all(path, error);
      }
   } directory{created};
   auto provider = std::make_shared<wallet_plugin::provider>("injected");
   auto descriptor = wallet_plugin::plugin::descriptor({.wallets = {provider}});
   descriptor.enabled_by_default = true;
   auto builder = forge::app::application_builder{};
   builder.compute({.worker_threads = 1}).plugin(std::move(descriptor));
   auto app = std::move(builder).build();
   static_cast<void>(app->describe_config());
   static_cast<void>(app->describe_config());
   forge::config::core::document document;
   document.set("plugins.crypto.wallet.directory", directory.path.string());
   app->configure(document);
   run(app->runtime(), app->startup());
   struct stop {
      forge::app::application_shell& app;
      ~stop() {
         run(app.runtime(), app.shutdown());
      }
   } shutdown{*app};
   auto source = app->apis().get<wallet_plugin::provider_source>(wallet_plugin::provider_source::ref());
   BOOST_TEST(source->get("injected") == provider);
   BOOST_CHECK(run(app->runtime(), provider->create("test-only")).status == forge::crypto::wallet::state::locked);
   BOOST_CHECK(run(app->runtime(), provider->unlock("test-only")).status == forge::crypto::wallet::state::unlocked);
   BOOST_TEST(run(app->runtime(), provider->create_key("key")).id == "key");
}

BOOST_AUTO_TEST_CASE(wallet_provider_source_connects_configured_names_in_dependency_order) {
   auto pattern = (std::filesystem::temp_directory_path() / "forge-wallet-composition-XXXXXX").string();
   const auto* created = ::mkdtemp(pattern.data());
   BOOST_REQUIRE(created != nullptr);
   struct cleanup {
      std::filesystem::path path;
      ~cleanup() {
         std::error_code error;
         std::filesystem::remove_all(path, error);
      }
   } directory{created};
   const auto wallet_name = std::string{"operator-7"};
   const auto key = forge::crypto::asymmetric::private_key::generate();
   const auto public_key = key.get_public_key();
   {
      auto store = forge::crypto::keystore::store::create(directory.path / (wallet_name + ".fks"), "test-only");
      store.put({"block"}, key);
      store.save();
   }
   const auto chain = forge::crypto::digest::sha256::hash(std::string{"wallet-composition"});
   auto resolved_names = std::vector<std::string>{};
   auto options = signer_plugin::plugin_options{};
   options.initial_config.block_profiles = {
       {.name = "producer",
        .chain_id = chain.str(),
        .producer = "alice",
        .signing = {{wallet_name, "block", forge::crypto::asymmetric::encoding::forge().format(public_key)}},
        .allow_local = true}};
   options.resolve_provider = [&](std::string_view name, const forge::api::core::view& apis) {
      resolved_names.emplace_back(name);
      return apis.get<wallet_plugin::provider_source>(wallet_plugin::provider_source::ref())->get(std::string{name});
   };
   auto signer = signer_plugin::descriptor(options);
   signer.enabled_by_default = true;
   signer.dependencies.push_back({"forge.plugins.crypto.wallet"});
   auto wallet = wallet_plugin::plugin::descriptor(
       {.initial_config = {.directory = directory.path.string(), .wallets = {{.name = wallet_name, .open = true}}}});
   wallet.enabled_by_default = true;
   auto registry = forge::app::plugin_registry{};
   // Reverse registration order: dependencies own initialization order.
   registry.register_plugin(std::move(signer));
   registry.register_plugin(std::move(wallet));
   auto plugins = registry.instantiate_enabled({});
   BOOST_REQUIRE_EQUAL(plugins.size(), 2U);
   BOOST_TEST(plugins.front()->id().value == "forge.plugins.crypto.wallet");
   auto runtime = forge::asio::runtime{{.worker_threads = 2}};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto compute = forge::asio::compute::pool{{.worker_threads = 2}};
   auto apis = forge::api::core::registry{};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto installer = forge::api::core::installer{apis};
   auto context = forge::app::plugin_context{scheduler, apis, signals, events, nullptr, {}, compute.get_executor()};
   for (auto& value : plugins) {
      run(runtime, value->provide(installer));
   }
   for (auto& value : plugins) {
      run(runtime, value->initialize(context));
   }
   for (auto& value : plugins) {
      run(runtime, value->startup());
   }
   struct stop {
      forge::asio::runtime& runtime;
      std::vector<std::unique_ptr<forge::app::plugin>>& plugins;
      ~stop() {
         for (auto it = plugins.rbegin(); it != plugins.rend(); ++it) {
            (*it)->request_stop();
            run(runtime, (*it)->shutdown());
         }
      }
   } shutdown{runtime, plugins};
   BOOST_REQUIRE_EQUAL(resolved_names.size(), 1U);
   BOOST_TEST(resolved_names.front() == wallet_name);
   auto source = apis.get<wallet_plugin::provider_source>(wallet_plugin::provider_source::ref());
   auto provider = source->get(wallet_name);
   auto lifecycle = std::dynamic_pointer_cast<wallet_plugin::provider>(provider);
   BOOST_REQUIRE(lifecycle);
   BOOST_CHECK(run(runtime, lifecycle->status()).status == forge::crypto::wallet::state::locked);
   BOOST_CHECK_THROW(source->get("unknown-wallet"), forge::crypto::wallet::exceptions::not_found);
   BOOST_TEST(!std::filesystem::exists(directory.path / "unknown-wallet.fks"));
   auto block_signer = apis.get<forge::chain::api::block_signer>(forge::chain::api::block_signer::ref());
   auto request = protocol::block_sign_request{.chain = chain, .keys = {public_key}};
   request.header.producer = protocol::name{"alice"};
   BOOST_CHECK_THROW(run(runtime, block_signer->sign(request, {})), std::exception);
   run(runtime, lifecycle->unlock("test-only"));
   const auto signatures = run(runtime, block_signer->sign(request, {}));
   BOOST_REQUIRE_EQUAL(signatures.size(), 1U);
   BOOST_CHECK(forge::crypto::asymmetric::recover(signatures.front(), protocol::calculate_block_id(request.header)) ==
               public_key);
   run(runtime, lifecycle->lock());
   BOOST_TEST(source->get(wallet_name) == provider);
   BOOST_CHECK_THROW(run(runtime, block_signer->sign(request, {})), std::exception);
   run(runtime, lifecycle->unlock("test-only"));
   BOOST_TEST(source->get(wallet_name) == provider);
   BOOST_TEST(run(runtime, block_signer->sign(request, {})).size() == 1U);
   BOOST_TEST(resolved_names.size() == 1U);
}
