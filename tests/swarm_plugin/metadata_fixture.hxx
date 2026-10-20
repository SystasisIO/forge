#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "../quic_p2p/libp2p_identity_fixture.hxx"

import forge.app.application_builder;
import forge.app.application_shell;
import forge.app.plugin;
import forge.app.plugin_registry;
import forge.api.core.registry;
import forge.api.core.handle;
import forge.asio.compute;
import forge.asio.runtime;
import forge.asio.task;
import forge.config.core.document;
import forge.config.core.value;
import forge.plugins.crypto.secrets.plugin;
import forge.plugins.db.store.api;
import forge.plugins.db.store.plugin;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.plugin;
import forge.net.p2p.endpoint;
import forge.plugins.swarm.node.api;
import forge.plugins.swarm.node.plugin;

namespace swarm_plugin_metadata_test {

namespace sn = forge::plugins::swarm::node;
namespace db_plugin = forge::plugins::db::store;
namespace p2p_plugin = forge::plugins::net::p2p::node;

// All four plugins are real. The caller-owned DB executor outlives every shell
// generation; Swarm uses App compute and never owns this test's worker pool.
class metadata_fixture {
 private:
   struct directory {
      std::filesystem::path path;
      directory() {
         auto pattern = (std::filesystem::temp_directory_path() / "forge-swarm-metadata-XXXXXX").string();
         const auto created = ::mkdtemp(pattern.data());
         if (!created) throw std::runtime_error{"metadata fixture mkdtemp failed"};
         path = std::filesystem::canonical(created);
      }
      ~directory() {
         auto error = std::error_code{};
         std::filesystem::remove_all(path, error);
         if (error) BOOST_ERROR("metadata fixture cleanup: " << error.message());
      }
   };
   directory _directory;
   boost::asio::thread_pool _database{1};
   const forge::tests::p2p::identity_fixture _identity = forge::tests::p2p::make_identity_fixture("swarm-metadata");
   const std::string _backend;
   sn::plugin* _swarm = nullptr;

 public:
   static constexpr auto metadata_name = "swarm-metadata";
   static constexpr auto peer_name = "p2p-cache";
   std::unique_ptr<forge::app::application_shell> shell;

   explicit metadata_fixture(std::string backend, bool include_swarm = true,
      std::string object_name = metadata_name) : _backend{std::move(backend)} {
      build(include_swarm, std::move(object_name));
   }
   ~metadata_fixture() {
      if (shell) {
         try { close(); }
         catch (...) {
            // Never release an active App/DB owner merely to finish a failed test.
            std::cerr << "metadata fixture could not join its application owners\n";
            std::terminate();
         }
         shell.reset();
      }
      _database.join();
   }
   metadata_fixture(const metadata_fixture&) = delete;
   metadata_fixture& operator=(const metadata_fixture&) = delete;

   template <typename T> T run(boost::asio::awaitable<T> operation, std::string_view phase = "metadata operation") {
      auto result = boost::asio::co_spawn(shell->runtime().context(), std::move(operation), boost::asio::use_future);
      return join(result, phase);
   }
   template <typename T> T run_database(boost::asio::awaitable<T> operation) {
      auto result = boost::asio::co_spawn(_database.get_executor(), std::move(operation), boost::asio::use_future);
      return join(result, "guarded DB operation on the caller-owned executor");
   }
   void start() { run(shell->startup(), "official DB/P2P/Swarm startup"); }
   void stop_swarm() {
      if (!_swarm) throw std::logic_error{"metadata fixture has no Swarm owner"};
      _swarm->request_stop();
      run(_swarm->shutdown(), "Swarm-only shutdown before shared DB stop");
   }
   void close() {
      shell->request_stop();
      auto failure = std::exception_ptr{};
      try { run(shell->shutdown(), "reverse plugin shutdown"); }
      catch (...) { failure = std::current_exception(); }
      // Always join the App scheduler, including after a failed startup/cleanup.
      run(shell->scheduler().shutdown(), "App scheduler join");
      if (failure) std::rethrow_exception(failure);
   }
   void reopen(bool include_swarm = true) {
      close();
      shell.reset();
      _swarm = nullptr;
      build(include_swarm, metadata_name);
   }
   auto api() { return shell->apis().get<sn::api>(sn::api::ref()).shared(); }
   auto database() { return shell->apis().get<db_plugin::api>(db_plugin::api::ref()).shared(); }
   auto node() { return shell->apis().get<p2p_plugin::api>(p2p_plugin::api::ref()).shared(); }

   boost::asio::awaitable<void> database_executor_probe() {
      // A round trip through the injected executor proves Swarm did not stop it.
      co_await boost::asio::co_spawn(_database.get_executor(), executor_work(), boost::asio::use_awaitable);
   }

 private:
   template <typename T> static T join(std::future<T>& result, std::string_view phase) {
      if (result.wait_for(std::chrono::seconds{10}) != std::future_status::ready) {
         std::cerr << "metadata fixture exceeded owned join budget: " << phase << '\n';
         std::terminate();
      }
      return result.get();
   }
   static boost::asio::awaitable<void> executor_work() { co_return; }
   static forge::config::core::value secret(std::string id, const std::string& bytes, std::string purpose) {
      using value = forge::config::core::value;
      return value::object_type{{"id", std::move(id)}, {"kind", "bytes"},
         {"source", value::object_type{{"type", "value"}, {"encoding", "raw"}, {"value", bytes}}},
         {"purposes", value::array_type{value{std::move(purpose)}}},
         {"operations", value::array_type{value{"get_bytes"}}}, {"allow-raw-export", true}};
   }
   forge::config::core::document configuration() const {
      using value = forge::config::core::value;
      const auto store = [&](const char* name) -> value {
         return value::object_type{{"name", name}, {"driver", _backend},
            {"path", (_directory.path / name).string()},
            {"object", value::object_type{{"family", "objectdb"}, {"write-policy", "single-writer"}}}};
      };
      auto document = forge::config::core::document{};
      document.set("plugins.db.store.stores", value::array_type{store(peer_name), store(metadata_name)});
      document.set("plugins.crypto.secrets.secrets", value::array_type{
         secret("identity/cert", _identity.certificate_pem, "p2p.identity.certificate"),
         secret("identity/key", _identity.private_key_pem, "p2p.identity.private-key")});
      document.set("plugins.net.p2p.node.peer-store.store", peer_name);
      document.set("plugins.net.p2p.node.identity.certificate-secret", "identity/cert");
      document.set("plugins.net.p2p.node.identity.private-key-secret", "identity/key");
      document.set("plugins.net.p2p.node.allow-insecure-test-mode", false);
      document.set("plugins.net.p2p.node.topology.mode", "static-only");
      document.set("plugins.net.p2p.node.listen", value::array_type{value{"/ip4/127.0.0.1/tcp/0"}});
      document.set("plugins.net.p2p.node.tcp.security", "noise");
      document.set("plugins.net.p2p.node.path.policy", "direct-only");
      document.set("plugins.net.p2p.node.relay.client-enabled", false);
      document.set("plugins.net.p2p.node.relay.server-enabled", false);
      document.set("plugins.net.p2p.node.relay.auto-discovery-enabled", false);
      return document;
   }
   void build(bool include_swarm, std::string object_name) {
      auto builder = forge::app::application_builder{};
      builder.name("swarm-metadata-" + _backend)
         .runtime(forge::asio::runtime_options{.worker_threads = 2})
         .compute(forge::asio::compute::pool::options{.worker_threads = 2})
         .plugin(db_plugin::plugin::descriptor())
         .plugin(forge::plugins::crypto::secrets::plugin::descriptor())
         .plugin(p2p_plugin::plugin::descriptor());
      if (include_swarm) {
         auto options = sn::plugin_options{};
         options.initial_config.free_admission = true;
         options.initial_config.metadata.mode = "object";
         options.initial_config.metadata.object_name = std::move(object_name);
         options.database = _database.get_executor();
         auto descriptor = sn::plugin::descriptor(options);
         auto factory = std::move(descriptor.factory);
         descriptor.factory = [this, factory = std::move(factory)] {
            auto result = factory();
            _swarm = static_cast<sn::plugin*>(result.get());
            return result;
         };
         builder.plugin(std::move(descriptor));
      }
      shell = std::move(builder).build();
      shell->configure(configuration());
   }
};

} // namespace swarm_plugin_metadata_test
