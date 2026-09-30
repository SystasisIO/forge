#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/api/core/macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../quic_p2p/libp2p_identity_fixture.hxx"

import forge.api.core.binding;
import forge.api.core.connection;
import forge.api.core.exceptions;
import forge.api.core.registry;
import forge.api.core.types;
import forge.api.transport.connection;
import forge.app.application;
import forge.app.application_shell;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.asio.blocking;
import forge.asio.notification;
import forge.asio.runtime;
import forge.config.core.document;
import forge.config.core.value;
import forge.crypto.digest.sha256;
import forge.exceptions;
import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.p2p.protocol;
import forge.api.p2p.publication;
import forge.plugins.crypto.secrets.api;
import forge.plugins.crypto.secrets.types;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.plugin;
import forge.plugins.net.p2p.resolver.api;
import forge.plugins.net.p2p.resolver.exceptions;
import forge.plugins.net.p2p.resolver.managed_api;
import forge.plugins.net.p2p.resolver.plugin;
import forge.plugins.net.p2p.resolver.types;

#include "details/managed_remote_state.hxx"
#include "details/managed_remote_invoker.hxx"

namespace {

namespace crypto_secrets = forge::plugins::crypto::secrets;

class test_api : public forge::api::core::contract<test_api, forge::api::core::surface::local |
                                                                 forge::api::core::surface::remote> {
 public:
   virtual ~test_api() = default;
   virtual boost::asio::awaitable<int> ping(int request) = 0;
};

class test_api_impl final : public test_api {
 public:
   explicit test_api_impl(int increment, std::chrono::milliseconds first_call_delay = {})
       : increment_{increment}, first_call_delay_{first_call_delay} {}

   boost::asio::awaitable<int> ping(int request) override {
      const auto call = calls_.fetch_add(1, std::memory_order_relaxed) + 1U;
      if (call == 1U && first_call_delay_.count() != 0) {
         auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor, first_call_delay_};
         co_await timer.async_wait(boost::asio::use_awaitable);
      }
      co_return request + increment_;
   }

   [[nodiscard]] std::uint64_t calls() const noexcept {
      return calls_.load(std::memory_order_relaxed);
   }

 private:
   int increment_ = 0;
   std::chrono::milliseconds first_call_delay_{};
   std::atomic_uint64_t calls_{0};
};

class dependency_plugin : public forge::app::plugin {
 public:
   explicit dependency_plugin(std::string value) : id_{std::move(value)} {}

   [[nodiscard]] forge::app::plugin_id id() const override {
      return {.value = id_};
   }

   [[nodiscard]] std::string version() const override {
      return "test";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context&) override {
      co_return;
   }

   boost::asio::awaitable<void> startup() override {
      co_return;
   }

   boost::asio::awaitable<void> shutdown() override {
      co_return;
   }

 private:
   std::string id_;
};

class secrets_api final : public crypto_secrets::api {
 public:
   explicit secrets_api(std::shared_ptr<const forge::tests::p2p::identity_fixture> identity)
       : identity_{std::move(identity)} {}

   boost::asio::awaitable<crypto_secrets::snapshot> status(crypto_secrets::query) override {
      co_return crypto_secrets::snapshot{.configured_secrets = 2};
   }

   boost::asio::awaitable<crypto_secrets::get_result> get_bytes(crypto_secrets::get_request request) override {
      const auto* material = request.secret_id == "p2p/test-certificate"   ? &identity_->certificate_pem
                             : request.secret_id == "p2p/test-private-key" ? &identity_->private_key_pem
                                                                           : nullptr;
      if (material == nullptr) {
         throw std::runtime_error{"unknown test secret"};
      }
      auto bytes = std::vector<std::uint8_t>{};
      bytes.reserve(material->size());
      for (const auto value : *material) {
         bytes.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(value)));
      }
      co_return crypto_secrets::get_result{.secret_id = std::move(request.secret_id), .bytes = std::move(bytes)};
   }

   boost::asio::awaitable<crypto_secrets::derive_result> derive_hkdf_sha256(crypto_secrets::derive_request) override {
      throw std::logic_error{"not used"};
   }

   boost::asio::awaitable<crypto_secrets::aead_encrypt_result>
   encrypt_aes_gcm(crypto_secrets::aead_encrypt_request) override {
      throw std::logic_error{"not used"};
   }

   boost::asio::awaitable<crypto_secrets::aead_decrypt_result>
   decrypt_aes_gcm(crypto_secrets::aead_decrypt_request) override {
      throw std::logic_error{"not used"};
   }

 private:
   std::shared_ptr<const forge::tests::p2p::identity_fixture> identity_;
};

class secrets_plugin final : public dependency_plugin {
 public:
   explicit secrets_plugin(std::shared_ptr<const forge::tests::p2p::identity_fixture> identity)
       : dependency_plugin{"forge.plugins.crypto.secrets"}, identity_{std::move(identity)} {}

   boost::asio::awaitable<void> provide(forge::api::core::provider& provider) override {
      provider.install<crypto_secrets::api>(std::make_shared<secrets_api>(identity_));
      co_return;
   }

 private:
   std::shared_ptr<const forge::tests::p2p::identity_fixture> identity_;
};

void register_p2p(forge::app::plugin_registry& registry,
                  std::shared_ptr<const forge::tests::p2p::identity_fixture> identity) {
   registry.register_plugin(forge::app::plugin_descriptor{
       .id = {.value = "forge.plugins.db.store"},
       .factory = [] { return std::make_unique<dependency_plugin>("forge.plugins.db.store"); },
   });
   registry.register_plugin(forge::app::plugin_descriptor{
       .id = {.value = "forge.plugins.crypto.secrets"},
       .factory = [identity = std::move(identity)] { return std::make_unique<secrets_plugin>(identity); },
   });
   registry.register_plugin(forge::plugins::net::p2p::node::descriptor());
   registry.register_plugin(forge::plugins::net::p2p::resolver::descriptor());
}

class publisher_plugin final : public forge::app::plugin {
 public:
   explicit publisher_plugin(std::size_t max_inflight) : max_inflight_{max_inflight} {}

   [[nodiscard]] forge::app::plugin_id id() const override {
      return {.value = "managed-remote-publisher"};
   }

   [[nodiscard]] std::string version() const override {
      return "test";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      auto resolver = context.apis().get<forge::plugins::net::p2p::resolver::api>(
          {.id = {"forge.plugins.net.p2p.resolver"}, .major = 2, .min_revision = 0});
      auto plan = forge::api::core::binding()
                      .serve(context.apis())
                      .export_api<test_api>({.id = {"managed.test"}, .major = 1, .min_revision = 0})
                      .build();
      auto options = forge::plugins::net::p2p::resolver::publish_options{};
      options.transport.max_inflight = max_inflight_;
      publication_ = resolver->publish_api(std::move(plan), {.value = "/forge/api/managed-test/1"}, options);
      co_return;
   }

   boost::asio::awaitable<void> startup() override {
      co_return;
   }

   boost::asio::awaitable<void> shutdown() override {
      co_await publication_.async_close();
      co_return;
   }

 private:
   std::size_t max_inflight_ = 128;
   forge::api::p2p::publication publication_;
};

class test_application final : public forge::app::application_shell {
 public:
   explicit test_application(std::string identity_name, std::shared_ptr<test_api> api = {},
                             std::size_t max_inflight = 128)
       : test_application{std::make_shared<const forge::tests::p2p::identity_fixture>(
                              forge::tests::p2p::make_identity_fixture(std::move(identity_name))),
                          std::move(api), max_inflight} {}

   explicit test_application(std::shared_ptr<const forge::tests::p2p::identity_fixture> identity,
                             std::shared_ptr<test_api> api = {}, std::size_t max_inflight = 128)
       : identity_{std::move(identity)},
         peer_{forge::net::p2p::make_peer_id_from_certificate_pem(identity_->certificate_pem)}, api_{std::move(api)},
         max_inflight_{max_inflight} {}

   [[nodiscard]] const forge::net::p2p::peer_id& peer() const noexcept {
      return peer_;
   }

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      register_p2p(registry, identity_);
      if (api_) {
         registry.register_plugin(forge::app::plugin_descriptor{
             .id = {.value = "managed-remote-publisher"},
             .dependencies = {{.value = "forge.plugins.net.p2p.resolver"}},
             .factory = [max_inflight = max_inflight_] { return std::make_unique<publisher_plugin>(max_inflight); },
         });
      }
   }

   boost::asio::awaitable<void> on_provide(forge::app::application_context& context) override {
      if (api_) {
         context.apis().install<test_api>(test_api::describe(), api_);
      }
      co_return;
   }

 private:
   std::shared_ptr<const forge::tests::p2p::identity_fixture> identity_;
   forge::net::p2p::peer_id peer_;
   std::shared_ptr<test_api> api_;
   std::size_t max_inflight_ = 128;
};

[[nodiscard]] forge::net::p2p::peer_id unavailable_peer(std::uint8_t seed) {
   return forge::net::p2p::make_peer_id(
       {.type = forge::net::p2p::public_key::type::ed25519, .data = std::vector<std::uint8_t>(32, seed)});
}

[[nodiscard]] forge::config::core::document test_config(const test_application& app) {
   auto value = forge::config::core::document{};
   value.set("plugins.net.p2p.node.allow-insecure-test-mode", true);
   value.set("plugins.net.p2p.node.identity.certificate-secret", "p2p/test-certificate");
   value.set("plugins.net.p2p.node.identity.private-key-secret", "p2p/test-private-key");
   value.set("plugins.net.p2p.node.peer-id", app.peer().to_string());
   return value;
}

[[nodiscard]] std::string listen_endpoint(test_application& app) {
   const auto endpoint =
       app.apis()
           .get<forge::plugins::net::p2p::node::api>({.id = {"forge.plugins.net.p2p.node"}, .major = 2, .min_revision = 0})
           ->local_endpoint();
   BOOST_REQUIRE(endpoint.has_value());
   return endpoint->to_string();
}

[[nodiscard]] std::string listen_address(test_application& app) {
   const auto endpoint =
       app.apis()
           .get<forge::plugins::net::p2p::node::api>({.id = {"forge.plugins.net.p2p.node"}, .major = 2, .min_revision = 0})
           ->local_endpoint();
   BOOST_REQUIRE(endpoint.has_value());
   auto address = *endpoint;
   address.peer.reset();
   return address.to_string();
}

} // namespace

FORGE_API(::test_api, FORGE_API_CONTRACT("managed.test", 1, 0), FORGE_API_METHOD(ping))

BOOST_AUTO_TEST_CASE(managed_remote_state_completion_is_atomic_before_waiter_wake) {
   using state_type = forge::plugins::net::p2p::resolver::detail::managed_remote_state;

   auto runtime = forge::asio::runtime{};
   auto state = state_type{2};
   const auto first = state.acquire_or_join(runtime.context().get_executor());
   BOOST_REQUIRE(static_cast<int>(first.status) == static_cast<int>(state_type::acquire_status::joined));
   BOOST_REQUIRE(first.start);
   BOOST_REQUIRE(first.flight);

   auto generation = std::make_shared<forge::plugins::net::p2p::resolver::detail::managed_remote_generation>();
   auto subscribed = std::promise<void>{};
   auto subscribed_future = subscribed.get_future();
   auto waiter = boost::asio::co_spawn(
       first.flight->executor(),
       [&]() -> boost::asio::awaitable<std::pair<bool, bool>> {
          const auto executor = co_await boost::asio::this_coro::executor;
          boost::asio::post(executor, [&subscribed] { subscribed.set_value(); });
          const auto delivered = co_await first.flight->completed().async_wait(first.observed);
          const auto snapshot = state.read_completion(first.flight);
          const auto current = state.acquire_or_join(executor);
          co_return std::pair{
              delivered > first.observed && snapshot.done && snapshot.result == generation && !snapshot.error,
              current.status == state_type::acquire_status::current && current.current == generation,
          };
       },
       boost::asio::use_future);
   subscribed_future.get();
   static_cast<void>(state.complete(first.flight, generation, {}, {}));

   const auto observed = waiter.get();
   BOOST_TEST(observed.first);
   BOOST_TEST(observed.second);
   state.leave(first.flight);
}

BOOST_AUTO_TEST_CASE(managed_remote_invoker_launch_failure_completes_waiters) {
   auto runtime = forge::asio::runtime{};
   auto invoker = forge::plugins::net::p2p::resolver::detail::managed_remote_invoker{
       {},
       {unavailable_peer(208)},
       forge::api::core::api_ref{.id = {"managed.test"}, .major = 1, .min_revision = 0},
       test_api::describe(),
       forge::plugins::net::p2p::resolver::managed_remote_options{.max_connect_rounds = 1},
       1,
   };

   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, invoker.connect_initial()), std::bad_weak_ptr);
   invoker.request_stop();
   forge::asio::blocking::run(runtime, invoker.async_stop());
}

BOOST_AUTO_TEST_CASE(managed_remote_state_completed_error_does_not_lose_waiters) {
   using state_type = forge::plugins::net::p2p::resolver::detail::managed_remote_state;

   auto runtime = forge::asio::runtime{};
   auto state = state_type{2};
   const auto first = state.acquire_or_join(runtime.context().get_executor());
   BOOST_REQUIRE(first.flight);

   auto failure = std::make_exception_ptr(std::runtime_error{"reconnect failed"});
   static_cast<void>(state.complete(first.flight, {}, failure, {}));

   const auto late = state.acquire_or_join(runtime.context().get_executor());
   BOOST_REQUIRE(static_cast<int>(late.status) == static_cast<int>(state_type::acquire_status::draining));
   BOOST_TEST(!late.start);
   auto drained = boost::asio::co_spawn(
       late.flight->executor(), late.flight->cleanup_completed().async_wait(late.observed), boost::asio::use_future);

   state.leave(first.flight);
   state.finish_watcher(first.flight);
   BOOST_CHECK(drained.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);
   state.finish_child(first.flight, {});
   BOOST_TEST(drained.get() > late.observed);
   state.leave(late.flight);
   const auto retry = state.acquire_or_join(runtime.context().get_executor());
   BOOST_TEST(retry.start);
   state.leave(retry.flight);
}

BOOST_AUTO_TEST_CASE(managed_remote_state_stop_rejects_unpublished_generation) {
   using state_type = forge::plugins::net::p2p::resolver::detail::managed_remote_state;

   auto runtime = forge::asio::runtime{};
   auto state = state_type{1};
   const auto acquired = state.acquire_or_join(runtime.context().get_executor());
   BOOST_REQUIRE(static_cast<int>(acquired.status) == static_cast<int>(state_type::acquire_status::joined));
   BOOST_REQUIRE(acquired.flight);

   const auto stopped = state.request_stop();
   BOOST_REQUIRE(stopped.initiated);
   BOOST_TEST(stopped.flight == acquired.flight);

   auto generation = std::make_shared<forge::plugins::net::p2p::resolver::detail::managed_remote_generation>();
   auto stopped_error = std::make_exception_ptr(std::runtime_error{"managed remote stopped"});
   const auto completed = state.complete(acquired.flight, generation, {}, stopped_error);
   BOOST_TEST(completed.canceled == generation);

   const auto snapshot = state.read_completion(acquired.flight);
   BOOST_REQUIRE(snapshot.done);
   BOOST_TEST(!snapshot.result);
   BOOST_REQUIRE(snapshot.error);
   BOOST_REQUIRE(snapshot.stopped);

   const auto after = state.acquire_or_join(runtime.context().get_executor());
   BOOST_TEST(static_cast<int>(after.status) == static_cast<int>(state_type::acquire_status::stopped));
   state.leave(acquired.flight);
}

BOOST_AUTO_TEST_CASE(managed_remote_is_sticky_and_fails_over_without_replay) {
   auto first_api = std::make_shared<test_api_impl>(1);
   auto second_api = std::make_shared<test_api_impl>(2);

   auto first = test_application{"managed-remote-failover-first", first_api};
   const auto first_peer = first.peer();
   auto first_config = test_config(first);
   first_config.set("plugins.net.p2p.node.listen",
                    forge::config::core::value::array_type{forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});
   first.configure(first_config);
   forge::asio::blocking::run(first.runtime(), first.startup());

   auto second_identity = std::make_shared<const forge::tests::p2p::identity_fixture>(
       forge::tests::p2p::make_identity_fixture("managed-remote-failover-second"));
   auto second = test_application{second_identity, second_api};
   const auto second_peer = second.peer();
   auto second_config = test_config(second);
   second_config.set("plugins.net.p2p.node.listen", forge::config::core::value::array_type{
                                                    forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});
   second.configure(second_config);
   forge::asio::blocking::run(second.runtime(), second.startup());
   const auto second_endpoint = listen_endpoint(second);
   const auto second_listen = listen_address(second);

   auto client = test_application{"managed-remote-failover-client"};
   auto client_config = test_config(client);
   client_config.set("plugins.net.p2p.node.bootstrap", forge::config::core::value::array_type{
                                                       forge::config::core::value{listen_endpoint(first)},
                                                       forge::config::core::value{second_endpoint},
                                                   });
   client_config.set("plugins.net.p2p.resolver.managed.max-waiters", std::uint64_t{2});
   client.configure(client_config);
   forge::asio::blocking::run(client.runtime(), client.startup());

   auto managed = client.apis().get<forge::plugins::net::p2p::resolver::managed_api>(
       {.id = {"forge.plugins.net.p2p.resolver.managed"}, .major = 1, .min_revision = 0});
   auto remote = forge::asio::blocking::run(
       client.runtime(), managed->remote<test_api>({first_peer, second_peer},
                                                   {
                                                       .resolution =
                                                           {
                                                               .query_deadline = std::chrono::milliseconds{100},
                                                               .open_deadline = std::chrono::milliseconds{100},
                                                           },
                                                       .max_connect_rounds = 16,
                                                       .initial_backoff = std::chrono::milliseconds{10},
                                                       .max_backoff = std::chrono::milliseconds{10},
                                                   }));

   BOOST_TEST(forge::asio::blocking::run(client.runtime(), remote->ping(40)) == 41);
   BOOST_TEST(first_api->calls() == 1U);
   BOOST_TEST(second_api->calls() == 0U);

   forge::asio::blocking::run(second.runtime(), second.shutdown());
   forge::asio::blocking::run(first.runtime(), first.shutdown());
   BOOST_CHECK_THROW(forge::asio::blocking::run(client.runtime(), remote->ping(40)), forge::exceptions::base);
   BOOST_TEST(first_api->calls() == 1U);
   BOOST_TEST(second_api->calls() == 0U);

   auto first_follower = boost::asio::co_spawn(client.runtime().context(), remote->ping(40), boost::asio::use_future);
   auto second_follower = boost::asio::co_spawn(client.runtime().context(), remote->ping(40), boost::asio::use_future);
   std::this_thread::sleep_for(std::chrono::milliseconds{50});
   BOOST_CHECK_THROW(forge::asio::blocking::run(client.runtime(), remote->ping(40)),
                     forge::api::core::exceptions::resource_exhausted);

   auto replacement = test_application{second_identity, second_api};
   auto replacement_config = test_config(replacement);
   replacement_config.set("plugins.net.p2p.node.listen",
                          forge::config::core::value::array_type{forge::config::core::value{second_listen}});
   replacement.configure(replacement_config);
   forge::asio::blocking::run(replacement.runtime(), replacement.startup());

   auto first_result = 0;
   auto second_result = 0;
   try {
      first_result = first_follower.get();
   } catch (const forge::exceptions::base& error) {
      BOOST_ERROR("first managed remote follower failed: " << error.what());
   }
   try {
      second_result = second_follower.get();
   } catch (const forge::exceptions::base& error) {
      BOOST_ERROR("second managed remote follower failed: " << error.what());
   }
   BOOST_TEST(first_result == 42);
   BOOST_TEST(second_result == 42);
   BOOST_TEST(first_api->calls() == 1U);
   BOOST_TEST(second_api->calls() == 2U);

   forge::asio::blocking::run(client.runtime(), client.shutdown());
   BOOST_CHECK_THROW(forge::asio::blocking::run(client.runtime(), remote->ping(40)),
                     forge::plugins::net::p2p::resolver::exceptions::remote_stopped);
   forge::asio::blocking::run(replacement.runtime(), replacement.shutdown());
}

BOOST_AUTO_TEST_CASE(managed_remote_supports_concurrent_first_calls_on_fresh_generation) {
   auto server_api = std::make_shared<test_api_impl>(1);
   auto server = test_application{"managed-remote-concurrent-server", server_api};
   const auto server_peer = server.peer();
   auto server_config = test_config(server);
   server_config.set("plugins.net.p2p.node.listen", forge::config::core::value::array_type{
                                                    forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});
   server.configure(server_config);
   forge::asio::blocking::run(server.runtime(), server.startup());

   auto client = test_application{"managed-remote-concurrent-client"};
   auto client_config = test_config(client);
   client_config.set("plugins.net.p2p.node.bootstrap",
                     forge::config::core::value::array_type{forge::config::core::value{listen_endpoint(server)}});
   client.configure(client_config);
   forge::asio::blocking::run(client.runtime(), client.startup());

   auto managed = client.apis().get<forge::plugins::net::p2p::resolver::managed_api>(
       {.id = {"forge.plugins.net.p2p.resolver.managed"}, .major = 1, .min_revision = 0});
   auto remote = forge::asio::blocking::run(client.runtime(),
                                            managed->remote<test_api>({server_peer}, {.max_connect_rounds = 1}));
   auto first = boost::asio::co_spawn(client.runtime().context(), remote->ping(40), boost::asio::use_future);
   auto second = boost::asio::co_spawn(client.runtime().context(), remote->ping(40), boost::asio::use_future);

   BOOST_TEST(first.get() == 41);
   BOOST_TEST(second.get() == 41);
   BOOST_TEST(server_api->calls() == 2U);

   forge::asio::blocking::run(client.runtime(), client.shutdown());
   forge::asio::blocking::run(server.runtime(), server.shutdown());
}

BOOST_AUTO_TEST_CASE(managed_remote_rejects_invalid_peer_sets) {
   auto app = test_application{"managed-remote-invalid-peers"};
   auto config = test_config(app);
   config.set("plugins.net.p2p.resolver.managed.max-peers", std::uint64_t{1});
   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.startup());

   auto managed = app.apis().get<forge::plugins::net::p2p::resolver::managed_api>(
       {.id = {"forge.plugins.net.p2p.resolver.managed"}, .major = 1, .min_revision = 0});
   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), managed->remote<test_api>({})),
                     forge::plugins::net::p2p::resolver::exceptions::invalid_remote);
   const auto peer = unavailable_peer(205);
   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), managed->remote<test_api>({peer, peer})),
                     forge::plugins::net::p2p::resolver::exceptions::invalid_remote);
   BOOST_CHECK_THROW(forge::asio::blocking::run(
                         app.runtime(), managed->remote<test_api>({unavailable_peer(206), unavailable_peer(207)})),
                     forge::plugins::net::p2p::resolver::exceptions::invalid_remote);

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(managed_remote_preserves_caller_cancellation) {
   auto app = test_application{"managed-remote-cancellation"};
   app.configure(test_config(app));
   forge::asio::blocking::run(app.runtime(), app.startup());

   auto managed = app.apis().get<forge::plugins::net::p2p::resolver::managed_api>(
       {.id = {"forge.plugins.net.p2p.resolver.managed"}, .major = 1, .min_revision = 0});
   auto cancellation = boost::asio::cancellation_signal{};
   auto pending =
       boost::asio::co_spawn(app.runtime().context(),
                             managed->remote<test_api>({unavailable_peer(209)},
                                                       {
                                                           .resolution = {.query_deadline = std::chrono::seconds{5},
                                                                          .open_deadline = std::chrono::seconds{5}},
                                                           .max_connect_rounds = 8,
                                                           .initial_backoff = std::chrono::seconds{1},
                                                           .max_backoff = std::chrono::seconds{1},
                                                       }),
                             boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
   std::this_thread::sleep_for(std::chrono::milliseconds{50});
   cancellation.emit(boost::asio::cancellation_type::all);

   BOOST_CHECK_THROW((void)pending.get(), forge::api::core::exceptions::cancelled);
   const auto shutdown_started = std::chrono::steady_clock::now();
   forge::asio::blocking::run(app.runtime(), app.shutdown());
   const auto shutdown_was_prompt = std::chrono::steady_clock::now() - shutdown_started < std::chrono::seconds{2};
   BOOST_TEST(shutdown_was_prompt);
}

BOOST_AUTO_TEST_CASE(managed_remote_keeps_healthy_session_after_call_deadline) {
   auto first_api = std::make_shared<test_api_impl>(1, std::chrono::seconds{5});
   auto second_api = std::make_shared<test_api_impl>(2);

   auto first = test_application{"managed-remote-deadline-first", first_api, 1};
   const auto first_peer = first.peer();
   auto first_config = test_config(first);
   first_config.set("plugins.net.p2p.node.listen",
                    forge::config::core::value::array_type{forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});
   first.configure(first_config);
   forge::asio::blocking::run(first.runtime(), first.startup());

   auto second = test_application{"managed-remote-deadline-second", second_api};
   const auto second_peer = second.peer();
   auto second_config = test_config(second);
   second_config.set("plugins.net.p2p.node.listen", forge::config::core::value::array_type{
                                                    forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});
   second.configure(second_config);
   forge::asio::blocking::run(second.runtime(), second.startup());

   auto client = test_application{"managed-remote-deadline-client"};
   auto client_config = test_config(client);
   client_config.set("plugins.net.p2p.node.bootstrap", forge::config::core::value::array_type{
                                                       forge::config::core::value{listen_endpoint(first)},
                                                       forge::config::core::value{listen_endpoint(second)},
                                                   });
   client.configure(client_config);
   forge::asio::blocking::run(client.runtime(), client.startup());

   auto managed = client.apis().get<forge::plugins::net::p2p::resolver::managed_api>(
       {.id = {"forge.plugins.net.p2p.resolver.managed"}, .major = 1, .min_revision = 0});
   auto remote = forge::asio::blocking::run(
       client.runtime(),
       managed->remote<test_api>({first_peer, second_peer},
                                 {
                                     .resolution = {.request_deadline = std::chrono::milliseconds{100}},
                                     .max_connect_rounds = 1,
                                 }));

   auto delayed = boost::asio::co_spawn(client.runtime().context(), remote->ping(40), boost::asio::use_future);
   for (auto count = 0; count < 500 && first_api->calls() == 0U; ++count) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
   }
   BOOST_REQUIRE_EQUAL(first_api->calls(), 1U);
   BOOST_CHECK_THROW(forge::asio::blocking::run(client.runtime(), remote->ping(40)),
                     forge::api::core::exceptions::resource_exhausted);
   BOOST_CHECK_THROW((void)delayed.get(), forge::api::core::exceptions::deadline_exceeded);
   BOOST_TEST(first_api->calls() == 1U);

   BOOST_TEST(forge::asio::blocking::run(client.runtime(), remote->ping(40)) == 41);
   BOOST_TEST(first_api->calls() == 2U);
   BOOST_TEST(second_api->calls() == 0U);

   forge::asio::blocking::run(client.runtime(), client.shutdown());
   forge::asio::blocking::run(first.runtime(), first.shutdown());
   forge::asio::blocking::run(second.runtime(), second.shutdown());
}

BOOST_AUTO_TEST_CASE(managed_remote_bounds_reconnect_waiters) {
   auto server_api = std::make_shared<test_api_impl>(1);
   auto server = test_application{"managed-remote-waiters-server", server_api};
   const auto server_peer = server.peer();
   auto server_config = test_config(server);
   server_config.set("plugins.net.p2p.node.listen", forge::config::core::value::array_type{
                                                    forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});
   server.configure(server_config);
   forge::asio::blocking::run(server.runtime(), server.startup());

   auto client = test_application{"managed-remote-waiters-client"};
   auto client_config = test_config(client);
   client_config.set("plugins.net.p2p.node.bootstrap", forge::config::core::value::array_type{
                                                       forge::config::core::value{listen_endpoint(server)},
                                                   });
   client_config.set("plugins.net.p2p.resolver.managed.max-waiters", std::uint64_t{1});
   client.configure(client_config);
   forge::asio::blocking::run(client.runtime(), client.startup());

   auto managed = client.apis().get<forge::plugins::net::p2p::resolver::managed_api>(
       {.id = {"forge.plugins.net.p2p.resolver.managed"}, .major = 1, .min_revision = 0});
   auto remote = forge::asio::blocking::run(
       client.runtime(),
       managed->remote<test_api>({server_peer}, {
                                                    .resolution = {.query_deadline = std::chrono::milliseconds{250},
                                                                   .open_deadline = std::chrono::milliseconds{250}},
                                                    .max_connect_rounds = 8,
                                                    .initial_backoff = std::chrono::seconds{1},
                                                    .max_backoff = std::chrono::seconds{1},
                                                }));
   BOOST_TEST(forge::asio::blocking::run(client.runtime(), remote->ping(40)) == 41);

   forge::asio::blocking::run(server.runtime(), server.shutdown());
   BOOST_CHECK_THROW(forge::asio::blocking::run(client.runtime(), remote->ping(40)), forge::exceptions::base);

   auto cancellation = boost::asio::cancellation_signal{};
   auto first =
       boost::asio::co_spawn(client.runtime().context(), remote->ping(40),
                             boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
   std::this_thread::sleep_for(std::chrono::milliseconds{50});
   BOOST_CHECK_THROW(forge::asio::blocking::run(client.runtime(), remote->ping(40)),
                     forge::api::core::exceptions::resource_exhausted);
   cancellation.emit(boost::asio::cancellation_type::all);
   BOOST_CHECK_THROW((void)first.get(), forge::api::core::exceptions::cancelled);

   forge::asio::blocking::run(client.runtime(), client.shutdown());
}
