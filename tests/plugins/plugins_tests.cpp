#include <boost/asio/awaitable.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/describe.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/system/system_error.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/api/core/macros.hpp>
#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "../quic_p2p/libp2p_identity_fixture.hxx"

import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.api.core.server_supplied;
import forge.api.core.trusted_invocation;
import forge.api.transport.exceptions;
import forge.api.transport.options;
import forge.api.transport.client;
import forge.api.transport.connection;
import forge.api.transport.server;
import forge.app.exceptions;
import forge.app.application;
import forge.app.diagnostics;
import forge.app.plugin_context;
import forge.app.plugin;
import forge.app.plugin_registry;
import forge.app.application_shell;
import forge.asio.blocking;
import forge.asio.compute;
import forge.asio.notification;
import forge.asio.runtime;
import forge.asio.task;
import forge.config.core.component;
import forge.config.core.document;
import forge.config.core.value;
import forge.config.core.decode;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.endpoint;
import forge.net.p2p.envelope;
import forge.net.p2p.identify;
import forge.net.p2p.diagnostics;
import forge.net.p2p.discovery;
import forge.net.p2p.dht;
import forge.net.p2p.lifecycle;
import forge.net.p2p.rendezvous;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.hole_punch;
import forge.net.p2p.protocol;
import forge.net.p2p.message;
import forge.net.p2p.scoring;
import forge.net.p2p.relay;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.p2p.negotiation;
import forge.net.p2p.peer_store;
import forge.net.p2p.node;
import forge.api.p2p.authenticated_peer;
import forge.api.p2p.binding;
import forge.api.p2p.publication;
import forge.plugins.crypto.secrets.types;
import forge.plugins.crypto.secrets.api;
import forge.plugins.p2p.diagnostics.types;
import forge.plugins.p2p.diagnostics.exceptions;
import forge.plugins.p2p.diagnostics.api;
import forge.plugins.p2p.diagnostics.plugin;
import forge.plugins.p2p.pubsub.types;
import forge.plugins.p2p.pubsub.exceptions;
import forge.plugins.p2p.pubsub.api;
import forge.plugins.p2p.pubsub.plugin;
import forge.plugins.p2p.resolver.types;
import forge.plugins.p2p.resolver.exceptions;
import forge.plugins.p2p.resolver.api;
import forge.plugins.p2p.resolver.plugin;
import forge.plugins.p2p.node.types;
import forge.plugins.p2p.node.exceptions;
import forge.plugins.p2p.node.api;
import forge.plugins.p2p.node.plugin;
import forge.schema.diagnostic;
import forge.schema.value_kind;
import forge.schema.object;
import forge.schema.enums;
import forge.variant.value;
import forge.variant.conversion;
import forge.variant.containers;
import forge.variant.described;

#include "details/config_assertions.hxx"

using forge::tests::plugins::require_field;

#include "details/resolver_publication_race.hxx"

struct pubsub_payload {
   std::string text;
   std::uint32_t value = 0;

   bool operator==(const pubsub_payload&) const = default;
};
BOOST_DESCRIBE_STRUCT(pubsub_payload, (), (text, value))

struct operation_request {
   std::string request_id;
   std::string subject;
   std::uint64_t revision = 0;

   bool operator==(const operation_request&) const = default;
};
BOOST_DESCRIBE_STRUCT(operation_request, (), (request_id, subject, revision))

struct operation_receipt {
   std::string request_id;
   bool accepted = false;
   std::uint64_t applied_revision = 0;
   std::string authority;
   std::string evidence;

   bool operator==(const operation_receipt&) const = default;
};
BOOST_DESCRIBE_STRUCT(operation_receipt, (), (request_id, accepted, applied_revision, authority, evidence))

struct peer_context_caller {
   forge::net::p2p::peer_id id;
};

struct peer_context_request {
   std::string value;
   peer_context_caller caller;
};

BOOST_DESCRIBE_STRUCT(peer_context_caller, (), (id))
BOOST_DESCRIBE_STRUCT(peer_context_request, (), (value, caller))

namespace forge::api::core {

template <> struct server_supplied<::peer_context_caller> {
   static constexpr bool required = true;

   static void reset(::peer_context_caller& value) {
      value = {};
   }

   [[nodiscard]] static bool apply(::peer_context_caller& value, const trusted_invocation& trusted) {
      const auto* peer = trusted.find<forge::api::p2p::authenticated_peer>();
      if (peer == nullptr) {
         return false;
      }
      value.id = peer->id;
      return true;
   }
};

} // namespace forge::api::core

namespace plugin_test_contract {

class node_test_api : public forge::api::core::contract<node_test_api, forge::api::core::surface::local |
                                                                           forge::api::core::surface::remote> {
 public:
   virtual ~node_test_api() = default;
   virtual boost::asio::awaitable<int> ping(int request) = 0;
};

class peer_context_test_api
    : public forge::api::core::contract<peer_context_test_api,
                                        forge::api::core::surface::local | forge::api::core::surface::remote> {
 public:
   virtual ~peer_context_test_api() = default;
   virtual boost::asio::awaitable<std::string> remote_peer(peer_context_request request) = 0;
};

class receipt_test_api : public forge::api::core::contract<receipt_test_api, forge::api::core::surface::local |
                                                                                 forge::api::core::surface::remote> {
 public:
   virtual ~receipt_test_api() = default;
   virtual boost::asio::awaitable<operation_receipt> apply(operation_request request) = 0;
};

class scripted_resolver_api
    : public forge::api::core::contract<scripted_resolver_api,
                                        forge::api::core::surface::local | forge::api::core::surface::remote> {
 public:
   virtual ~scripted_resolver_api() = default;
   virtual boost::asio::awaitable<forge::plugins::p2p::resolver::response>
   query(forge::plugins::p2p::resolver::query request) = 0;
};

} // namespace plugin_test_contract

FORGE_API(::plugin_test_contract::node_test_api, FORGE_API_CONTRACT("node.test", 1, 0), FORGE_API_METHOD(ping))
FORGE_API(::plugin_test_contract::peer_context_test_api, FORGE_API_CONTRACT("peer-context.test", 1, 0),
          FORGE_API_METHOD(remote_peer))
FORGE_API(::plugin_test_contract::receipt_test_api, FORGE_API_CONTRACT("receipt.test", 1, 0), FORGE_API_METHOD(apply))
FORGE_API(::plugin_test_contract::scripted_resolver_api,
          FORGE_API_CONTRACT("forge.plugins.p2p.resolver.protocol", 1, 0), FORGE_API_METHOD(query))

namespace {

using plugin_test_contract::node_test_api;
using plugin_test_contract::peer_context_test_api;
using plugin_test_contract::receipt_test_api;
using plugin_test_contract::scripted_resolver_api;

namespace crypto_secrets = forge::plugins::crypto::secrets;

struct plugin_log {
   std::vector<std::string> entries;
};

struct route_install_gate {
   std::atomic_bool armed = false;
   std::atomic_size_t copies = 0;
   std::mutex mutex;
   std::condition_variable changed;
   bool blocked = false;
   bool released = false;
};

class blocking_route_handler {
 public:
   explicit blocking_route_handler(std::shared_ptr<route_install_gate> gate) : gate_{std::move(gate)} {}

   blocking_route_handler(const blocking_route_handler& other) : gate_{other.gate_} {
      if (!gate_ || !gate_->armed.load(std::memory_order_acquire) ||
          gate_->copies.fetch_add(1, std::memory_order_acq_rel) + 1 != 2) {
         return;
      }
      auto lock = std::unique_lock{gate_->mutex};
      gate_->blocked = true;
      gate_->changed.notify_all();
      gate_->changed.wait(lock, [&] { return gate_->released; });
   }

   blocking_route_handler& operator=(const blocking_route_handler&) = default;

   boost::asio::awaitable<void> operator()(forge::net::p2p::node::incoming_protocol_stream) const {
      co_return;
   }

 private:
   std::shared_ptr<route_install_gate> gate_;
};

[[nodiscard]] forge::net::p2p::peer_id test_peer(std::uint8_t seed) {
   return forge::net::p2p::make_peer_id(
       {.type = forge::net::p2p::public_key::type::ed25519, .data = std::vector<std::uint8_t>(32, seed)});
}

[[nodiscard]] forge::config::core::document
test_p2p_config(std::optional<forge::net::p2p::peer_id> peer = std::nullopt) {
   auto document = forge::config::core::document{};
   document.set("plugins.p2p.node.allow-insecure-test-mode", true);
   document.set("plugins.p2p.node.identity.certificate-secret", "p2p/test-certificate");
   document.set("plugins.p2p.node.identity.private-key-secret", "p2p/test-private-key");
   if (peer) {
      document.set("plugins.p2p.node.peer-id", peer->to_string());
   }
   return document;
}

[[nodiscard]] forge::config::core::value::object_type dht_profile_config(std::string kind, std::string mode,
                                                                         std::string protocol, bool peers = true,
                                                                         bool providers = true, bool values = true) {
   auto profile = forge::config::core::value::object_type{};
   profile.emplace("kind", std::move(kind));
   profile.emplace("mode", std::move(mode));
   profile.emplace("protocol", std::move(protocol));
   profile.emplace("peers", peers);
   profile.emplace("providers", providers);
   profile.emplace("values", values);
   return profile;
}

[[nodiscard]] forge::config::core::value::object_type rendezvous_point_config(std::string endpoint,
                                                                              std::vector<std::string> namespaces) {
   auto point = forge::config::core::value::object_type{};
   point.emplace("endpoint", std::move(endpoint));
   auto encoded_namespaces = forge::config::core::value::array_type{};
   encoded_namespaces.reserve(namespaces.size());
   for (auto& namespace_name : namespaces) {
      encoded_namespaces.emplace_back(std::move(namespace_name));
   }
   point.emplace("namespaces", std::move(encoded_namespaces));
   return point;
}

class p2p_test_dependency_plugin : public forge::app::plugin {
 public:
   explicit p2p_test_dependency_plugin(std::string id) : id_{std::move(id)} {}

   [[nodiscard]] forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = id_};
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

class p2p_test_secrets_api final : public crypto_secrets::api {
 public:
   boost::asio::awaitable<crypto_secrets::snapshot> status(crypto_secrets::query) override {
      co_return crypto_secrets::snapshot{.configured_secrets = 2};
   }

   boost::asio::awaitable<crypto_secrets::get_result> get_bytes(crypto_secrets::get_request request) override {
      const auto& identity = fixture();
      const auto* material = request.secret_id == "p2p/test-certificate"   ? &identity.certificate_pem
                             : request.secret_id == "p2p/test-private-key" ? &identity.private_key_pem
                                                                           : nullptr;
      if (material == nullptr) {
         throw std::runtime_error{"unknown P2P test secret"};
      }
      auto bytes = decltype(crypto_secrets::get_result{}.bytes){};
      bytes.reserve(material->size());
      for (const auto value : *material) {
         bytes.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(value)));
      }
      co_return crypto_secrets::get_result{.secret_id = std::move(request.secret_id), .bytes = std::move(bytes)};
   }

   boost::asio::awaitable<crypto_secrets::derive_result> derive_hkdf_sha256(crypto_secrets::derive_request) override {
      throw std::logic_error{"P2P test secrets do not implement derivation"};
      co_return crypto_secrets::derive_result{};
   }

   boost::asio::awaitable<crypto_secrets::aead_encrypt_result>
   encrypt_aes_gcm(crypto_secrets::aead_encrypt_request) override {
      throw std::logic_error{"P2P test secrets do not implement encryption"};
      co_return crypto_secrets::aead_encrypt_result{};
   }

   boost::asio::awaitable<crypto_secrets::aead_decrypt_result>
   decrypt_aes_gcm(crypto_secrets::aead_decrypt_request) override {
      throw std::logic_error{"P2P test secrets do not implement decryption"};
      co_return crypto_secrets::aead_decrypt_result{};
   }

 private:
   [[nodiscard]] static const forge::tests::p2p::identity_fixture& fixture() {
      static const auto value = forge::tests::p2p::make_identity_fixture("p2p-plugin-suite");
      return value;
   }
};

class p2p_test_secrets_plugin final : public p2p_test_dependency_plugin {
 public:
   p2p_test_secrets_plugin() : p2p_test_dependency_plugin{"forge.plugins.crypto.secrets"} {}

   boost::asio::awaitable<void> provide(forge::api::core::provider& provider) override {
      provider.install<crypto_secrets::api>(std::make_shared<p2p_test_secrets_api>());
      co_return;
   }
};

void register_p2p_stack(forge::app::plugin_registry& registry) {
   registry.register_plugin(forge::app::plugin_descriptor{
       .id = forge::app::plugin_id{.value = "forge.plugins.db.store"},
       .factory = [] { return std::make_unique<p2p_test_dependency_plugin>("forge.plugins.db.store"); },
   });
   registry.register_plugin(forge::app::plugin_descriptor{
       .id = forge::app::plugin_id{.value = "forge.plugins.crypto.secrets"},
       .factory = [] { return std::make_unique<p2p_test_secrets_plugin>(); },
   });
   registry.register_plugin(forge::plugins::p2p::node::descriptor());
}

class node_test_api_impl final : public node_test_api {
 public:
   boost::asio::awaitable<int> ping(int request) override {
      co_return request + 1;
   }
};

class offset_node_test_api_impl final : public node_test_api {
 public:
   explicit offset_node_test_api_impl(int offset) : offset_{offset} {}

   boost::asio::awaitable<int> ping(int request) override {
      co_return request + offset_;
   }

 private:
   int offset_ = 0;
};

[[nodiscard]] forge::api::core::binding_plan node_test_plan(const forge::api::core::registry& apis) {
   return forge::api::core::binding()
       .serve(apis)
       .export_api<node_test_api>({.id = {"node.test"}, .major = 1, .min_revision = 0})
       .build();
}

[[nodiscard]] forge::api::core::binding_plan peer_context_test_plan(const forge::api::core::registry& apis) {
   return forge::api::core::binding()
       .serve(apis)
       .export_api<peer_context_test_api>({.id = {"peer-context.test"}, .major = 1, .min_revision = 0})
       .build();
}

struct nonresponding_node_test_state {
   std::atomic_bool started{false};
   std::atomic_bool cancelled{false};
};

class nonresponding_node_test_api_impl final : public node_test_api {
 public:
   explicit nonresponding_node_test_api_impl(std::shared_ptr<nonresponding_node_test_state> state)
       : state_{std::move(state)} {}

   boost::asio::awaitable<int> ping(int request) override {
      auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor, std::chrono::seconds{2}};
      state_->started.store(true, std::memory_order_release);
      try {
         co_await timer.async_wait(boost::asio::use_awaitable);
      } catch (const boost::system::system_error& error) {
         if (error.code() == boost::asio::error::operation_aborted) {
            state_->cancelled.store(true, std::memory_order_release);
         }
         throw;
      }
      co_return request + 1;
   }

 private:
   std::shared_ptr<nonresponding_node_test_state> state_;
};

class peer_context_test_api_impl final : public peer_context_test_api {
 public:
   boost::asio::awaitable<std::string> remote_peer(peer_context_request request) override {
      co_return request.caller.id.to_string() + ":" + request.value;
   }
};

struct receipt_test_state {
   mutable std::mutex mutex;
   std::unordered_map<std::string, operation_receipt> receipts;
   std::size_t applied = 0;
};

class receipt_test_api_impl final : public receipt_test_api {
 public:
   explicit receipt_test_api_impl(std::shared_ptr<receipt_test_state> state) : state_{std::move(state)} {}

   boost::asio::awaitable<operation_receipt> apply(operation_request request) override {
      auto lock = std::scoped_lock{state_->mutex};
      if (const auto existing = state_->receipts.find(request.request_id); existing != state_->receipts.end()) {
         co_return existing->second;
      }

      const auto revision = ++state_->applied;
      auto receipt = operation_receipt{
          .request_id = request.request_id,
          .accepted = true,
          .applied_revision = revision,
          .authority = "receipt-test",
          .evidence = request.subject + ":" + std::to_string(request.revision) + ":" + std::to_string(revision),
      };
      auto [inserted, _] = state_->receipts.emplace(request.request_id, std::move(receipt));
      co_return inserted->second;
   }

 private:
   std::shared_ptr<receipt_test_state> state_;
};

struct received_pubsub_messages {
   mutable std::mutex mutex;
   std::vector<forge::plugins::p2p::pubsub::message> raw;
   std::vector<forge::plugins::p2p::pubsub::typed_message<pubsub_payload>> typed;
   std::size_t accepted = 0;
   std::size_t rejected = 0;
   std::size_t ignored = 0;

   void push(forge::plugins::p2p::pubsub::message value, forge::net::p2p::pubsub::validation_result result) {
      auto lock = std::scoped_lock{mutex};
      raw.push_back(std::move(value));
      switch (result) {
      case forge::net::p2p::pubsub::validation_result::accept:
         ++accepted;
         break;
      case forge::net::p2p::pubsub::validation_result::reject:
         ++rejected;
         break;
      case forge::net::p2p::pubsub::validation_result::ignore:
         ++ignored;
         break;
      case forge::net::p2p::pubsub::validation_result::retry:
         break;
      }
   }

   void push(forge::plugins::p2p::pubsub::typed_message<pubsub_payload> value) {
      auto lock = std::scoped_lock{mutex};
      typed.push_back(std::move(value));
   }

   [[nodiscard]] std::size_t raw_size() const {
      auto lock = std::scoped_lock{mutex};
      return raw.size();
   }

   [[nodiscard]] std::size_t typed_size() const {
      auto lock = std::scoped_lock{mutex};
      return typed.size();
   }
};

bool wait_for_count(const received_pubsub_messages& messages, std::size_t raw, std::size_t typed = 0,
                    std::chrono::milliseconds timeout = std::chrono::seconds{5}) {
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   while (std::chrono::steady_clock::now() < deadline) {
      if (messages.raw_size() >= raw && messages.typed_size() >= typed) {
         return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{25});
   }
   return messages.raw_size() >= raw && messages.typed_size() >= typed;
}

template <typename Predicate>
bool wait_for_pubsub_snapshot(const forge::plugins::p2p::pubsub::api& pubsub, Predicate predicate,
                              std::chrono::milliseconds timeout) {
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   while (std::chrono::steady_clock::now() < deadline) {
      if (predicate(pubsub.snapshot())) {
         return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{25});
   }
   return predicate(pubsub.snapshot());
}

bool wait_for_pubsub_peer(const forge::plugins::p2p::pubsub::api& pubsub, std::chrono::milliseconds timeout) {
   return wait_for_pubsub_snapshot(
       pubsub, [](const forge::plugins::p2p::pubsub::snapshot& snapshot) { return snapshot.core.peers > 0; }, timeout);
}

template <typename Predicate>
boost::asio::awaitable<bool> async_wait_for_condition(Predicate predicate, std::chrono::milliseconds timeout) {
   auto executor = co_await boost::asio::this_coro::executor;
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   while (std::chrono::steady_clock::now() < deadline) {
      if (predicate()) {
         co_return true;
      }
      auto timer = boost::asio::steady_timer{executor, std::chrono::milliseconds{10}};
      co_await timer.async_wait(boost::asio::use_awaitable);
   }
   co_return predicate();
}

struct fake_pubsub_source_state {
   mutable std::mutex mutex;
   bool release_join = false;
   bool fail_join = false;
   std::size_t enable_calls = 0;
   std::size_t join_attempts = 0;
   std::size_t leave_attempts = 0;
   std::size_t joined_handlers = 0;

   [[nodiscard]] std::size_t joins() const {
      auto lock = std::scoped_lock{mutex};
      return join_attempts;
   }

   [[nodiscard]] bool released() const {
      auto lock = std::scoped_lock{mutex};
      return release_join;
   }

   void release(bool fail) {
      auto lock = std::scoped_lock{mutex};
      fail_join = fail;
      release_join = true;
   }
};

struct subscribe_task_result {
   mutable std::mutex mutex;
   bool done = false;
   std::exception_ptr error;
   std::optional<forge::plugins::p2p::pubsub::subscription> value;

   void complete(forge::plugins::p2p::pubsub::subscription subscription) {
      auto lock = std::scoped_lock{mutex};
      value = std::move(subscription);
      done = true;
   }

   void fail(std::exception_ptr exception) {
      auto lock = std::scoped_lock{mutex};
      error = std::move(exception);
      done = true;
   }

   [[nodiscard]] bool finished() const {
      auto lock = std::scoped_lock{mutex};
      return done;
   }

   [[nodiscard]] bool failed() const {
      auto lock = std::scoped_lock{mutex};
      return error != nullptr;
   }
};

class fake_pubsub_source final : public forge::plugins::p2p::node::pubsub_source {
 public:
   explicit fake_pubsub_source(std::shared_ptr<fake_pubsub_source_state> state) : state_{std::move(state)} {}

   void enable(forge::net::p2p::pubsub::options) override {
      auto lock = std::scoped_lock{state_->mutex};
      ++state_->enable_calls;
   }

   [[nodiscard]] forge::net::p2p::peer_id local_peer() const override {
      return forge::net::p2p::peer_id{.value = "fake-pubsub-peer"};
   }

   boost::asio::awaitable<forge::net::p2p::pubsub::message>
   async_publish_message(forge::net::p2p::pubsub::topic subject, std::vector<std::uint8_t> data,
                         forge::net::p2p::pubsub::publish_options) override {
      co_return forge::net::p2p::pubsub::message{
          .from = local_peer(),
          .data = std::move(data),
          .seqno = {1},
          .subject = std::move(subject),
      };
   }

   boost::asio::awaitable<forge::net::p2p::pubsub::subscription>
   async_join_topic(forge::net::p2p::pubsub::topic subject, forge::net::p2p::pubsub::handler) override {
      auto executor = co_await boost::asio::this_coro::executor;
      {
         auto lock = std::scoped_lock{state_->mutex};
         ++state_->join_attempts;
      }
      while (!state_->released()) {
         auto timer = boost::asio::steady_timer{executor, std::chrono::milliseconds{10}};
         co_await timer.async_wait(boost::asio::use_awaitable);
      }
      {
         auto lock = std::scoped_lock{state_->mutex};
         if (state_->fail_join) {
            FORGE_THROW_EXCEPTION(forge::plugins::p2p::pubsub::exceptions::handler_limit,
                                  "fake PubSub source join failed");
         }
         ++state_->joined_handlers;
      }
      co_return forge::net::p2p::pubsub::subscription{.subscribe = true, .subject = std::move(subject)};
   }

   boost::asio::awaitable<void> async_leave_topic(forge::net::p2p::pubsub::topic) override {
      auto lock = std::scoped_lock{state_->mutex};
      ++state_->leave_attempts;
      co_return;
   }

   forge::net::p2p::pubsub::snapshot snapshot() const override {
      return forge::net::p2p::pubsub::snapshot{};
   }

 private:
   std::shared_ptr<fake_pubsub_source_state> state_;
};

class fake_p2p_node_plugin final : public forge::app::plugin {
 public:
   explicit fake_p2p_node_plugin(std::shared_ptr<fake_pubsub_source_state> state) : state_{std::move(state)} {}

   [[nodiscard]] forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "forge.plugins.p2p.node"};
   }

   [[nodiscard]] std::string version() const override {
      return "test";
   }

   boost::asio::awaitable<void> provide(forge::api::core::provider& provider) override {
      provider.install<forge::plugins::p2p::node::pubsub_source>(std::make_shared<fake_pubsub_source>(state_));
      co_return;
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
   std::shared_ptr<fake_pubsub_source_state> state_;
};

struct scripted_resolver_state {
   std::vector<forge::plugins::p2p::resolver::response> responses;
   std::size_t calls = 0;
};

class scripted_resolver_api_impl final : public scripted_resolver_api {
 public:
   explicit scripted_resolver_api_impl(std::shared_ptr<scripted_resolver_state> state) : state_{std::move(state)} {}

   boost::asio::awaitable<forge::plugins::p2p::resolver::response>
   query(forge::plugins::p2p::resolver::query) override {
      const auto index = std::min(state_->calls, state_->responses.size() - 1);
      ++state_->calls;
      co_return state_->responses[index];
   }

 private:
   std::shared_ptr<scripted_resolver_state> state_;
};

[[nodiscard]] forge::plugins::p2p::resolver::entry resolver_test_entry(std::string protocol) {
   return forge::plugins::p2p::resolver::entry{
       .id = {.value = "node.test"},
       .version = {.major = 1, .revision = 0},
       .protocol = std::move(protocol),
       .codec = {.value = "forge.raw"},
       .max_inflight = 64,
       .max_frame_size = 16 * 1024 * 1024,
       .methods = {forge::plugins::p2p::resolver::method{
           .name = "ping",
           .kind = forge::api::core::method_kind::unary,
       }},
   };
}

class route_publisher_plugin final : public forge::app::plugin {
 public:
   explicit route_publisher_plugin(plugin_log& log) : log_{&log} {}

   [[nodiscard]] forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "route-publisher"};
   }

   [[nodiscard]] std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      auto p2p = context.apis().get<forge::plugins::p2p::node::api>(
          {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});

      auto plan = forge::api::core::binding()
                      .serve(context.apis())
                      .export_api<node_test_api>({.id = {"node.test"}, .major = 1, .min_revision = 0})
                      .export_api<peer_context_test_api>({.id = {"peer-context.test"}, .major = 1, .min_revision = 0})
                      .build();
      publication_ = p2p->publish_api(std::move(plan), forge::net::p2p::protocol_id{.value = "/forge/api/node-test/1"});
      p2p->publish_protocol(
          forge::net::p2p::protocol_id{.value = "/forge/test/blob-transfer/1"},
          [](forge::net::p2p::node::incoming_protocol_stream) -> boost::asio::awaitable<void> { co_return; });
      log_->entries.push_back("routes.published");
      co_return;
   }

   boost::asio::awaitable<void> startup() override {
      log_->entries.push_back("routes.startup");
      co_return;
   }

   boost::asio::awaitable<void> shutdown() override {
      log_->entries.push_back("routes.shutdown");
      co_await publication_.async_close();
      co_return;
   }

 private:
   plugin_log* log_ = nullptr;
   forge::api::p2p::publication publication_;
};

class duplicate_route_plugin final : public forge::app::plugin {
 public:
   [[nodiscard]] forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "duplicate-route"};
   }

   [[nodiscard]] std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      auto p2p = context.apis().get<forge::plugins::p2p::node::api>(
          {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
      auto handler = [](forge::net::p2p::node::incoming_protocol_stream) -> boost::asio::awaitable<void> { co_return; };
      p2p->publish_protocol(forge::net::p2p::protocol_id{.value = "/forge/test/duplicate/1"}, handler);
      p2p->publish_protocol(forge::net::p2p::protocol_id{.value = "/forge/test/duplicate/1"}, handler);
      co_return;
   }

   boost::asio::awaitable<void> startup() override {
      co_return;
   }

   boost::asio::awaitable<void> shutdown() override {
      co_return;
   }
};

class resolver_route_publisher_plugin final : public forge::app::plugin {
 public:
   [[nodiscard]] forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "resolver-route-publisher"};
   }

   [[nodiscard]] std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      auto resolver = context.apis().get<forge::plugins::p2p::resolver::api>(
          {.id = {"forge.plugins.p2p.resolver"}, .major = 2, .min_revision = 0});

      auto plan = forge::api::core::binding()
                      .serve(context.apis())
                      .export_api<node_test_api>({.id = {"node.test"}, .major = 1, .min_revision = 0})
                      .build();
      publication_ =
          resolver->publish_api(std::move(plan), forge::net::p2p::protocol_id{.value = "/forge/api/node-test/1"});
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
   forge::api::p2p::publication publication_;
};

class duplicate_resolver_route_plugin final : public forge::app::plugin {
 public:
   [[nodiscard]] forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "duplicate-resolver-route"};
   }

   [[nodiscard]] std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      auto resolver = context.apis().get<forge::plugins::p2p::resolver::api>(
          {.id = {"forge.plugins.p2p.resolver"}, .major = 2, .min_revision = 0});
      auto plan = forge::api::core::binding()
                      .serve(context.apis())
                      .export_api<node_test_api>({.id = {"node.test"}, .major = 1, .min_revision = 0})
                      .build();
      publication_ = resolver->publish_api(plan, forge::net::p2p::protocol_id{.value = "/forge/api/node-test/1"});
      static_cast<void>(resolver->publish_api(
          std::move(plan), forge::net::p2p::protocol_id{.value = "/forge/api/node-test-duplicate/1"}));
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
   forge::api::p2p::publication publication_;
};

class resolver_custom_transport_route_plugin final : public forge::app::plugin {
 public:
   [[nodiscard]] forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "resolver-custom-transport-route"};
   }

   [[nodiscard]] std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      auto resolver = context.apis().get<forge::plugins::p2p::resolver::api>(
          {.id = {"forge.plugins.p2p.resolver"}, .major = 2, .min_revision = 0});
      auto plan = forge::api::core::binding()
                      .serve(context.apis())
                      .export_api<node_test_api>({.id = {"node.test"}, .major = 1, .min_revision = 0})
                      .build();
      publication_ =
          resolver->publish_api(std::move(plan), forge::net::p2p::protocol_id{.value = "/forge/api/node-test-custom/1"},
                                forge::plugins::p2p::resolver::publish_options{
                                    .transport =
                                        forge::api::transport::options{
                                            .codec = {.value = "forge.test.raw"},
                                            .max_inflight = 7,
                                            .max_frame_size = 512 * 1024,
                                        },
                                });
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
   forge::api::p2p::publication publication_;
};

class receipt_route_publisher_plugin final : public forge::app::plugin {
 public:
   [[nodiscard]] forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "receipt-route-publisher"};
   }

   [[nodiscard]] std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      auto resolver = context.apis().get<forge::plugins::p2p::resolver::api>(
          {.id = {"forge.plugins.p2p.resolver"}, .major = 2, .min_revision = 0});

      auto plan = forge::api::core::binding()
                      .serve(context.apis())
                      .export_api<receipt_test_api>({.id = {"receipt.test"}, .major = 1, .min_revision = 0})
                      .build();
      publication_ =
          resolver->publish_api(std::move(plan), forge::net::p2p::protocol_id{.value = "/forge/api/receipt-test/1"});
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
   forge::api::p2p::publication publication_;
};

class resolver_protocol_conflict_plugin final : public forge::app::plugin {
 public:
   [[nodiscard]] forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "resolver-protocol-conflict"};
   }

   [[nodiscard]] std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      auto p2p = context.apis().get<forge::plugins::p2p::node::api>(
          {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
      p2p->publish_protocol(
          forge::net::p2p::protocol_id{.value = "/forge/api/resolver/2"},
          [](forge::net::p2p::node::incoming_protocol_stream) -> boost::asio::awaitable<void> { co_return; });
      co_return;
   }

   boost::asio::awaitable<void> startup() override {
      co_return;
   }

   boost::asio::awaitable<void> shutdown() override {
      co_return;
   }
};

class scripted_resolver_plugin final : public forge::app::plugin {
 public:
   [[nodiscard]] forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "scripted-resolver"};
   }

   [[nodiscard]] std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      auto p2p = context.apis().get<forge::plugins::p2p::node::api>(
          {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
      auto plan = forge::api::core::binding()
                      .serve(context.apis())
                      .export_api<scripted_resolver_api>(
                          {.id = {"forge.plugins.p2p.resolver.protocol"}, .major = 1, .min_revision = 0})
                      .build();
      publication_ = p2p->publish_api(std::move(plan), forge::net::p2p::protocol_id{.value = "/forge/api/resolver/2"});
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
   forge::api::p2p::publication publication_;
};

class p2p_plugin_application final : public forge::app::application_shell {
 public:
   explicit p2p_plugin_application(plugin_log& log) : log_{&log} {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      register_p2p_stack(registry);
      registry.register_plugin(forge::app::plugin_descriptor{
          .id = forge::app::plugin_id{.value = "route-publisher"},
          .dependencies = {forge::app::plugin_id{.value = "forge.plugins.p2p.node"}},
          .factory = [this] { return std::make_unique<route_publisher_plugin>(*log_); },
      });
   }

   boost::asio::awaitable<void> on_provide(forge::app::application_context& context) override {
      context.apis().install<node_test_api>(node_test_api::describe(), std::make_shared<node_test_api_impl>());
      context.apis().install<peer_context_test_api>(peer_context_test_api::describe(),
                                                    std::make_shared<peer_context_test_api_impl>());
      co_return;
   }

 private:
   plugin_log* log_ = nullptr;
};

class duplicate_p2p_plugin_application final : public forge::app::application_shell {
 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      register_p2p_stack(registry);
      registry.register_plugin(forge::app::plugin_descriptor{
          .id = forge::app::plugin_id{.value = "duplicate-route"},
          .dependencies = {forge::app::plugin_id{.value = "forge.plugins.p2p.node"}},
          .factory = [] { return std::make_unique<duplicate_route_plugin>(); },
      });
   }
};

class p2p_only_application final : public forge::app::application_shell {
 public:
   using forge::app::application_shell::application_shell;

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      register_p2p_stack(registry);
   }
};

class diagnostics_application final : public forge::app::application_shell {
 public:
   using forge::app::application_shell::application_shell;

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      register_p2p_stack(registry);
      registry.register_plugin(forge::plugins::p2p::diagnostics::descriptor());
   }
};

class pubsub_application final : public forge::app::application_shell {
 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      register_p2p_stack(registry);
      registry.register_plugin(forge::plugins::p2p::pubsub::descriptor());
   }
};

class fake_pubsub_application final : public forge::app::application_shell {
 public:
   explicit fake_pubsub_application(std::shared_ptr<fake_pubsub_source_state> state) : state_{std::move(state)} {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(forge::app::plugin_descriptor{
          .id = forge::app::plugin_id{.value = "forge.plugins.p2p.node"},
          .factory = [state = state_] { return std::make_unique<fake_p2p_node_plugin>(state); },
      });
      registry.register_plugin(forge::plugins::p2p::pubsub::descriptor());
   }

 private:
   std::shared_ptr<fake_pubsub_source_state> state_;
};

class resolver_plugin_application final : public forge::app::application_shell {
 public:
   explicit resolver_plugin_application(
       std::shared_ptr<node_test_api> node_api = std::make_shared<node_test_api_impl>())
       : node_api_{std::move(node_api)} {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      register_p2p_stack(registry);
      registry.register_plugin(forge::plugins::p2p::resolver::descriptor());
      registry.register_plugin(forge::app::plugin_descriptor{
          .id = forge::app::plugin_id{.value = "resolver-route-publisher"},
          .dependencies = {forge::app::plugin_id{.value = "forge.plugins.p2p.resolver"}},
          .factory = [] { return std::make_unique<resolver_route_publisher_plugin>(); },
      });
   }

   boost::asio::awaitable<void> on_provide(forge::app::application_context& context) override {
      context.apis().install<node_test_api>(node_test_api::describe(), node_api_);
      context.apis().install<peer_context_test_api>(peer_context_test_api::describe(),
                                                    std::make_shared<peer_context_test_api_impl>());
      co_return;
   }

 private:
   std::shared_ptr<node_test_api> node_api_;
};

class resolver_only_application final : public forge::app::application_shell {
 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      register_p2p_stack(registry);
      registry.register_plugin(forge::plugins::p2p::resolver::descriptor());
   }
};

class resolver_publication_application final : public forge::app::application_shell {
 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      register_p2p_stack(registry);
      registry.register_plugin(forge::plugins::p2p::resolver::descriptor());
   }

   boost::asio::awaitable<void> on_provide(forge::app::application_context& context) override {
      context.apis().install<node_test_api>(node_test_api::describe(), std::make_shared<node_test_api_impl>());
      context.apis().install<peer_context_test_api>(peer_context_test_api::describe(),
                                                    std::make_shared<peer_context_test_api_impl>());
      co_return;
   }
};

class duplicate_resolver_plugin_application final : public forge::app::application_shell {
 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      register_p2p_stack(registry);
      registry.register_plugin(forge::plugins::p2p::resolver::descriptor());
      registry.register_plugin(forge::app::plugin_descriptor{
          .id = forge::app::plugin_id{.value = "duplicate-resolver-route"},
          .dependencies = {forge::app::plugin_id{.value = "forge.plugins.p2p.resolver"}},
          .factory = [] { return std::make_unique<duplicate_resolver_route_plugin>(); },
      });
   }

   boost::asio::awaitable<void> on_provide(forge::app::application_context& context) override {
      context.apis().install<node_test_api>(node_test_api::describe(), std::make_shared<node_test_api_impl>());
      context.apis().install<peer_context_test_api>(peer_context_test_api::describe(),
                                                    std::make_shared<peer_context_test_api_impl>());
      co_return;
   }
};

class resolver_custom_transport_application final : public forge::app::application_shell {
 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      register_p2p_stack(registry);
      registry.register_plugin(forge::plugins::p2p::resolver::descriptor());
      registry.register_plugin(forge::app::plugin_descriptor{
          .id = forge::app::plugin_id{.value = "resolver-custom-transport-route"},
          .dependencies = {forge::app::plugin_id{.value = "forge.plugins.p2p.resolver"}},
          .factory = [] { return std::make_unique<resolver_custom_transport_route_plugin>(); },
      });
   }

   boost::asio::awaitable<void> on_provide(forge::app::application_context& context) override {
      context.apis().install<node_test_api>(node_test_api::describe(), std::make_shared<node_test_api_impl>());
      context.apis().install<peer_context_test_api>(peer_context_test_api::describe(),
                                                    std::make_shared<peer_context_test_api_impl>());
      co_return;
   }
};

class receipt_resolver_application final : public forge::app::application_shell {
 public:
   explicit receipt_resolver_application(std::shared_ptr<receipt_test_state> state) : state_{std::move(state)} {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      register_p2p_stack(registry);
      registry.register_plugin(forge::plugins::p2p::resolver::descriptor());
      registry.register_plugin(forge::app::plugin_descriptor{
          .id = forge::app::plugin_id{.value = "receipt-route-publisher"},
          .dependencies = {forge::app::plugin_id{.value = "forge.plugins.p2p.resolver"}},
          .factory = [] { return std::make_unique<receipt_route_publisher_plugin>(); },
      });
   }

   boost::asio::awaitable<void> on_provide(forge::app::application_context& context) override {
      context.apis().install<receipt_test_api>(receipt_test_api::describe(),
                                               std::make_shared<receipt_test_api_impl>(state_));
      co_return;
   }

 private:
   std::shared_ptr<receipt_test_state> state_;
};

class resolver_protocol_conflict_application final : public forge::app::application_shell {
 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      register_p2p_stack(registry);
      registry.register_plugin(forge::app::plugin_descriptor{
          .id = forge::app::plugin_id{.value = "resolver-protocol-conflict"},
          .dependencies = {forge::app::plugin_id{.value = "forge.plugins.p2p.node"}},
          .factory = [] { return std::make_unique<resolver_protocol_conflict_plugin>(); },
      });
      registry.register_plugin(forge::plugins::p2p::resolver::descriptor());
   }
};

class scripted_resolver_application final : public forge::app::application_shell {
 public:
   explicit scripted_resolver_application(std::shared_ptr<scripted_resolver_state> state) : state_{std::move(state)} {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      register_p2p_stack(registry);
      registry.register_plugin(forge::app::plugin_descriptor{
          .id = forge::app::plugin_id{.value = "scripted-resolver"},
          .dependencies = {forge::app::plugin_id{.value = "forge.plugins.p2p.node"}},
          .factory = [] { return std::make_unique<scripted_resolver_plugin>(); },
      });
   }

   boost::asio::awaitable<void> on_provide(forge::app::application_context& context) override {
      context.apis().install<scripted_resolver_api>(scripted_resolver_api::describe(),
                                                    std::make_shared<scripted_resolver_api_impl>(state_));
      co_return;
   }

 private:
   std::shared_ptr<scripted_resolver_state> state_;
};

[[nodiscard]] bool has_field(const forge::config::core::component_descriptor& descriptor, std::string_view name) {
   return std::ranges::any_of(descriptor.fields, [&](const auto& field) { return field.name == name; });
}

template <typename T>
concept has_metrics = requires(T& value) { value.metrics(); };

template <typename T>
concept has_peers = requires(T& value) { value.peers(); };

template <typename T>
concept has_pubsub_publish = requires(T& value) {
   value.publish(forge::net::p2p::pubsub::topic{.value = "topic"}, std::vector<std::uint8_t>{},
                 forge::plugins::p2p::pubsub::publish_options{});
};

template <typename T>
concept has_pubsub_subscribe = requires(T& value, forge::plugins::p2p::pubsub::handler handler) {
   value.subscribe(forge::net::p2p::pubsub::topic{.value = "topic"}, std::move(handler),
                   forge::plugins::p2p::pubsub::subscribe_options{});
};

static_assert(!has_metrics<forge::plugins::p2p::node::api>);
static_assert(!has_peers<forge::plugins::p2p::node::api>);
static_assert(!has_pubsub_publish<forge::plugins::p2p::node::api>);
static_assert(!has_pubsub_subscribe<forge::plugins::p2p::node::api>);
} // namespace

BOOST_AUTO_TEST_CASE(p2p_node_plugin_config_is_described_from_public_schema) {
   auto plugin = forge::plugins::p2p::node::plugin{};
   const auto descriptor = plugin.describe_config();
   BOOST_REQUIRE(descriptor.has_value());
   BOOST_TEST(descriptor->section == "plugins.p2p.node");

   const auto& listen = require_field(*descriptor, "listen");
   BOOST_TEST(static_cast<int>(listen.kind) == static_cast<int>(forge::schema::value_kind::string_list));
   BOOST_TEST(listen.has_default);

   const auto& bootstrap = require_field(*descriptor, "bootstrap");
   BOOST_TEST(static_cast<int>(bootstrap.kind) == static_cast<int>(forge::schema::value_kind::string_list));
   BOOST_TEST(bootstrap.has_default);

   const auto& bootstrap_requirement = require_field(*descriptor, "bootstrap-requirement");
   BOOST_TEST(bootstrap_requirement.has_default);
   BOOST_TEST(std::get<std::string>(bootstrap_requirement.default_value.storage) == "allow-disconnected");

   const auto& bootstrap_startup_budget = require_field(*descriptor, "bootstrap-startup-budget-ms");
   BOOST_TEST(bootstrap_startup_budget.has_default);
   BOOST_TEST(std::get<std::uint64_t>(bootstrap_startup_budget.default_value.storage) == 10'000U);

   const auto& bootstrap_connect_timeout = require_field(*descriptor, "bootstrap-connect-timeout-ms");
   BOOST_TEST(bootstrap_connect_timeout.has_default);
   BOOST_TEST(std::get<std::uint64_t>(bootstrap_connect_timeout.default_value.storage) == 2'000U);

   const auto& bootstrap_parallelism = require_field(*descriptor, "bootstrap-max-parallel");
   BOOST_TEST(bootstrap_parallelism.has_default);
   BOOST_TEST(std::get<std::uint64_t>(bootstrap_parallelism.default_value.storage) == 4U);

   const auto& peer_store = require_field(*descriptor, "peer-store.store");
   BOOST_TEST(peer_store.has_default);
   BOOST_TEST(std::get<std::string>(peer_store.default_value.storage).empty());

   const auto& schema_policy = require_field(*descriptor, "peer-store.schema-policy");
   BOOST_TEST(schema_policy.has_default);
   BOOST_TEST(std::get<std::string>(schema_policy.default_value.storage) == "reset");

   const auto& dht_profiles = require_field(*descriptor, "dht.profiles");
   BOOST_TEST(static_cast<int>(dht_profiles.kind) == static_cast<int>(forge::schema::value_kind::object_list));
   BOOST_TEST(dht_profiles.has_default);

   const auto& topology_mode = require_field(*descriptor, "topology.mode");
   BOOST_TEST(topology_mode.has_default);
   BOOST_TEST(std::get<std::string>(topology_mode.default_value.storage) == "managed");
   BOOST_TEST(std::get<std::uint64_t>(require_field(*descriptor, "topology.peers.low").default_value.storage) == 128U);
   BOOST_TEST(std::get<std::uint64_t>(require_field(*descriptor, "topology.peers.target").default_value.storage) ==
              160U);
   BOOST_TEST(std::get<std::uint64_t>(require_field(*descriptor, "topology.peers.high").default_value.storage) == 192U);
   BOOST_TEST(
       std::get<std::uint64_t>(require_field(*descriptor, "topology.max-parallel-dials").default_value.storage) == 4U);

   const auto& rendezvous_role = require_field(*descriptor, "rendezvous.role");
   BOOST_TEST(rendezvous_role.has_default);
   BOOST_TEST(std::get<std::string>(rendezvous_role.default_value.storage) == "disabled");
   BOOST_TEST(static_cast<int>(require_field(*descriptor, "rendezvous.points").kind) ==
              static_cast<int>(forge::schema::value_kind::object_list));
   BOOST_TEST(std::get<bool>(require_field(*descriptor, "peer-exchange.enabled").default_value.storage));
   BOOST_TEST(std::get<std::uint64_t>(require_field(*descriptor, "peer-exchange.max-peers").default_value.storage) ==
              4U);

   const auto& certificate_secret = require_field(*descriptor, "identity.certificate-secret");
   BOOST_TEST(certificate_secret.has_default);
   BOOST_TEST(std::get<std::string>(certificate_secret.default_value.storage).empty());

   const auto& private_key_secret = require_field(*descriptor, "identity.private-key-secret");
   BOOST_TEST(private_key_secret.has_default);
   BOOST_TEST(std::get<std::string>(private_key_secret.default_value.storage).empty());
   BOOST_TEST(!has_field(*descriptor, "certificate-pem"));
   BOOST_TEST(!has_field(*descriptor, "private-key-pem"));

   const auto& max_inflight = require_field(*descriptor, "max-inflight-per-peer");
   BOOST_TEST(max_inflight.has_default);
   BOOST_TEST(std::get<std::uint64_t>(max_inflight.default_value.storage) == 64U);

   const auto& api_deadline = require_field(*descriptor, "api.deadline-ms");
   BOOST_TEST(api_deadline.has_default);
   BOOST_TEST(std::get<std::uint64_t>(api_deadline.default_value.storage) == 0U);

   const auto& api_frame_size = require_field(*descriptor, "api.max-frame-size");
   BOOST_TEST(api_frame_size.has_default);
   BOOST_TEST(std::get<std::uint64_t>(api_frame_size.default_value.storage) == 16U * 1024U * 1024U);

   const auto& insecure = require_field(*descriptor, "allow-insecure-test-mode");
   BOOST_TEST(insecure.has_default);
   BOOST_TEST(!std::get<bool>(insecure.default_value.storage));

   const auto& path_policy = require_field(*descriptor, "path.policy");
   BOOST_TEST(path_policy.has_default);
   BOOST_TEST(std::get<std::string>(path_policy.default_value.storage) == "direct-preferred");

   const auto& relay_trust = require_field(*descriptor, "relay.trust");
   BOOST_TEST(relay_trust.has_default);
   BOOST_TEST(std::get<std::string>(relay_trust.default_value.storage) == "known-only");

   BOOST_TEST(!has_field(*descriptor, "retry.max-attempts"));
   BOOST_TEST(!has_field(*descriptor, "retry.deadline-ms"));
   BOOST_TEST(!has_field(*descriptor, "maintenance.peer-exchange-interval-ms"));
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_publishes_safe_local_api_for_route_contributions) {
   auto log = plugin_log{};
   auto app = p2p_plugin_application{log};

   app.configure(test_p2p_config(test_peer(18)));
   forge::asio::blocking::run(app.runtime(), app.startup());

   BOOST_TEST(app.apis().describe({.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0}) != nullptr);
   BOOST_TEST(log.entries == (std::vector<std::string>{"routes.published", "routes.startup"}),
              boost::test_tools::per_element());

   forge::asio::blocking::run(app.runtime(), app.shutdown());
   BOOST_TEST(log.entries == (std::vector<std::string>{"routes.published", "routes.startup", "routes.shutdown"}),
              boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_rejects_duplicate_protocol_contributions_before_startup) {
   auto app = duplicate_p2p_plugin_application{};

   app.configure(test_p2p_config(test_peer(19)));
   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), app.initialize()),
                     forge::plugins::p2p::node::exceptions::route_conflict);
}

BOOST_AUTO_TEST_CASE(p2p_node_api_rejects_facade_calls_before_initialize) {
   auto runtime = forge::asio::runtime{};
   auto plugin = forge::plugins::p2p::node::plugin{};
   auto apis = forge::api::core::registry{};
   auto provider = forge::api::core::installer{apis};
   forge::asio::blocking::run(runtime, plugin.provide(provider));

   auto p2p =
       apis.get<forge::plugins::p2p::node::api>({.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});

   BOOST_CHECK_THROW((void)p2p->local_peer(), forge::plugins::p2p::node::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)p2p->local_endpoint(), forge::plugins::p2p::node::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)p2p->local_endpoints(), forge::plugins::p2p::node::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)p2p->network_info(), forge::plugins::p2p::node::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW(forge::asio::blocking::run(
                         runtime, p2p->remote<node_test_api>(
                                      test_peer(10), forge::net::p2p::protocol_id{.value = "/forge/api/node-test/1"})),
                     forge::plugins::p2p::node::exceptions::plugin_not_initialized);
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_listens_from_config_and_exposes_local_endpoints) {
   const auto local_peer = test_peer(20);
   auto config = test_p2p_config(local_peer);
   config.set("plugins.p2p.node.listen",
              forge::config::core::value::array_type{forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"},
                                                     forge::config::core::value{"/ip4/127.0.0.1/tcp/0"}});

   auto app = p2p_only_application{};
   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.startup());

   const auto p2p = app.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   const auto endpoint = p2p->local_endpoint();
   BOOST_REQUIRE(endpoint.has_value());
   BOOST_CHECK_EQUAL(endpoint->transport.host, "127.0.0.1");
   BOOST_CHECK_NE(endpoint->transport.port, 0);
   BOOST_TEST(endpoint->peer->to_string() == local_peer.to_string());

   const auto endpoints = p2p->local_endpoints();
   BOOST_REQUIRE_EQUAL(endpoints.size(), 2U);
   BOOST_TEST(endpoints[0].peer->to_string() == local_peer.to_string());
   BOOST_TEST(endpoints[1].peer->to_string() == local_peer.to_string());

   const auto info = p2p->network_info();
   BOOST_TEST(info.local_peer.to_string() == local_peer.to_string());
   BOOST_TEST(info.started);
   BOOST_TEST(info.local_endpoints.size() == endpoints.size());

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_without_peer_id_does_not_inject_test_peer) {
   auto config = forge::config::core::document{};
   config.set("plugins.p2p.node.allow-insecure-test-mode", true);
   config.set("plugins.p2p.node.listen",
              forge::config::core::value::array_type{forge::config::core::value{"/ip4/127.0.0.1/tcp/0"}});

   auto app = p2p_only_application{};
   app.configure(config);
   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), app.startup()),
                     forge::net::p2p::exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_opens_remote_api_over_p2p_stream) {
   const auto server_peer = test_peer(30);
   auto server_config = test_p2p_config(server_peer);
   server_config.set("plugins.p2p.node.listen", forge::config::core::value::array_type{
                                                    forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});

   auto log = plugin_log{};
   auto server = p2p_plugin_application{log};
   server.configure(server_config);
   forge::asio::blocking::run(server.runtime(), server.startup());

   auto server_p2p = server.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   const auto server_endpoint = server_p2p->local_endpoint();
   BOOST_REQUIRE(server_endpoint.has_value());

   const auto client_peer = test_peer(31);
   auto client_config = test_p2p_config(client_peer);
   client_config.set("plugins.p2p.node.bootstrap",
                     forge::config::core::value::array_type{forge::config::core::value{server_endpoint->to_string()}});

   auto client = p2p_only_application{};
   client.configure(client_config);
   forge::asio::blocking::run(client.runtime(), client.startup());

   auto client_p2p = client.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   auto remote = forge::asio::blocking::run(
       client.runtime(),
       client_p2p->remote<node_test_api>(server_p2p->local_peer(),
                                         forge::net::p2p::protocol_id{.value = "/forge/api/node-test/1"}));
   const auto response = forge::asio::blocking::run(client.runtime(), remote->ping(41));
   BOOST_TEST(response == 42);

   forge::asio::blocking::run(client.runtime(), client.shutdown());
   forge::asio::blocking::run(server.runtime(), server.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_typed_trusted_invocation_replaces_spoofed_peer_context) {
   auto runtime = forge::asio::runtime{};
   auto registry = forge::api::core::registry{};
   registry.install<peer_context_test_api>(peer_context_test_api::describe(),
                                           std::make_shared<peer_context_test_api_impl>());

   const auto authenticated = test_peer(232);
   const auto spoofed = test_peer(233);
   auto request = forge::api::core::frame{
       .kind = forge::api::core::frame_kind::request,
       .id = {.value = 901},
       .api = peer_context_test_api::ref(),
       .method = "remote_peer",
       .meta = {{
           .key = std::string{forge::api::core::p2p_remote_peer_metadata_key},
           .value = spoofed.to_string(),
       }},
       .codec = {.value = "forge.raw"},
       .payload = forge::api::core::pack_body(peer_context_request{
           .value = "probe",
           .caller = {.id = spoofed},
       }),
   };
   const auto trusted = forge::api::core::trusted_invocation_builder{}
                            .set(forge::api::p2p::authenticated_peer{.id = authenticated})
                            .build();

   const auto response = forge::asio::blocking::run(runtime, registry.dispatch_contextual(std::move(request), trusted));

   BOOST_REQUIRE(response.kind == forge::api::core::frame_kind::response);
   BOOST_TEST(forge::api::core::unpack_body<std::string>(response.payload) == authenticated.to_string() + ":probe");
   BOOST_TEST(forge::api::core::unpack_body<std::string>(response.payload) != spoofed.to_string() + ":probe");
}

BOOST_AUTO_TEST_CASE(p2p_publication_is_move_only_and_joins_concurrent_closers) {
   auto runtime = forge::asio::runtime{};

   auto destructor_closes = std::atomic_size_t{0};
   {
      auto publication = forge::api::p2p::detail::publication_access::make(
          runtime.context().get_executor(),
          [&destructor_closes] { destructor_closes.fetch_add(1, std::memory_order_relaxed); },
          []() -> boost::asio::awaitable<void> { co_return; }, [] { return true; });
      BOOST_TEST(publication.active());
   }
   BOOST_TEST(destructor_closes.load(std::memory_order_relaxed) == 1U);

   auto active_closes = std::atomic_size_t{0};
   auto active_owner = std::unique_ptr<forge::api::p2p::publication>{};
   active_owner = std::make_unique<forge::api::p2p::publication>(forge::api::p2p::detail::publication_access::make(
       runtime.context().get_executor(), [&active_closes] { active_closes.fetch_add(1, std::memory_order_relaxed); },
       []() -> boost::asio::awaitable<void> { co_return; },
       [&active_owner] {
          active_owner.reset();
          return true;
       }));
   BOOST_CHECK_NO_THROW(static_cast<void>(active_owner->active()));
   BOOST_TEST(!active_owner);
   BOOST_TEST(active_closes.load(std::memory_order_relaxed) == 1U);

   auto close_closes = std::atomic_size_t{0};
   auto close_owner = std::unique_ptr<forge::api::p2p::publication>{};
   close_owner = std::make_unique<forge::api::p2p::publication>(forge::api::p2p::detail::publication_access::make(
       runtime.context().get_executor(),
       [&close_closes, &close_owner] {
          close_closes.fetch_add(1, std::memory_order_relaxed);
          close_owner.reset();
       },
       []() -> boost::asio::awaitable<void> { co_return; }, [] { return true; }));
   BOOST_CHECK_NO_THROW(close_owner->close());
   BOOST_TEST(!close_owner);
   BOOST_TEST(close_closes.load(std::memory_order_relaxed) == 1U);

   struct close_state {
      std::atomic_size_t close_calls{0};
      std::atomic_size_t drain_calls{0};
      forge::asio::notification drain_entered;
      forge::asio::notification release_drain;
   };

   auto state = std::make_shared<close_state>();
   auto publication = forge::api::p2p::detail::publication_access::make(
       runtime.context().get_executor(), [state] { state->close_calls.fetch_add(1, std::memory_order_relaxed); },
       [state]() -> boost::asio::awaitable<void> {
          const auto release_epoch = state->release_drain.epoch();
          state->drain_calls.fetch_add(1, std::memory_order_relaxed);
          state->drain_entered.notify();
          co_await state->release_drain.async_wait(release_epoch);
       },
       [] { return true; });

   auto moved = std::move(publication);
   BOOST_TEST(!publication.active());
   BOOST_TEST(moved.active());
   moved.close();
   moved.close();
   BOOST_TEST(!moved.active());

   const auto drain_entered_epoch = state->drain_entered.epoch();
   auto first = boost::asio::co_spawn(runtime.context(), moved.async_close(), boost::asio::use_future);
   auto second = boost::asio::co_spawn(runtime.context(), moved.async_close(), boost::asio::use_future);
   auto third = boost::asio::co_spawn(runtime.context(), moved.async_close(), boost::asio::use_future);
   auto drain_entered =
       boost::asio::co_spawn(runtime.context(),
                             state->drain_entered.async_wait_until(
                                 drain_entered_epoch, std::chrono::steady_clock::now() + std::chrono::seconds{2}),
                             boost::asio::use_future);
   BOOST_REQUIRE(drain_entered.wait_for(std::chrono::seconds{3}) == std::future_status::ready);
   BOOST_REQUIRE(drain_entered.get() != drain_entered_epoch);
   BOOST_TEST(state->close_calls.load(std::memory_order_relaxed) == 1U);
   state->release_drain.notify();
   BOOST_REQUIRE(first.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_REQUIRE(second.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_REQUIRE(third.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(first.get());
   BOOST_CHECK_NO_THROW(second.get());
   BOOST_CHECK_NO_THROW(third.get());
   BOOST_TEST(state->drain_calls.load(std::memory_order_relaxed) == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_publication_move_assignment_transfers_new_owner_before_reentrant_old_close) {
   auto runtime = forge::asio::runtime{};
   auto old_close_calls = std::atomic_size_t{0};
   auto new_close_calls = std::atomic_size_t{0};
   auto owner = std::unique_ptr<forge::api::p2p::publication>{};

   auto replacement = forge::api::p2p::detail::publication_access::make(
       runtime.context().get_executor(),
       [&new_close_calls] { new_close_calls.fetch_add(1, std::memory_order_relaxed); },
       []() -> boost::asio::awaitable<void> { co_return; }, [] { return true; });
   owner = std::make_unique<forge::api::p2p::publication>(forge::api::p2p::detail::publication_access::make(
       runtime.context().get_executor(),
       [&old_close_calls, &owner] {
          old_close_calls.fetch_add(1, std::memory_order_relaxed);
          owner.reset();
       },
       []() -> boost::asio::awaitable<void> { co_return; }, [] { return true; }));

   BOOST_CHECK_NO_THROW(static_cast<void>(*owner = std::move(replacement)));
   BOOST_TEST(!owner);
   BOOST_TEST(old_close_calls.load(std::memory_order_relaxed) == 1U);
   BOOST_TEST(new_close_calls.load(std::memory_order_relaxed) == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_publication_drain_uses_its_owner_runtime_after_first_caller_stops) {
   auto owner_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto first_caller_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto late_caller_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};

   struct drain_state {
      std::atomic_size_t close_calls{0};
      std::atomic_size_t drain_calls{0};
      forge::asio::notification drain_entered;
      forge::asio::notification release_drain;
      forge::asio::notification drain_completed;
   };

   auto state = std::make_shared<drain_state>();
   auto publication = forge::api::p2p::detail::publication_access::make(
       owner_runtime.context().get_executor(), [state] { state->close_calls.fetch_add(1, std::memory_order_relaxed); },
       [state]() -> boost::asio::awaitable<void> {
          const auto release_epoch = state->release_drain.epoch();
          state->drain_calls.fetch_add(1, std::memory_order_relaxed);
          state->drain_entered.notify();
          co_await state->release_drain.async_wait(release_epoch);
          state->drain_completed.notify();
       },
       [] { return true; });

   const auto drain_entered_epoch = state->drain_entered.epoch();
   auto first =
       boost::asio::co_spawn(first_caller_runtime.context(), publication.async_close(), boost::asio::use_future);
   auto drain_entered =
       boost::asio::co_spawn(owner_runtime.context(),
                             state->drain_entered.async_wait_until(
                                 drain_entered_epoch, std::chrono::steady_clock::now() + std::chrono::seconds{2}),
                             boost::asio::use_future);
   BOOST_REQUIRE(drain_entered.wait_for(std::chrono::seconds{3}) == std::future_status::ready);
   BOOST_REQUIRE(drain_entered.get() != drain_entered_epoch);

   first_caller_runtime.stop();
   BOOST_CHECK(first.wait_for(std::chrono::milliseconds{50}) != std::future_status::ready);
   const auto drain_completed_epoch = state->drain_completed.epoch();
   state->release_drain.notify();
   auto drain_completed =
       boost::asio::co_spawn(owner_runtime.context(),
                             state->drain_completed.async_wait_until(
                                 drain_completed_epoch, std::chrono::steady_clock::now() + std::chrono::seconds{2}),
                             boost::asio::use_future);
   BOOST_REQUIRE(drain_completed.wait_for(std::chrono::seconds{3}) == std::future_status::ready);
   BOOST_REQUIRE(drain_completed.get() != drain_completed_epoch);

   auto late = boost::asio::co_spawn(late_caller_runtime.context(), publication.async_close(), boost::asio::use_future);
   BOOST_REQUIRE(late.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(late.get());
   BOOST_TEST(state->close_calls.load(std::memory_order_relaxed) == 1U);
   BOOST_TEST(state->drain_calls.load(std::memory_order_relaxed) == 1U);

   BOOST_CHECK_THROW(static_cast<void>(forge::api::p2p::detail::publication_access::make(
                         boost::asio::any_io_executor{}, [] {}, []() -> boost::asio::awaitable<void> { co_return; },
                         [] { return true; })),
                     forge::api::core::exceptions::protocol_error);
}

BOOST_AUTO_TEST_CASE(p2p_node_publication_closed_before_startup_does_not_reserve_route) {
   const auto server_peer = test_peer(234);
   auto config = test_p2p_config(server_peer);
   config.set("plugins.p2p.node.listen",
              forge::config::core::value::array_type{forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});
   auto server = p2p_only_application{};
   server.configure(config);
   forge::asio::blocking::run(server.runtime(), server.initialize());

   auto server_p2p = server.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   auto staged_apis = forge::api::core::registry{};
   staged_apis.install<node_test_api>(node_test_api::describe(), std::make_shared<node_test_api_impl>());
   const auto protocol = forge::net::p2p::protocol_id{.value = "/forge/api/publication-before-startup/1"};
   auto closed = server_p2p->publish_api(node_test_plan(staged_apis), protocol);
   BOOST_TEST(closed.active());
   closed.close();
   BOOST_TEST(!closed.active());

   forge::asio::blocking::run(server.runtime(), server.startup());
   auto active = server_p2p->publish_api(node_test_plan(staged_apis), protocol);
   BOOST_TEST(active.active());
   const auto endpoint = server_p2p->local_endpoint();
   BOOST_REQUIRE(endpoint.has_value());

   auto client_config = test_p2p_config(test_peer(235));
   client_config.set("plugins.p2p.node.bootstrap",
                     forge::config::core::value::array_type{forge::config::core::value{endpoint->to_string()}});
   auto client = p2p_only_application{};
   client.configure(client_config);
   forge::asio::blocking::run(client.runtime(), client.startup());
   auto client_p2p = client.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   auto remote = forge::asio::blocking::run(client.runtime(), client_p2p->remote<node_test_api>(server_peer, protocol));
   BOOST_TEST(forge::asio::blocking::run(client.runtime(), remote->ping(41)) == 42);

   forge::asio::blocking::run(client.runtime(), client.shutdown());
   forge::asio::blocking::run(server.runtime(), active.async_close());
   forge::asio::blocking::run(server.runtime(), server.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_node_publication_replacement_keeps_latest_generation) {
   const auto server_peer = test_peer(236);
   auto server_config = test_p2p_config(server_peer);
   server_config.set("plugins.p2p.node.listen", forge::config::core::value::array_type{
                                                    forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});
   auto server = p2p_only_application{};
   server.configure(server_config);
   forge::asio::blocking::run(server.runtime(), server.startup());
   auto server_p2p = server.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   const auto endpoint = server_p2p->local_endpoint();
   BOOST_REQUIRE(endpoint.has_value());

   auto client_config = test_p2p_config(test_peer(237));
   client_config.set("plugins.p2p.node.bootstrap",
                     forge::config::core::value::array_type{forge::config::core::value{endpoint->to_string()}});
   auto client = p2p_only_application{};
   client.configure(client_config);
   forge::asio::blocking::run(client.runtime(), client.startup());
   auto client_p2p = client.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});

   auto first_apis = forge::api::core::registry{};
   first_apis.install<node_test_api>(node_test_api::describe(), std::make_shared<offset_node_test_api_impl>(1));
   auto second_apis = forge::api::core::registry{};
   second_apis.install<node_test_api>(node_test_api::describe(), std::make_shared<offset_node_test_api_impl>(100));
   const auto protocol = forge::net::p2p::protocol_id{.value = "/forge/api/publication-replacement/1"};
   auto first = server_p2p->publish_api(node_test_plan(first_apis), protocol);
   {
      auto remote =
          forge::asio::blocking::run(client.runtime(), client_p2p->remote<node_test_api>(server_peer, protocol));
      BOOST_TEST(forge::asio::blocking::run(client.runtime(), remote->ping(41)) == 42);
   }

   auto second = server_p2p->publish_api(node_test_plan(second_apis), protocol);
   BOOST_TEST(!first.active());
   BOOST_TEST(second.active());
   first.close();
   BOOST_TEST(second.active());
   {
      auto remote =
          forge::asio::blocking::run(client.runtime(), client_p2p->remote<node_test_api>(server_peer, protocol));
      BOOST_TEST(forge::asio::blocking::run(client.runtime(), remote->ping(41)) == 141);
   }

   forge::asio::blocking::run(client.runtime(), client.shutdown());
   forge::asio::blocking::run(server.runtime(), second.async_close());
   forge::asio::blocking::run(server.runtime(), server.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_node_publication_async_close_cancels_admitted_session) {
   const auto server_peer = test_peer(238);
   auto server_config = test_p2p_config(server_peer);
   server_config.set("plugins.p2p.node.listen", forge::config::core::value::array_type{
                                                    forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});
   auto server = p2p_only_application{};
   server.configure(server_config);
   forge::asio::blocking::run(server.runtime(), server.startup());
   auto server_p2p = server.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   const auto endpoint = server_p2p->local_endpoint();
   BOOST_REQUIRE(endpoint.has_value());

   auto handler_state = std::make_shared<nonresponding_node_test_state>();
   auto apis = forge::api::core::registry{};
   apis.install<node_test_api>(node_test_api::describe(),
                               std::make_shared<nonresponding_node_test_api_impl>(handler_state));
   const auto protocol = forge::net::p2p::protocol_id{.value = "/forge/api/publication-drain/1"};
   auto publication = server_p2p->publish_api(node_test_plan(apis), protocol);

   auto client_config = test_p2p_config(test_peer(239));
   client_config.set("plugins.p2p.node.bootstrap",
                     forge::config::core::value::array_type{forge::config::core::value{endpoint->to_string()}});
   auto client = p2p_only_application{};
   client.configure(client_config);
   forge::asio::blocking::run(client.runtime(), client.startup());
   auto client_p2p = client.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   auto remote = forge::asio::blocking::run(client.runtime(), client_p2p->remote<node_test_api>(server_peer, protocol));
   auto call = boost::asio::co_spawn(client.runtime().context(), remote->ping(41), boost::asio::use_future);
   BOOST_REQUIRE(forge::asio::blocking::run(
       server.runtime(),
       async_wait_for_condition([&] { return handler_state->started.load(std::memory_order_acquire); },
                                std::chrono::seconds{2})));

   publication.close();
   BOOST_TEST(!publication.active());
   auto drained = boost::asio::co_spawn(server.runtime().context(), publication.async_close(), boost::asio::use_future);
   BOOST_REQUIRE(drained.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(drained.get());
   BOOST_REQUIRE(forge::asio::blocking::run(
       server.runtime(),
       async_wait_for_condition([&] { return handler_state->cancelled.load(std::memory_order_acquire); },
                                std::chrono::seconds{2})));
   BOOST_REQUIRE(call.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(call.get()), std::exception);

   forge::asio::blocking::run(client.runtime(), client.shutdown());
   forge::asio::blocking::run(server.runtime(), server.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_resolver_publication_replacement_keeps_catalog_and_route_current) {
   const auto server_peer = test_peer(240);
   auto server_config = test_p2p_config(server_peer);
   server_config.set("plugins.p2p.node.listen", forge::config::core::value::array_type{
                                                    forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});
   auto server = resolver_publication_application{};
   server.configure(server_config);
   forge::asio::blocking::run(server.runtime(), server.startup());
   auto resolver = server.apis().get<forge::plugins::p2p::resolver::api>(
       {.id = {"forge.plugins.p2p.resolver"}, .major = 2, .min_revision = 0});
   auto server_p2p = server.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   const auto endpoint = server_p2p->local_endpoint();
   BOOST_REQUIRE(endpoint.has_value());

   auto client_config = test_p2p_config(test_peer(241));
   client_config.set("plugins.p2p.node.bootstrap",
                     forge::config::core::value::array_type{forge::config::core::value{endpoint->to_string()}});
   auto client = p2p_only_application{};
   client.configure(client_config);
   forge::asio::blocking::run(client.runtime(), client.startup());
   auto client_p2p = client.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});

   const auto protocol = forge::net::p2p::protocol_id{.value = "/forge/api/resolver-publication/1"};
   auto first = resolver->publish_api(node_test_plan(server.apis()), protocol);
   const auto first_entries = resolver->local_apis();
   BOOST_REQUIRE_EQUAL(first_entries.size(), 1U);
   BOOST_TEST(first_entries.front().id.value == "node.test");
   {
      auto remote =
          forge::asio::blocking::run(client.runtime(), client_p2p->remote<node_test_api>(server_peer, protocol));
      BOOST_TEST(forge::asio::blocking::run(client.runtime(), remote->ping(41)) == 42);
   }

   auto second = resolver->publish_api(peer_context_test_plan(server.apis()), protocol);
   BOOST_TEST(!first.active());
   BOOST_TEST(second.active());
   const auto replacement_entries = resolver->local_apis();
   BOOST_REQUIRE_EQUAL(replacement_entries.size(), 1U);
   BOOST_TEST(replacement_entries.front().id.value == "peer-context.test");
   first.close();
   BOOST_TEST(second.active());
   BOOST_REQUIRE_EQUAL(resolver->local_apis().size(), 1U);
   {
      auto remote = forge::asio::blocking::run(client.runtime(),
                                               client_p2p->remote<peer_context_test_api>(server_peer, protocol));
      static_cast<void>(remote);
   }

   second.close();
   BOOST_TEST(resolver->local_apis().empty());
   forge::asio::blocking::run(server.runtime(), second.async_close());
   auto post_close_apis = forge::api::core::registry{};
   post_close_apis.install<node_test_api>(node_test_api::describe(), std::make_shared<node_test_api_impl>());
   auto post_close = server_p2p->publish_api(node_test_plan(post_close_apis), protocol);
   BOOST_TEST(post_close.active());

   forge::asio::blocking::run(client.runtime(), client.shutdown());
   forge::asio::blocking::run(server.runtime(), post_close.async_close());
   forge::asio::blocking::run(server.runtime(), server.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_resolver_repeated_publication_replacement_retires_before_shutdown) {
   auto server = resolver_publication_application{};
   server.configure(test_p2p_config(test_peer(243)));
   forge::asio::blocking::run(server.runtime(), server.startup());
   auto resolver = server.apis().get<forge::plugins::p2p::resolver::api>(
       {.id = {"forge.plugins.p2p.resolver"}, .major = 2, .min_revision = 0});

   const auto protocol = forge::net::p2p::protocol_id{.value = "/forge/api/resolver-replacement-bounded/1"};
   constexpr auto replacement_count = std::size_t{256};
   const auto before = server.scheduler().snapshot();
   auto publication = resolver->publish_api(node_test_plan(server.apis()), protocol);
   for (auto index = std::size_t{0}; index < replacement_count; ++index) {
      publication = resolver->publish_api(
          index % 2 == 0 ? peer_context_test_plan(server.apis()) : node_test_plan(server.apis()), protocol);
      BOOST_TEST(publication.active());
      BOOST_REQUIRE_EQUAL(resolver->local_apis().size(), 1U);
   }

   BOOST_REQUIRE(forge::asio::blocking::run(server.runtime(),
                                            async_wait_for_condition(
                                                [&] {
                                                   const auto metrics = server.scheduler().snapshot();
                                                   return metrics.submitted >= before.submitted + replacement_count &&
                                                          metrics.completed >= before.completed + replacement_count &&
                                                          metrics.pending == 0U && metrics.running_awaitable == 0U;
                                                },
                                                std::chrono::seconds{2})));

   publication.close();
   forge::asio::blocking::run(server.runtime(), publication.async_close());
   forge::asio::blocking::run(server.runtime(), server.shutdown());
   const auto after_shutdown = server.scheduler().snapshot();
   BOOST_TEST(after_shutdown.pending == 0U);
   BOOST_TEST(after_shutdown.running_awaitable == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_resolver_rejected_retirement_drains_on_shutdown_without_poisoning_close) {
   auto barrier = std::make_shared<forge::tests::plugins::resolver_publish_barrier>();
   auto app = forge::tests::plugins::resolver_publication_race_application{barrier};
   forge::asio::blocking::run(app.runtime(), app.startup());
   auto resolver = app.apis().get<forge::plugins::p2p::resolver::api>(
       {.id = {"forge.plugins.p2p.resolver"}, .major = 2, .min_revision = 0});

   const auto protocol = std::string{"/forge/api/resolver-retirement-rejected/1"};
   auto publication =
       resolver->publish_api(node_test_plan(app.apis()), forge::net::p2p::protocol_id{.value = protocol});
   BOOST_TEST(publication.active());

   const auto before_rejection = app.scheduler().snapshot();
   app.scheduler().request_stop();
   publication.close();
   const auto after_rejection = app.scheduler().snapshot();

   BOOST_TEST(after_rejection.stopped);
   BOOST_TEST(after_rejection.rejected == before_rejection.rejected + 1U);
   BOOST_TEST(barrier->close_calls(protocol) == 1U);
   BOOST_TEST(barrier->drain_calls(protocol) == 0U);

   BOOST_CHECK_NO_THROW(forge::asio::blocking::run(app.runtime(), app.shutdown()));
   BOOST_TEST(barrier->drain_calls(protocol) == 1U);
   BOOST_CHECK_NO_THROW(forge::asio::blocking::run(app.runtime(), publication.async_close()));
   BOOST_TEST(barrier->drain_calls(protocol) == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_resolver_canceled_accepted_retirement_falls_back_during_shutdown) {
   struct retirement_blocker_state {
      forge::asio::notification started;
      forge::asio::notification release;
   };

   auto barrier = std::make_shared<forge::tests::plugins::resolver_publish_barrier>();
   auto app = forge::tests::plugins::resolver_publication_race_application{
       barrier, forge::app::application_shell_options{
                    .runtime = forge::asio::runtime_options{.worker_threads = 2},
                    .scheduler =
                        forge::asio::task::scheduler::options{
                            .max_blocking_tasks = 1,
                            .max_awaitable_tasks = 1,
                            .max_pending_tasks = 4,
                        },
                }};
   forge::asio::blocking::run(app.runtime(), app.startup());
   auto resolver = app.apis().get<forge::plugins::p2p::resolver::api>(
       {.id = {"forge.plugins.p2p.resolver"}, .major = 2, .min_revision = 0});

   auto blocker_state = std::make_shared<retirement_blocker_state>();
   [[maybe_unused]] auto release_on_exit =
       boost::scope::scope_exit{[state = blocker_state] { state->release.notify(); }};
   const auto blocker_started_epoch = blocker_state->started.epoch();
   const auto release_epoch = blocker_state->release.epoch();
   auto blocker = app.scheduler().submit(forge::asio::task::awaitable{
       .priority = forge::asio::task::priority{100},
       .name = "resolver-retirement-blocker",
       .work = [state = blocker_state, release_epoch](forge::asio::task::context&) -> boost::asio::awaitable<void> {
          state->started.notify();
          co_await state->release.async_wait(release_epoch);
          co_return;
       },
   });
   BOOST_REQUIRE(blocker.accepted());
   auto blocker_entered =
       boost::asio::co_spawn(app.runtime().context(),
                             blocker_state->started.async_wait_until(
                                 blocker_started_epoch, std::chrono::steady_clock::now() + std::chrono::seconds{2}),
                             boost::asio::use_future);
   BOOST_REQUIRE(blocker_entered.wait_for(std::chrono::seconds{3}) == std::future_status::ready);
   BOOST_REQUIRE(blocker_entered.get() != blocker_started_epoch);

   const auto protocol = std::string{"/forge/api/resolver-retirement-canceled/1"};
   auto publication =
       resolver->publish_api(node_test_plan(app.apis()), forge::net::p2p::protocol_id{.value = protocol});
   publication.close();
   const auto queued = app.scheduler().snapshot();
   BOOST_TEST(queued.running_awaitable == 1U);
   BOOST_TEST(queued.pending == 1U);

   app.scheduler().request_stop();
   const auto canceled = app.scheduler().snapshot();
   BOOST_TEST(canceled.stopped);
   BOOST_TEST(canceled.pending == 0U);
   BOOST_TEST(canceled.canceled >= 1U);
   blocker_state->release.notify();
   BOOST_CHECK_NO_THROW(forge::asio::blocking::run(app.runtime(), blocker.wait()));

   BOOST_CHECK_NO_THROW(forge::asio::blocking::run(app.runtime(), app.shutdown()));
   BOOST_TEST(barrier->close_calls(protocol) == 1U);
   BOOST_TEST(barrier->drain_calls(protocol) == 1U);
}

BOOST_AUTO_TEST_CASE(p2p_resolver_rejected_retirement_backlog_fails_closed_before_new_child_publication) {
   auto barrier = std::make_shared<forge::tests::plugins::resolver_publish_barrier>();
   auto app = forge::tests::plugins::resolver_publication_race_application{barrier};
   auto config = forge::config::core::document{};
   config.set("plugins.p2p.resolver.max-apis-per-peer", std::uint64_t{2});
   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.startup());
   auto resolver = app.apis().get<forge::plugins::p2p::resolver::api>(
       {.id = {"forge.plugins.p2p.resolver"}, .major = 2, .min_revision = 0});

   const auto protocol = std::string{"/forge/api/resolver-retirement-backlog/1"};
   auto first = resolver->publish_api(node_test_plan(app.apis()), forge::net::p2p::protocol_id{.value = protocol});
   app.scheduler().request_stop();
   auto second =
       resolver->publish_api(peer_context_test_plan(app.apis()), forge::net::p2p::protocol_id{.value = protocol});
   auto third = resolver->publish_api(node_test_plan(app.apis()), forge::net::p2p::protocol_id{.value = protocol});

   BOOST_TEST(barrier->publish_calls(protocol) == 3U);
   BOOST_CHECK_THROW(static_cast<void>(resolver->publish_api(peer_context_test_plan(app.apis()),
                                                             forge::net::p2p::protocol_id{.value = protocol})),
                     forge::plugins::p2p::resolver::exceptions::protocol_error);
   BOOST_TEST(barrier->publish_calls(protocol) == 3U);
   BOOST_TEST(app.scheduler().snapshot().rejected == 2U);

   BOOST_CHECK_NO_THROW(forge::asio::blocking::run(app.runtime(), app.shutdown()));
   BOOST_TEST(barrier->close_calls(protocol) == 3U);
   BOOST_TEST(barrier->drain_calls(protocol) == 3U);
}

BOOST_AUTO_TEST_CASE(p2p_resolver_publish_racing_shutdown_returns_no_usable_publication) {
   auto barrier = std::make_shared<forge::tests::plugins::resolver_publish_barrier>();
   auto app = forge::tests::plugins::resolver_publication_race_application{barrier};
   forge::asio::blocking::run(app.runtime(), app.startup());
   auto resolver = app.apis().get<forge::plugins::p2p::resolver::api>(
       {.id = {"forge.plugins.p2p.resolver"}, .major = 2, .min_revision = 0});

   const auto race_protocol = std::string{"/forge/api/resolver-publish-race/1"};
   auto plan = node_test_plan(app.apis());
   auto failure = std::exception_ptr{};
   auto publication = std::optional<forge::api::p2p::publication>{};
   auto result_mutex = std::mutex{};
   barrier->arm();
   auto publisher = std::thread{[&] {
      try {
         auto published = resolver->publish_api(std::move(plan), forge::net::p2p::protocol_id{.value = race_protocol});
         const auto lock = std::scoped_lock{result_mutex};
         publication.emplace(std::move(published));
      } catch (...) {
         const auto lock = std::scoped_lock{result_mutex};
         failure = std::current_exception();
      }
   }};

   BOOST_REQUIRE(barrier->wait_until_entered(std::chrono::seconds{2}));
   app.request_stop();
   barrier->release();
   publisher.join();

   {
      const auto lock = std::scoped_lock{result_mutex};
      BOOST_TEST(!publication.has_value());
      BOOST_REQUIRE(failure);
      BOOST_CHECK_THROW(std::rethrow_exception(failure),
                        forge::plugins::p2p::resolver::exceptions::plugin_not_initialized);
   }
   BOOST_TEST(barrier->close_calls(race_protocol) == 1U);

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_publication_remains_safe_after_node_plugin_shutdown) {
   auto app = p2p_only_application{};
   app.configure(test_p2p_config(test_peer(242)));
   forge::asio::blocking::run(app.runtime(), app.startup());
   auto p2p = app.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   auto apis = forge::api::core::registry{};
   apis.install<node_test_api>(node_test_api::describe(), std::make_shared<node_test_api_impl>());
   auto publication = p2p->publish_api(node_test_plan(apis),
                                       forge::net::p2p::protocol_id{.value = "/forge/api/publication-shutdown/1"});
   BOOST_TEST(publication.active());

   forge::asio::blocking::run(app.runtime(), app.shutdown());
   BOOST_TEST(!publication.active());
   BOOST_CHECK_NO_THROW(forge::asio::blocking::run(app.runtime(), publication.async_close()));
   BOOST_CHECK_NO_THROW(forge::asio::blocking::run(app.runtime(), publication.async_close()));
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_rejects_invalid_typed_config_before_startup) {
   {
      auto config = test_p2p_config();
      config.set("plugins.p2p.node.max-inflight-per-peer", std::uint64_t{0});
      auto app = p2p_only_application{};
      BOOST_CHECK_THROW(app.configure(config), forge::plugins::p2p::node::exceptions::invalid_config);
   }

   {
      auto config = test_p2p_config();
      config.set("plugins.p2p.node.topology.peers.low", std::uint64_t{5});
      config.set("plugins.p2p.node.topology.peers.target", std::uint64_t{4});
      auto app = p2p_only_application{};
      BOOST_CHECK_THROW(app.configure(config), forge::plugins::p2p::node::exceptions::invalid_config);
   }

   {
      auto config = test_p2p_config();
      config.set("plugins.p2p.node.rendezvous.points",
                 forge::config::core::value::array_type{forge::config::core::value{rendezvous_point_config(
                     "/ip4/127.0.0.1/udp/4001/quic-v1/p2p/" + test_peer(91).to_string(), {"forge.content"})}});
      auto app = p2p_only_application{};
      BOOST_CHECK_THROW(app.configure(config), forge::plugins::p2p::node::exceptions::invalid_config);
   }

   {
      auto config = test_p2p_config();
      config.set("plugins.p2p.node.rendezvous.role", std::string{"client"});
      config.set("plugins.p2p.node.rendezvous.points",
                 forge::config::core::value::array_type{forge::config::core::value{rendezvous_point_config(
                     "/ip4/127.0.0.1/udp/4001/quic-v1/p2p/" + test_peer(92).to_string(), {"forge.content"})}});
      auto app = p2p_only_application{};
      BOOST_CHECK_NO_THROW(app.configure(config));
   }

   {
      auto config = test_p2p_config();
      config.set("plugins.p2p.node.listen",
                 forge::config::core::value::array_type{forge::config::core::value{"127.0.0.1:0"}});
      auto app = p2p_only_application{};
      BOOST_CHECK_THROW(app.configure(config), forge::plugins::p2p::node::exceptions::invalid_config);
   }

   {
      auto config = test_p2p_config();
      config.set("plugins.p2p.node.api.max-frame-size", std::uint64_t{0});
      auto app = p2p_only_application{};
      BOOST_CHECK_THROW(app.configure(config), forge::plugins::p2p::node::exceptions::invalid_config);
   }

   {
      auto config = test_p2p_config();
      config.set("plugins.p2p.node.bootstrap", forge::config::core::value::array_type{
                                                   forge::config::core::value{"/ip4/127.0.0.1/tcp/1"},
                                               });
      auto app = p2p_only_application{};
      BOOST_CHECK_NO_THROW(app.configure(config));
   }

   {
      auto config = test_p2p_config();
      config.set("plugins.p2p.node.bootstrap-requirement", std::string{"always-online"});
      auto app = p2p_only_application{};
      BOOST_CHECK_THROW(app.configure(config), forge::plugins::p2p::node::exceptions::invalid_config);
   }

   {
      auto config = test_p2p_config();
      config.set("plugins.p2p.node.path.policy", std::string{"teleport"});
      auto app = p2p_only_application{};
      BOOST_CHECK_EXCEPTION(app.configure(config), forge::plugins::p2p::node::exceptions::invalid_config,
                            [](const auto& error) {
                               const auto text = std::string{error.what()};
                               return text.find("plugins.p2p.node.path.policy") != std::string::npos &&
                                      text.find("config.type") != std::string::npos;
                            });
   }

   {
      auto config = test_p2p_config();
      config.set("plugins.p2p.node.path.policy", std::string{"relay-only"});
      auto app = p2p_only_application{};
      BOOST_CHECK_NO_THROW(app.configure(config));
   }

   {
      auto config = test_p2p_config();
      config.set("plugins.p2p.node.relay.trust", std::string{"public-allowed"});
      auto app = p2p_only_application{};
      BOOST_CHECK_NO_THROW(app.configure(config));
   }

   {
      auto config = test_p2p_config();
      config.set("plugins.p2p.node.relay.trust", std::string{"everyone"});
      auto app = p2p_only_application{};
      BOOST_CHECK_EXCEPTION(app.configure(config), forge::plugins::p2p::node::exceptions::invalid_config,
                            [](const auto& error) {
                               const auto text = std::string{error.what()};
                               return text.find("plugins.p2p.node.relay.trust") != std::string::npos &&
                                      text.find("config.type") != std::string::npos;
                            });
   }

   {
      auto config = test_p2p_config();
      config.set("plugins.p2p.node.dht.profiles",
                 forge::config::core::value::array_type{
                     forge::config::core::value{dht_profile_config("amino-v1", "server", "/forge/not-amino/1.0.0")}});
      auto app = p2p_only_application{};
      BOOST_CHECK_THROW(app.configure(config), forge::plugins::p2p::node::exceptions::invalid_config);
   }

   {
      auto config = test_p2p_config();
      config.set("plugins.p2p.node.dht.profiles",
                 forge::config::core::value::array_type{forge::config::core::value{
                     dht_profile_config("custom", "server", "/forge/product/kad/1.0.0", true, true, true)}});
      auto app = p2p_only_application{};
      BOOST_CHECK_THROW(app.configure(config), forge::plugins::p2p::node::exceptions::invalid_config);
   }

   {
      auto config = test_p2p_config();
      const auto duplicate = dht_profile_config("custom", "client", "/forge/product/kad/1.0.0", true, true, false);
      config.set("plugins.p2p.node.dht.profiles",
                 forge::config::core::value::array_type{forge::config::core::value{duplicate},
                                                        forge::config::core::value{duplicate}});
      auto app = p2p_only_application{};
      BOOST_CHECK_THROW(app.configure(config), forge::plugins::p2p::node::exceptions::invalid_config);
   }
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_accepts_complete_amino_and_provider_only_custom_profiles) {
   auto config = test_p2p_config();
   config.set("plugins.p2p.node.dht.profiles",
              forge::config::core::value::array_type{
                  forge::config::core::value{dht_profile_config("amino-v1", "server", "/ipfs/kad/1.0.0")},
                  forge::config::core::value{
                      dht_profile_config("custom", "client", "/forge/content/providers/1.0.0", true, true, false)},
              });
   auto app = p2p_only_application{};
   BOOST_CHECK_NO_THROW(app.configure(config));
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_static_topology_starts_without_autonomous_discovery) {
   auto config = test_p2p_config(test_peer(93));
   config.set("plugins.p2p.node.topology.mode", std::string{"static-only"});
   auto app = p2p_only_application{};
   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.startup());

   const auto diagnostics = app.apis().get<forge::plugins::p2p::node::diagnostics_source>(
       {.id = {"forge.plugins.p2p.node.diagnostics_source"}, .major = 2, .min_revision = 0});
   const auto snapshot = diagnostics->snapshot();
   BOOST_TEST(snapshot.topology.mode == "static-only");
   BOOST_TEST(snapshot.topology.phase == "idle");
   BOOST_TEST(snapshot.topology.completed_refreshes == 0U);
   BOOST_TEST(!snapshot.topology.refresh_queued);
   BOOST_TEST(!snapshot.topology.refresh_in_flight);

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_normalizes_managed_topology_to_hard_session_capacity) {
   auto config = test_p2p_config(test_peer(94));
   config.set("plugins.p2p.node.max-sessions", std::uint64_t{1});
   auto app = p2p_only_application{};
   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.startup());

   const auto diagnostics = app.apis().get<forge::plugins::p2p::node::diagnostics_source>(
       {.id = {"forge.plugins.p2p.node.diagnostics_source"}, .major = 2, .min_revision = 0});
   const auto snapshot = diagnostics->snapshot();
   const auto topology = snapshot.topology;
   const auto& system = snapshot.effective_limits.system;
   BOOST_TEST(system.max_connections == 2U);
   BOOST_TEST(system.max_inbound_connections == 2U);
   BOOST_TEST(system.max_outbound_connections == 2U);
   BOOST_TEST(topology.low_watermark == 1U);
   BOOST_TEST(topology.target_watermark == 1U);
   BOOST_TEST(topology.high_watermark == 192U);

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_reserves_a_directional_provisional_session_slot) {
   auto config = test_p2p_config(test_peer(95));
   config.set("plugins.p2p.node.max-sessions", std::uint64_t{1024});
   auto app = p2p_only_application{};
   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.startup());

   const auto diagnostics = app.apis().get<forge::plugins::p2p::node::diagnostics_source>(
       {.id = {"forge.plugins.p2p.node.diagnostics_source"}, .major = 2, .min_revision = 0});
   const auto& system = diagnostics->snapshot().effective_limits.system;
   BOOST_TEST(system.max_connections == 1025U);
   BOOST_TEST(system.max_inbound_connections == 1025U);
   BOOST_TEST(system.max_outbound_connections == 1025U);

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_config_preserves_legacy_positional_prefix) {
   const auto value = forge::plugins::p2p::node::config{{}, {}, {"/ip4/127.0.0.1/tcp/4001"}};
   BOOST_REQUIRE_EQUAL(value.advertised_endpoints.size(), 1U);
   BOOST_TEST(value.advertised_endpoints.front() == "/ip4/127.0.0.1/tcp/4001");
   BOOST_TEST(static_cast<int>(value.bootstrap_requirement) ==
              static_cast<int>(forge::plugins::p2p::node::bootstrap_requirement::allow_disconnected));
}

BOOST_AUTO_TEST_CASE(p2p_diagnostics_plugin_config_is_described_from_public_schema) {
   auto plugin = forge::plugins::p2p::diagnostics::plugin{};
   BOOST_TEST(plugin.version() == "2.0.0");
   const auto descriptor = plugin.describe_config();
   BOOST_REQUIRE(descriptor.has_value());
   BOOST_TEST(descriptor->section == "plugins.p2p.diagnostics");

   const auto& max_peers = require_field(*descriptor, "max-peers");
   BOOST_TEST(max_peers.has_default);
   BOOST_TEST(std::get<std::uint64_t>(max_peers.default_value.storage) > 0U);

   const auto& max_sessions = require_field(*descriptor, "max-sessions");
   BOOST_TEST(max_sessions.has_default);
   BOOST_TEST(std::get<std::uint64_t>(max_sessions.default_value.storage) > 0U);

   const auto api_descriptor = forge::plugins::p2p::diagnostics::api::describe();
   BOOST_TEST(api_descriptor.id.value == "forge.plugins.p2p.diagnostics");
   BOOST_TEST(api_descriptor.version.major == 2U);
}

BOOST_AUTO_TEST_CASE(p2p_diagnostics_api_rejects_facade_calls_before_initialize) {
   auto runtime = forge::asio::runtime{};
   auto plugin = forge::plugins::p2p::diagnostics::plugin{};
   auto apis = forge::api::core::registry{};
   auto provider = forge::api::core::installer{apis};
   forge::asio::blocking::run(runtime, plugin.provide(provider));

   auto diagnostics = apis.get<forge::plugins::p2p::diagnostics::api>(
       {.id = {"forge.plugins.p2p.diagnostics"}, .major = 2, .min_revision = 0});

   BOOST_CHECK_THROW((void)diagnostics->snapshot(),
                     forge::plugins::p2p::diagnostics::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)diagnostics->network(),
                     forge::plugins::p2p::diagnostics::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)diagnostics->resources(),
                     forge::plugins::p2p::diagnostics::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)diagnostics->pubsub(), forge::plugins::p2p::diagnostics::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)diagnostics->peers(), forge::plugins::p2p::diagnostics::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)diagnostics->peer(test_peer(90)),
                     forge::plugins::p2p::diagnostics::exceptions::plugin_not_initialized);
}

BOOST_AUTO_TEST_CASE(p2p_diagnostics_plugin_reports_live_p2p_node_state) {
   const auto server_peer = test_peer(91);
   auto server_config = test_p2p_config(server_peer);
   server_config.set("plugins.p2p.node.listen", forge::config::core::value::array_type{
                                                    forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});

   auto log = plugin_log{};
   auto server = p2p_plugin_application{log};
   server.configure(server_config);
   forge::asio::blocking::run(server.runtime(), server.startup());

   auto server_p2p = server.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   const auto server_endpoint = server_p2p->local_endpoint();
   BOOST_REQUIRE(server_endpoint.has_value());

   const auto client_peer = test_peer(92);
   auto client_config = test_p2p_config(client_peer);
   client_config.set("plugins.p2p.node.bootstrap",
                     forge::config::core::value::array_type{forge::config::core::value{server_endpoint->to_string()}});

   auto client = diagnostics_application{};
   client.configure(client_config);
   forge::asio::blocking::run(client.runtime(), client.startup());

   auto client_p2p = client.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   auto diagnostics = client.apis().get<forge::plugins::p2p::diagnostics::api>(
       {.id = {"forge.plugins.p2p.diagnostics"}, .major = 2, .min_revision = 0});

   auto remote = forge::asio::blocking::run(
       client.runtime(),
       client_p2p->remote<node_test_api>(server_p2p->local_peer(),
                                         forge::net::p2p::protocol_id{.value = "/forge/api/node-test/1"}));
   const auto response = forge::asio::blocking::run(client.runtime(), remote->ping(10));
   BOOST_TEST(response == 11);

   const auto snapshot = diagnostics->snapshot();
   BOOST_TEST(snapshot.network.local_peer.to_string() == client_p2p->local_peer().to_string());
   BOOST_TEST(snapshot.black_holes.udp.enabled);
   BOOST_TEST(snapshot.black_holes.ipv6.enabled);
   BOOST_TEST(snapshot.black_holes.udp.peer_requests == 0U);
   BOOST_TEST(snapshot.black_holes.ipv6.peer_requests == 0U);
   const auto bounded_snapshot = diagnostics->snapshot(forge::net::p2p::diagnostics::options{.max_peers = 0});
   BOOST_TEST(bounded_snapshot.black_holes.udp.enabled == snapshot.black_holes.udp.enabled);
   BOOST_TEST(bounded_snapshot.black_holes.ipv6.enabled == snapshot.black_holes.ipv6.enabled);
   BOOST_TEST(snapshot.metrics.active_sessions >= 1U);
   BOOST_TEST(snapshot.resources.system.outbound_connections >= 1U);
   BOOST_REQUIRE(!snapshot.sessions.empty());
   BOOST_TEST(snapshot.sessions.front().remote_peer.to_string() == server_peer.to_string());
   BOOST_REQUIRE(!diagnostics->peers().empty());

   const auto server_record = diagnostics->peer(server_peer);
   BOOST_TEST(server_record.peer.to_string() == server_peer.to_string());
   BOOST_CHECK_THROW((void)diagnostics->peer(test_peer(93)), forge::plugins::p2p::diagnostics::exceptions::not_found);

   forge::asio::blocking::run(client.runtime(), client.shutdown());
   forge::asio::blocking::run(server.runtime(), server.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_maintains_bootstrap_session_after_peer_restart) {
   const auto server_peer = test_peer(112);
   auto server_config = test_p2p_config(server_peer);
   server_config.set("plugins.p2p.node.listen", forge::config::core::value::array_type{
                                                    forge::config::core::value{"/ip4/127.0.0.1/tcp/0"},
                                                });

   auto server = diagnostics_application{};
   server.configure(server_config);
   forge::asio::blocking::run(server.runtime(), server.startup());

   auto server_p2p = server.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   const auto server_endpoint = server_p2p->local_endpoint();
   BOOST_REQUIRE(server_endpoint.has_value());
   BOOST_TEST(server_endpoint->is_direct_tcp());
   BOOST_REQUIRE(server_endpoint->peer.has_value());
   const auto bootstrap_peer = *server_endpoint->peer;
   BOOST_TEST(bootstrap_peer.to_string() == server_peer.to_string());

   auto client_config = test_p2p_config(test_peer(113));
   client_config.set("plugins.p2p.node.bootstrap", forge::config::core::value::array_type{
                                                       forge::config::core::value{server_endpoint->to_string()},
                                                   });

   auto client = diagnostics_application{};
   client.configure(client_config);
   forge::asio::blocking::run(client.runtime(), client.startup());
   auto client_diagnostics = client.apis().get<forge::plugins::p2p::diagnostics::api>(
       {.id = {"forge.plugins.p2p.diagnostics"}, .major = 2, .min_revision = 0});

   const auto connected = forge::asio::blocking::run(
       client.runtime(),
       async_wait_for_condition(
           [&] {
              const auto snapshot = client_diagnostics->snapshot();
              return snapshot.metrics.active_sessions >= 1U && snapshot.metrics.sessions_opened >= 1U &&
                     std::ranges::any_of(snapshot.sessions, [](const auto& session) { return session.protected_peer; });
           },
           std::chrono::seconds{8}));
   const auto connected_snapshot = client_diagnostics->snapshot();
   BOOST_TEST_CONTEXT("opened=" << connected_snapshot.metrics.sessions_opened
                                << " closed=" << connected_snapshot.metrics.sessions_closed
                                << " active=" << connected_snapshot.metrics.active_sessions
                                << " protected=" << connected_snapshot.connections.protected_peers.size()) {
      BOOST_REQUIRE(connected);
   }

   auto replacement_endpoint = *server_endpoint;
   replacement_endpoint.peer.reset();
   forge::asio::blocking::run(server.runtime(), server.shutdown());

   const auto disconnected = forge::asio::blocking::run(
       client.runtime(),
       async_wait_for_condition([&] { return client_diagnostics->snapshot().metrics.active_sessions == 0U; },
                                std::chrono::seconds{5}));
   BOOST_REQUIRE(disconnected);

   auto replacement_config = test_p2p_config(server_peer);
   replacement_config.set("plugins.p2p.node.listen", forge::config::core::value::array_type{
                                                         forge::config::core::value{replacement_endpoint.to_string()},
                                                     });

   auto replacement = diagnostics_application{};
   replacement.configure(replacement_config);
   forge::asio::blocking::run(replacement.runtime(), replacement.startup());
   auto replacement_diagnostics = replacement.apis().get<forge::plugins::p2p::diagnostics::api>(
       {.id = {"forge.plugins.p2p.diagnostics"}, .major = 2, .min_revision = 0});

   const auto reconnected = forge::asio::blocking::run(
       client.runtime(),
       async_wait_for_condition(
           [&] {
              const auto snapshot = client_diagnostics->snapshot();
              return snapshot.metrics.active_sessions == 1U && snapshot.metrics.sessions_opened >= 2U &&
                     std::ranges::any_of(snapshot.sessions, [](const auto& session) { return session.protected_peer; });
           },
           std::chrono::seconds{12}));
   const auto reconnected_snapshot = client_diagnostics->snapshot();
   BOOST_TEST_CONTEXT("opened=" << reconnected_snapshot.metrics.sessions_opened
                                << " closed=" << reconnected_snapshot.metrics.sessions_closed
                                << " active=" << reconnected_snapshot.metrics.active_sessions
                                << " protected=" << reconnected_snapshot.connections.protected_peers.size()) {
      BOOST_TEST(reconnected);
   }

   forge::asio::blocking::run(client.runtime(), client.shutdown());
   const auto gracefully_closed = forge::asio::blocking::run(
       replacement.runtime(),
       async_wait_for_condition([&] { return replacement_diagnostics->snapshot().metrics.active_sessions == 0U; },
                                std::chrono::seconds{5}));
   BOOST_TEST(gracefully_closed);
   forge::asio::blocking::run(replacement.runtime(), replacement.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_preserves_stop_requested_before_node_startup) {
   auto config = test_p2p_config(test_peer(114));
   config.set("plugins.p2p.node.bootstrap", forge::config::core::value::array_type{
                                                forge::config::core::value{
                                                    "/ip4/127.0.0.1/tcp/1/p2p/" + test_peer(115).to_string(),
                                                },
                                            });

   auto app = p2p_only_application{forge::app::application_shell_options{
       .name = "p2p-stop-before-maintenance",
       .runtime = {.worker_threads = 2, .thread_name = "p2p-stop-before"},
   }};
   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.initialize());
   auto p2p = app.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});

   auto stop_thread = std::thread{[&] { app.request_stop(); }};
   stop_thread.join();

   const auto started = std::chrono::steady_clock::now();
   forge::asio::blocking::run(app.runtime(), app.startup());
   const auto elapsed = std::chrono::steady_clock::now() - started;

   BOOST_TEST(elapsed < std::chrono::seconds{2});
   BOOST_CHECK_THROW((void)p2p->local_endpoint(), forge::plugins::p2p::node::exceptions::plugin_not_initialized);
   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_stop_cannot_race_route_installation) {
   auto app = p2p_only_application{forge::app::application_shell_options{
       .name = "p2p-stop-during-route-install",
       .runtime = {.worker_threads = 2, .thread_name = "p2p-route-stop"},
   }};
   app.configure(test_p2p_config(test_peer(226)));
   forge::asio::blocking::run(app.runtime(), app.initialize());
   auto p2p = app.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});

   auto gate = std::make_shared<route_install_gate>();
   p2p->publish_protocol(forge::net::p2p::protocol_id{.value = "/forge/test/startup-stop-race/1"},
                         blocking_route_handler{gate});
   gate->copies.store(0, std::memory_order_release);
   gate->armed.store(true, std::memory_order_release);

   auto startup_failure = std::exception_ptr{};
   auto startup = std::thread{[&] {
      try {
         forge::asio::blocking::run(app.runtime(), app.startup());
      } catch (...) {
         startup_failure = std::current_exception();
      }
   }};

   auto blocked = false;
   {
      auto lock = std::unique_lock{gate->mutex};
      blocked = gate->changed.wait_for(lock, std::chrono::seconds{5}, [&] { return gate->blocked; });
   }
   app.request_stop();
   {
      auto lock = std::scoped_lock{gate->mutex};
      gate->released = true;
   }
   gate->changed.notify_all();
   startup.join();

   BOOST_REQUIRE(blocked);
   if (startup_failure) {
      std::rethrow_exception(startup_failure);
   }
   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_closes_route_publication_before_startup_suspends) {
   auto config = test_p2p_config(test_peer(224));
   config.set("plugins.p2p.node.bootstrap", forge::config::core::value::array_type{
                                                forge::config::core::value{
                                                    "/ip4/127.0.0.1/tcp/1/p2p/" + test_peer(225).to_string(),
                                                },
                                            });
   config.set("plugins.p2p.node.bootstrap-requirement", std::string{"require-connection"});
   config.set("plugins.p2p.node.bootstrap-startup-budget-ms", std::uint64_t{5'000});
   config.set("plugins.p2p.node.bootstrap-connect-timeout-ms", std::uint64_t{1'000});

   auto app = p2p_only_application{forge::app::application_shell_options{
       .name = "p2p-route-startup-boundary",
       .runtime = {.worker_threads = 2, .thread_name = "p2p-route-boundary"},
   }};
   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.initialize());
   auto p2p = app.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});

   auto startup_failure = std::exception_ptr{};
   auto startup = std::thread{[&] {
      try {
         forge::asio::blocking::run(app.runtime(), app.startup());
      } catch (...) {
         startup_failure = std::current_exception();
      }
   }};

   auto node_created = false;
   for (auto attempt = 0; attempt < 200 && !node_created; ++attempt) {
      try {
         static_cast<void>(p2p->local_peer());
         node_created = true;
      } catch (const forge::plugins::p2p::node::exceptions::plugin_not_initialized&) {
         std::this_thread::sleep_for(std::chrono::milliseconds{10});
      }
   }
   BOOST_REQUIRE(node_created);

   BOOST_CHECK_THROW(
       p2p->publish_protocol(
           forge::net::p2p::protocol_id{.value = "/forge/test/late-route/1"},
           [](forge::net::p2p::node::incoming_protocol_stream) -> boost::asio::awaitable<void> { co_return; }),
       forge::plugins::p2p::node::exceptions::route_conflict);

   app.request_stop();
   startup.join();
   if (startup_failure) {
      std::rethrow_exception(startup_failure);
   }
   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_cancels_bootstrap_sleep_from_an_external_thread) {
   const auto server_peer = test_peer(116);
   auto server_config = test_p2p_config(server_peer);
   server_config.set("plugins.p2p.node.listen", forge::config::core::value::array_type{
                                                    forge::config::core::value{"/ip4/127.0.0.1/tcp/0"},
                                                });

   auto server = diagnostics_application{};
   server.configure(server_config);
   forge::asio::blocking::run(server.runtime(), server.startup());

   auto server_p2p = server.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   const auto server_endpoint = server_p2p->local_endpoint();
   BOOST_REQUIRE(server_endpoint.has_value());

   auto client_config = test_p2p_config(test_peer(117));
   client_config.set("plugins.p2p.node.bootstrap", forge::config::core::value::array_type{
                                                       forge::config::core::value{server_endpoint->to_string()},
                                                   });

   auto client = diagnostics_application{forge::app::application_shell_options{
       .name = "p2p-cross-thread-stop",
       .runtime = {.worker_threads = 2, .thread_name = "p2p-cross-stop"},
   }};
   client.configure(client_config);
   forge::asio::blocking::run(client.runtime(), client.startup());

   auto server_diagnostics = server.apis().get<forge::plugins::p2p::diagnostics::api>(
       {.id = {"forge.plugins.p2p.diagnostics"}, .major = 2, .min_revision = 0});
   auto client_diagnostics = client.apis().get<forge::plugins::p2p::diagnostics::api>(
       {.id = {"forge.plugins.p2p.diagnostics"}, .major = 2, .min_revision = 0});
   BOOST_REQUIRE(forge::asio::blocking::run(
       server.runtime(),
       async_wait_for_condition([&] { return server_diagnostics->snapshot().metrics.active_sessions == 1U; },
                                std::chrono::seconds{5})));

   std::this_thread::sleep_for(std::chrono::milliseconds{100});
   auto stop_thread = std::thread{[&] { client.request_stop(); }};
   stop_thread.join();

   const auto requested_stop = client_diagnostics->snapshot();
   BOOST_TEST(static_cast<int>(requested_stop.lifecycle.phase) ==
              static_cast<int>(forge::net::p2p::lifecycle_phase::stopping));
   BOOST_TEST(requested_stop.metrics.active_sessions >= 1U);

   const auto started = std::chrono::steady_clock::now();
   forge::asio::blocking::run(client.runtime(), client.shutdown());
   const auto elapsed = std::chrono::steady_clock::now() - started;
   BOOST_TEST(elapsed < std::chrono::milliseconds{750});

   const auto disconnected_after_shutdown = forge::asio::blocking::run(
       server.runtime(),
       async_wait_for_condition([&] { return server_diagnostics->snapshot().metrics.active_sessions == 0U; },
                                std::chrono::seconds{5}));
   BOOST_TEST(disconnected_after_shutdown);

   forge::asio::blocking::run(server.runtime(), server.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_cancels_active_bootstrap_dial) {
   namespace asio = boost::asio;
   using asio_tcp = asio::ip::tcp;

   auto app = p2p_only_application{forge::app::application_shell_options{
       .name = "p2p-active-bootstrap-stop",
       .runtime = {.worker_threads = 2, .thread_name = "p2p-active-stop"},
   }};
   auto acceptor = std::make_shared<asio_tcp::acceptor>(app.runtime().context());
   acceptor->open(asio_tcp::v4());
   acceptor->bind({asio::ip::make_address("127.0.0.1"), 0});
   const auto port = acceptor->local_endpoint().port();

   auto config = test_p2p_config(test_peer(120));
   config.set("plugins.p2p.node.bootstrap",
              forge::config::core::value::array_type{
                  forge::config::core::value{
                      "/ip4/127.0.0.1/tcp/" + std::to_string(port) + "/p2p/" + test_peer(121).to_string(),
                  },
              });
   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.startup());

   acceptor->listen();
   auto socket = std::make_shared<asio_tcp::socket>(app.runtime().context());
   auto hold = std::make_shared<asio::steady_timer>(app.runtime().context());
   auto accepted = std::make_shared<std::promise<void>>();
   auto accepted_future = accepted->get_future();
   asio::co_spawn(
       app.runtime().context(),
       [acceptor, socket, hold, accepted]() -> asio::awaitable<void> {
          auto error = boost::system::error_code{};
          co_await acceptor->async_accept(*socket, asio::redirect_error(asio::use_awaitable, error));
          if (!error) {
             accepted->set_value();
             hold->expires_after(std::chrono::seconds{5});
             co_await hold->async_wait(asio::redirect_error(asio::use_awaitable, error));
          }
       },
       asio::detached);

   BOOST_REQUIRE(accepted_future.wait_for(std::chrono::seconds{3}) == std::future_status::ready);
   const auto started = std::chrono::steady_clock::now();
   forge::asio::blocking::run(app.runtime(), app.shutdown());
   const auto elapsed = std::chrono::steady_clock::now() - started;

   BOOST_TEST(elapsed < std::chrono::milliseconds{750});
   auto ignored = boost::system::error_code{};
   (void)hold->cancel();
   socket->close(ignored);
   acceptor->close(ignored);
}

BOOST_AUTO_TEST_CASE(p2p_node_plugin_rolls_back_strict_bootstrap_failure) {
   auto config = test_p2p_config(test_peer(118));
   config.set("plugins.p2p.node.bootstrap", forge::config::core::value::array_type{
                                                forge::config::core::value{
                                                    "/ip4/127.0.0.1/tcp/1/p2p/" + test_peer(119).to_string(),
                                                },
                                            });
   config.set("plugins.p2p.node.bootstrap-requirement", std::string{"require-connection"});
   config.set("plugins.p2p.node.bootstrap-startup-budget-ms", std::uint64_t{100});
   config.set("plugins.p2p.node.bootstrap-connect-timeout-ms", std::uint64_t{50});

   auto app = p2p_only_application{};
   app.configure(config);
   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), app.startup()), forge::net::p2p::exceptions::timeout);

   const auto diagnostics = app.diagnostics().snapshot(app.events());
   const auto failed_plugin = std::ranges::find_if(
       diagnostics.plugins, [](const auto& plugin) { return plugin.id == "forge.plugins.p2p.node"; });
   BOOST_REQUIRE(failed_plugin != diagnostics.plugins.end());
   BOOST_TEST(static_cast<int>(failed_plugin->state) == static_cast<int>(forge::app::lifecycle_state::failed));
   BOOST_TEST(!failed_plugin->last_error.empty());
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_plugin_config_is_described_from_public_schema) {
   auto plugin = forge::plugins::p2p::pubsub::plugin{};
   const auto descriptor = plugin.describe_config();
   BOOST_REQUIRE(descriptor.has_value());
   BOOST_TEST(descriptor->section == "plugins.p2p.pubsub");

   BOOST_TEST(require_field(*descriptor, "max-topics").has_default);
   BOOST_TEST(require_field(*descriptor, "max-handlers-per-topic").has_default);
   BOOST_TEST(require_field(*descriptor, "max-active-handlers").has_default);
   BOOST_TEST(require_field(*descriptor, "max-message-size").has_default);
   BOOST_TEST(require_field(*descriptor, "handler-deadline-ms").has_default);
   BOOST_TEST(require_field(*descriptor, "allowed-topics").has_default);
   BOOST_TEST(require_field(*descriptor, "denied-topics").has_default);
   BOOST_TEST(require_field(*descriptor, "sign-publishes").has_default);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_api_rejects_facade_calls_before_initialize) {
   auto runtime = forge::asio::runtime{};
   auto plugin = forge::plugins::p2p::pubsub::plugin{};
   auto apis = forge::api::core::registry{};
   auto provider = forge::api::core::installer{apis};
   forge::asio::blocking::run(runtime, plugin.provide(provider));

   auto pubsub =
       apis.get<forge::plugins::p2p::pubsub::api>({.id = {"forge.plugins.p2p.pubsub"}, .major = 1, .min_revision = 0});

   BOOST_CHECK_THROW((void)pubsub->snapshot(), forge::plugins::p2p::pubsub::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)pubsub->subscriptions(), forge::plugins::p2p::pubsub::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW(
       forge::asio::blocking::run(
           runtime, pubsub->publish(forge::net::p2p::pubsub::topic{.value = "forge.before-init"}, {1, 2, 3})),
       forge::plugins::p2p::pubsub::exceptions::plugin_not_initialized);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_plugin_rejects_invalid_typed_config_before_startup) {
   {
      auto config = test_p2p_config();
      config.set("plugins.p2p.pubsub.max-topics", std::uint64_t{0});
      auto app = pubsub_application{};
      BOOST_CHECK_THROW(app.configure(config), forge::plugins::p2p::pubsub::exceptions::invalid_config);
   }
   {
      auto config = test_p2p_config();
      config.set("plugins.p2p.pubsub.handler-deadline-ms", std::uint64_t{0});
      auto app = pubsub_application{};
      BOOST_CHECK_THROW(app.configure(config), forge::plugins::p2p::pubsub::exceptions::invalid_config);
   }
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_plugin_requests_core_pubsub_capability_before_startup) {
   auto config = test_p2p_config(test_peer(94));
   config.set("plugins.p2p.node.listen",
              forge::config::core::value::array_type{forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});
   config.set("plugins.p2p.pubsub.sign-publishes", false);

   auto app = pubsub_application{};
   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.startup());

   auto pubsub = app.apis().get<forge::plugins::p2p::pubsub::api>(
       {.id = {"forge.plugins.p2p.pubsub"}, .major = 1, .min_revision = 0});
   auto subscription = forge::asio::blocking::run(
       app.runtime(), pubsub->subscribe(forge::net::p2p::pubsub::topic{.value = "forge.local"},
                                        [](forge::plugins::p2p::pubsub::message)
                                            -> boost::asio::awaitable<forge::net::p2p::pubsub::validation_result> {
                                           co_return forge::net::p2p::pubsub::validation_result::accept;
                                        }));

   BOOST_TEST(subscription.id != 0U);
   BOOST_TEST(pubsub->snapshot().core.topics == 1U);
   BOOST_TEST(pubsub->subscriptions().size() == 1U);

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_plugin_omits_unverified_author_from_unsigned_messages) {
   auto source_state = std::make_shared<fake_pubsub_source_state>();
   auto app = fake_pubsub_application{source_state};
   forge::asio::blocking::run(app.runtime(), app.startup());

   auto pubsub = app.apis().get<forge::plugins::p2p::pubsub::api>(
       {.id = {"forge.plugins.p2p.pubsub"}, .major = 1, .min_revision = 0});
   const auto published = forge::asio::blocking::run(
       app.runtime(), pubsub->publish(forge::net::p2p::pubsub::topic{.value = "forge.fake.unsigned"}, {1, 2, 3},
                                      forge::plugins::p2p::pubsub::publish_options{.sign = false}));

   BOOST_TEST(published.source.to_string() == "fake-pubsub-peer");
   BOOST_TEST(!published.author.has_value());
   BOOST_TEST(published.data == (std::vector<std::uint8_t>{1, 2, 3}), boost::test_tools::per_element());

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_plugin_serializes_first_join_per_topic) {
   auto source_state = std::make_shared<fake_pubsub_source_state>();
   auto app = fake_pubsub_application{source_state};
   forge::asio::blocking::run(app.runtime(), app.startup());

   auto pubsub = app.apis().get<forge::plugins::p2p::pubsub::api>(
       {.id = {"forge.plugins.p2p.pubsub"}, .major = 1, .min_revision = 0});
   const auto topic = forge::net::p2p::pubsub::topic{.value = "forge.fake.pending"};
   auto first = std::make_shared<subscribe_task_result>();
   auto second = std::make_shared<subscribe_task_result>();
   auto handler =
       [](forge::plugins::p2p::pubsub::message) -> boost::asio::awaitable<forge::net::p2p::pubsub::validation_result> {
      co_return forge::net::p2p::pubsub::validation_result::accept;
   };

   forge::asio::blocking::run(app.runtime(), [&]() -> boost::asio::awaitable<void> {
      auto executor = co_await boost::asio::this_coro::executor;
      boost::asio::co_spawn(
          executor,
          [pubsub, topic, first, handler]() mutable -> boost::asio::awaitable<void> {
             try {
                first->complete(co_await pubsub->subscribe(topic, handler));
             } catch (...) {
                first->fail(std::current_exception());
             }
          },
          boost::asio::detached);
      boost::asio::co_spawn(
          executor,
          [pubsub, topic, second, handler]() mutable -> boost::asio::awaitable<void> {
             try {
                second->complete(co_await pubsub->subscribe(topic, handler));
             } catch (...) {
                second->fail(std::current_exception());
             }
          },
          boost::asio::detached);

      BOOST_REQUIRE(
          co_await async_wait_for_condition([&] { return source_state->joins() == 1U; }, std::chrono::seconds{1}));
      auto settle_timer = boost::asio::steady_timer{executor, std::chrono::milliseconds{50}};
      co_await settle_timer.async_wait(boost::asio::use_awaitable);
      BOOST_TEST(!first->finished());
      BOOST_TEST(!second->finished());
      source_state->release(false);
      BOOST_REQUIRE(co_await async_wait_for_condition([&] { return first->finished() && second->finished(); },
                                                      std::chrono::seconds{1}));
   }());

   BOOST_TEST(!first->failed());
   BOOST_TEST(!second->failed());
   BOOST_TEST(source_state->joins() == 1U);
   BOOST_TEST(pubsub->snapshot().topics == 1U);
   BOOST_TEST(pubsub->subscriptions().size() == 2U);

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_plugin_failed_first_join_clears_pending_topic) {
   auto source_state = std::make_shared<fake_pubsub_source_state>();
   auto app = fake_pubsub_application{source_state};
   forge::asio::blocking::run(app.runtime(), app.startup());

   auto pubsub = app.apis().get<forge::plugins::p2p::pubsub::api>(
       {.id = {"forge.plugins.p2p.pubsub"}, .major = 1, .min_revision = 0});
   const auto topic = forge::net::p2p::pubsub::topic{.value = "forge.fake.failed"};
   auto first = std::make_shared<subscribe_task_result>();
   auto second = std::make_shared<subscribe_task_result>();
   auto handler =
       [](forge::plugins::p2p::pubsub::message) -> boost::asio::awaitable<forge::net::p2p::pubsub::validation_result> {
      co_return forge::net::p2p::pubsub::validation_result::accept;
   };

   forge::asio::blocking::run(app.runtime(), [&]() -> boost::asio::awaitable<void> {
      auto executor = co_await boost::asio::this_coro::executor;
      boost::asio::co_spawn(
          executor,
          [pubsub, topic, first, handler]() mutable -> boost::asio::awaitable<void> {
             try {
                first->complete(co_await pubsub->subscribe(topic, handler));
             } catch (...) {
                first->fail(std::current_exception());
             }
          },
          boost::asio::detached);
      boost::asio::co_spawn(
          executor,
          [pubsub, topic, second, handler]() mutable -> boost::asio::awaitable<void> {
             try {
                second->complete(co_await pubsub->subscribe(topic, handler));
             } catch (...) {
                second->fail(std::current_exception());
             }
          },
          boost::asio::detached);

      BOOST_REQUIRE(
          co_await async_wait_for_condition([&] { return source_state->joins() == 1U; }, std::chrono::seconds{1}));
      source_state->release(true);
      BOOST_REQUIRE(co_await async_wait_for_condition([&] { return first->finished() && second->finished(); },
                                                      std::chrono::seconds{1}));
   }());

   BOOST_TEST(first->failed());
   BOOST_TEST(second->failed());
   BOOST_TEST(pubsub->snapshot().topics == 0U);
   BOOST_TEST(pubsub->subscriptions().empty());

   source_state->release(false);
   const auto retry = forge::asio::blocking::run(app.runtime(), pubsub->subscribe(topic, handler));
   BOOST_TEST(retry.subject.value == topic.value);
   BOOST_TEST(source_state->joins() == 2U);

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_plugin_publishes_and_subscribes_raw_and_typed_messages) {
   const auto subscriber_peer = test_peer(95);
   auto subscriber_config = test_p2p_config(subscriber_peer);
   subscriber_config.set("plugins.p2p.node.listen", forge::config::core::value::array_type{
                                                        forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});
   subscriber_config.set("plugins.p2p.pubsub.sign-publishes", false);

   auto subscriber = pubsub_application{};
   subscriber.configure(subscriber_config);
   forge::asio::blocking::run(subscriber.runtime(), subscriber.startup());

   auto subscriber_p2p = subscriber.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   const auto subscriber_endpoint = subscriber_p2p->local_endpoint();
   BOOST_REQUIRE(subscriber_endpoint.has_value());

   const auto publisher_peer = test_peer(96);
   auto publisher_config = test_p2p_config(publisher_peer);
   publisher_config.set("plugins.p2p.node.bootstrap", forge::config::core::value::array_type{forge::config::core::value{
                                                          subscriber_endpoint->to_string()}});
   publisher_config.set("plugins.p2p.pubsub.sign-publishes", false);

   auto publisher = pubsub_application{};
   publisher.configure(publisher_config);
   forge::asio::blocking::run(publisher.runtime(), publisher.startup());

   auto received = std::make_shared<received_pubsub_messages>();
   auto subscriber_pubsub = subscriber.apis().get<forge::plugins::p2p::pubsub::api>(
       {.id = {"forge.plugins.p2p.pubsub"}, .major = 1, .min_revision = 0});
   auto publisher_pubsub = publisher.apis().get<forge::plugins::p2p::pubsub::api>(
       {.id = {"forge.plugins.p2p.pubsub"}, .major = 1, .min_revision = 0});

   const auto raw_topic = forge::net::p2p::pubsub::topic{.value = "forge.plugins.raw"};
   const auto typed_topic = forge::net::p2p::pubsub::topic{.value = "forge.plugins.typed"};
   auto first = forge::asio::blocking::run(
       subscriber.runtime(),
       subscriber_pubsub->subscribe(raw_topic,
                                    [received](forge::plugins::p2p::pubsub::message message) mutable
                                        -> boost::asio::awaitable<forge::net::p2p::pubsub::validation_result> {
                                       received->push(std::move(message),
                                                      forge::net::p2p::pubsub::validation_result::accept);
                                       co_return forge::net::p2p::pubsub::validation_result::accept;
                                    }));
   auto second = forge::asio::blocking::run(
       subscriber.runtime(),
       subscriber_pubsub->subscribe(raw_topic,
                                    [received](forge::plugins::p2p::pubsub::message message) mutable
                                        -> boost::asio::awaitable<forge::net::p2p::pubsub::validation_result> {
                                       received->push(std::move(message),
                                                      forge::net::p2p::pubsub::validation_result::accept);
                                       co_return forge::net::p2p::pubsub::validation_result::accept;
                                    }));
   (void)forge::asio::blocking::run(
       subscriber.runtime(), subscriber_pubsub->subscribe<pubsub_payload>(
                                 typed_topic,
                                 [received](forge::plugins::p2p::pubsub::typed_message<pubsub_payload> message) mutable
                                     -> boost::asio::awaitable<forge::net::p2p::pubsub::validation_result> {
                                    received->push(std::move(message));
                                    co_return forge::net::p2p::pubsub::validation_result::accept;
                                 }));

   BOOST_TEST(first.subject.value == raw_topic.value);
   BOOST_TEST(second.id != first.id);
   BOOST_REQUIRE_MESSAGE(wait_for_pubsub_peer(*publisher_pubsub.shared(), std::chrono::seconds{5}),
                         "publisher did not learn a remote PubSub topic subscription");

   (void)forge::asio::blocking::run(publisher.runtime(),
                                    publisher_pubsub->publish(raw_topic, std::vector<std::uint8_t>{1, 2, 3, 4}));
   (void)forge::asio::blocking::run(
       publisher.runtime(), publisher_pubsub->publish(typed_topic, pubsub_payload{.text = "hello", .value = 7}));

   if (!wait_for_count(*received, 2, 1)) {
      const auto publisher_snapshot = publisher_pubsub->snapshot();
      const auto subscriber_snapshot = subscriber_pubsub->snapshot();
      BOOST_FAIL("pubsub plugin delivery did not finish; raw="
                 << received->raw_size() << " typed=" << received->typed_size()
                 << " publisher_core_peers=" << publisher_snapshot.core.peers
                 << " publisher_published=" << publisher_snapshot.core.messages_published
                 << " subscriber_core_received=" << subscriber_snapshot.core.messages_received
                 << " subscriber_core_delivered=" << subscriber_snapshot.core.messages_delivered
                 << " subscriber_core_invalid=" << subscriber_snapshot.core.invalid_messages
                 << " subscriber_plugin_delivered=" << subscriber_snapshot.messages_delivered
                 << " subscriber_plugin_failures=" << subscriber_snapshot.handler_failures
                 << " subscriber_plugin_dropped=" << subscriber_snapshot.messages_dropped);
   }
   {
      auto lock = std::scoped_lock{received->mutex};
      BOOST_TEST(received->raw.size() == 2U);
      BOOST_TEST(forge::net::p2p::valid_peer_id(received->raw.front().source));
      BOOST_TEST(received->raw.front().data == (std::vector<std::uint8_t>{1, 2, 3, 4}),
                 boost::test_tools::per_element());
      BOOST_TEST(received->typed.front().source.to_string() == received->raw.front().source.to_string());
      BOOST_TEST(received->typed.front().value.text == "hello");
      BOOST_TEST(received->typed.front().value.value == 7U);
   }

   forge::asio::blocking::run(subscriber.runtime(), subscriber_pubsub->unsubscribe(first));
   BOOST_TEST(subscriber_pubsub->subscriptions().size() == 2U);
   forge::asio::blocking::run(subscriber.runtime(), subscriber_pubsub->unsubscribe(second));
   BOOST_TEST(subscriber_pubsub->subscriptions().size() == 1U);

   forge::asio::blocking::run(publisher.runtime(), publisher.shutdown());
   forge::asio::blocking::run(subscriber.runtime(), subscriber.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_plugin_aggregates_handler_results_and_deadlines) {
   const auto subscriber_peer = test_peer(98);
   auto subscriber_config = test_p2p_config(subscriber_peer);
   subscriber_config.set("plugins.p2p.node.listen", forge::config::core::value::array_type{
                                                        forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});
   subscriber_config.set("plugins.p2p.pubsub.sign-publishes", false);

   auto subscriber = pubsub_application{};
   subscriber.configure(subscriber_config);
   forge::asio::blocking::run(subscriber.runtime(), subscriber.startup());

   auto subscriber_p2p = subscriber.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   const auto subscriber_endpoint = subscriber_p2p->local_endpoint();
   BOOST_REQUIRE(subscriber_endpoint.has_value());

   auto publisher_config = test_p2p_config(test_peer(99));
   publisher_config.set("plugins.p2p.node.bootstrap", forge::config::core::value::array_type{forge::config::core::value{
                                                          subscriber_endpoint->to_string()}});
   publisher_config.set("plugins.p2p.pubsub.sign-publishes", false);

   auto publisher = pubsub_application{};
   publisher.configure(publisher_config);
   forge::asio::blocking::run(publisher.runtime(), publisher.startup());

   auto received = std::make_shared<received_pubsub_messages>();
   auto subscriber_pubsub = subscriber.apis().get<forge::plugins::p2p::pubsub::api>(
       {.id = {"forge.plugins.p2p.pubsub"}, .major = 1, .min_revision = 0});
   auto publisher_pubsub = publisher.apis().get<forge::plugins::p2p::pubsub::api>(
       {.id = {"forge.plugins.p2p.pubsub"}, .major = 1, .min_revision = 0});

   const auto aggregate_topic = forge::net::p2p::pubsub::topic{.value = "forge.plugins.aggregate"};
   const auto timeout_topic = forge::net::p2p::pubsub::topic{.value = "forge.plugins.timeout"};
   const auto mixed_retry_topic = forge::net::p2p::pubsub::topic{.value = "forge.plugins.mixed-retry"};
   (void)forge::asio::blocking::run(
       subscriber.runtime(),
       subscriber_pubsub->subscribe(aggregate_topic,
                                    [received](forge::plugins::p2p::pubsub::message message) mutable
                                        -> boost::asio::awaitable<forge::net::p2p::pubsub::validation_result> {
                                       received->push(std::move(message),
                                                      forge::net::p2p::pubsub::validation_result::ignore);
                                       co_return forge::net::p2p::pubsub::validation_result::ignore;
                                    }));
   (void)forge::asio::blocking::run(
       subscriber.runtime(),
       subscriber_pubsub->subscribe(aggregate_topic,
                                    [](forge::plugins::p2p::pubsub::message)
                                        -> boost::asio::awaitable<forge::net::p2p::pubsub::validation_result> {
                                       throw forge::plugins::p2p::pubsub::exceptions::handler_limit{
                                           "test handler failure"};
                                    }));
   (void)forge::asio::blocking::run(
       subscriber.runtime(),
       subscriber_pubsub->subscribe(aggregate_topic,
                                    [received](forge::plugins::p2p::pubsub::message message) mutable
                                        -> boost::asio::awaitable<forge::net::p2p::pubsub::validation_result> {
                                       received->push(std::move(message),
                                                      forge::net::p2p::pubsub::validation_result::accept);
                                       co_return forge::net::p2p::pubsub::validation_result::accept;
                                    }));
   (void)forge::asio::blocking::run(
       subscriber.runtime(),
       subscriber_pubsub->subscribe(aggregate_topic,
                                    [received](forge::plugins::p2p::pubsub::message message) mutable
                                        -> boost::asio::awaitable<forge::net::p2p::pubsub::validation_result> {
                                       received->push(std::move(message),
                                                      forge::net::p2p::pubsub::validation_result::reject);
                                       co_return forge::net::p2p::pubsub::validation_result::reject;
                                    }));
   auto timeout_attempts = std::make_shared<std::atomic_uint64_t>(0);
   (void)forge::asio::blocking::run(
       subscriber.runtime(),
       subscriber_pubsub->subscribe(
           timeout_topic,
           [timeout_attempts](forge::plugins::p2p::pubsub::message)
               -> boost::asio::awaitable<forge::net::p2p::pubsub::validation_result> {
              if (timeout_attempts->fetch_add(1, std::memory_order_relaxed) == 0) {
                 auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
                 timer.expires_after(std::chrono::milliseconds{100});
                 co_await timer.async_wait(boost::asio::use_awaitable);
              }
              co_return forge::net::p2p::pubsub::validation_result::accept;
           },
           forge::plugins::p2p::pubsub::subscribe_options{.handler_deadline = std::chrono::milliseconds{10}}));
   auto mixed_retry_attempts = std::make_shared<std::atomic_uint64_t>(0);
   (void)forge::asio::blocking::run(
       subscriber.runtime(),
       subscriber_pubsub->subscribe(mixed_retry_topic,
                                    [mixed_retry_attempts](forge::plugins::p2p::pubsub::message)
                                        -> boost::asio::awaitable<forge::net::p2p::pubsub::validation_result> {
                                       if (mixed_retry_attempts->fetch_add(1, std::memory_order_relaxed) == 0) {
                                          co_return forge::net::p2p::pubsub::validation_result::retry;
                                       }
                                       co_return forge::net::p2p::pubsub::validation_result::accept;
                                    }));
   (void)forge::asio::blocking::run(
       subscriber.runtime(),
       subscriber_pubsub->subscribe(mixed_retry_topic,
                                    [](forge::plugins::p2p::pubsub::message)
                                        -> boost::asio::awaitable<forge::net::p2p::pubsub::validation_result> {
                                       co_return forge::net::p2p::pubsub::validation_result::accept;
                                    }));

   BOOST_REQUIRE_MESSAGE(wait_for_pubsub_peer(*publisher_pubsub.shared(), std::chrono::seconds{5}),
                         "publisher did not learn a remote PubSub topic subscription");

   (void)forge::asio::blocking::run(publisher.runtime(),
                                    publisher_pubsub->publish(aggregate_topic, std::vector<std::uint8_t>{9}));
   BOOST_REQUIRE_MESSAGE(wait_for_pubsub_snapshot(
                             *subscriber_pubsub.shared(),
                             [](const forge::plugins::p2p::pubsub::snapshot& snapshot) {
                                return snapshot.messages_rejected >= 1 && snapshot.handler_failures >= 1;
                             },
                             std::chrono::seconds{5}),
                         "PubSub handler aggregation did not finish");
   {
      auto lock = std::scoped_lock{received->mutex};
      BOOST_TEST(received->ignored == 1U);
      BOOST_TEST(received->accepted == 1U);
      BOOST_TEST(received->rejected == 1U);
   }

   (void)forge::asio::blocking::run(publisher.runtime(),
                                    publisher_pubsub->publish(timeout_topic, std::vector<std::uint8_t>{10}));
   BOOST_REQUIRE_MESSAGE(wait_for_pubsub_snapshot(
                             *subscriber_pubsub.shared(),
                             [](const forge::plugins::p2p::pubsub::snapshot& snapshot) {
                                return snapshot.messages_retried >= 1 && snapshot.messages_accepted >= 1 &&
                                       snapshot.handler_failures >= 2;
                             },
                             std::chrono::seconds{5}),
                         "PubSub handler timeout was not redelivered successfully");
   BOOST_TEST(timeout_attempts->load(std::memory_order_relaxed) == 2U);
   BOOST_TEST(subscriber_pubsub->snapshot().active_handlers == 0U);

   (void)forge::asio::blocking::run(publisher.runtime(),
                                    publisher_pubsub->publish(mixed_retry_topic, std::vector<std::uint8_t>{11}));
   BOOST_REQUIRE_MESSAGE(wait_for_pubsub_snapshot(
                             *subscriber_pubsub.shared(),
                             [](const forge::plugins::p2p::pubsub::snapshot& snapshot) {
                                return snapshot.messages_retried >= 2 && snapshot.messages_accepted >= 2;
                             },
                             std::chrono::seconds{5}),
                         "PubSub accept masked a transient sibling handler result");
   BOOST_TEST(mixed_retry_attempts->load(std::memory_order_relaxed) == 2U);

   forge::asio::blocking::run(publisher.runtime(), publisher.shutdown());
   forge::asio::blocking::run(subscriber.runtime(), subscriber.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_plugin_enforces_topic_policy_and_handler_bounds) {
   auto config = test_p2p_config(test_peer(97));
   config.set("plugins.p2p.pubsub.sign-publishes", false);
   config.set("plugins.p2p.pubsub.max-handlers-per-topic", std::uint64_t{1});
   config.set("plugins.p2p.pubsub.max-message-size", std::uint64_t{4});
   config.set("plugins.p2p.pubsub.allowed-topics",
              forge::config::core::value::array_type{forge::config::core::value{"forge.allowed"}});
   config.set("plugins.p2p.pubsub.denied-topics",
              forge::config::core::value::array_type{forge::config::core::value{"forge.denied"}});

   auto app = pubsub_application{};
   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.startup());

   auto pubsub = app.apis().get<forge::plugins::p2p::pubsub::api>(
       {.id = {"forge.plugins.p2p.pubsub"}, .major = 1, .min_revision = 0});
   auto handler =
       [](forge::plugins::p2p::pubsub::message) -> boost::asio::awaitable<forge::net::p2p::pubsub::validation_result> {
      co_return forge::net::p2p::pubsub::validation_result::ignore;
   };

   auto subscription = forge::asio::blocking::run(
       app.runtime(), pubsub->subscribe(forge::net::p2p::pubsub::topic{.value = "forge.allowed"}, handler));
   BOOST_TEST(subscription.id != 0U);
   BOOST_CHECK_THROW(
       forge::asio::blocking::run(app.runtime(),
                                  pubsub->subscribe(forge::net::p2p::pubsub::topic{.value = "forge.allowed"}, handler)),
       forge::plugins::p2p::pubsub::exceptions::handler_limit);
   BOOST_CHECK_THROW(forge::asio::blocking::run(
                         app.runtime(), pubsub->publish(forge::net::p2p::pubsub::topic{.value = "forge.denied"}, {1})),
                     forge::plugins::p2p::pubsub::exceptions::topic_not_allowed);
   BOOST_CHECK_THROW(
       forge::asio::blocking::run(
           app.runtime(), pubsub->publish(forge::net::p2p::pubsub::topic{.value = "forge.allowed"}, {1, 2, 3, 4, 5})),
       forge::plugins::p2p::pubsub::exceptions::message_too_large);

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_api_resolver_plugin_config_is_described_from_public_schema) {
   auto plugin = forge::plugins::p2p::resolver::plugin{};
   const auto descriptor = plugin.describe_config();
   BOOST_REQUIRE(descriptor.has_value());
   BOOST_TEST(descriptor->section == "plugins.p2p.resolver");

   const auto& protocol = require_field(*descriptor, "protocol-id");
   BOOST_TEST(protocol.has_default);
   BOOST_TEST(std::get<std::string>(protocol.default_value.storage) == "/forge/api/resolver/2");

   const auto& cache_ttl = require_field(*descriptor, "cache-ttl-ms");
   BOOST_TEST(cache_ttl.has_default);
   BOOST_TEST(std::get<std::uint64_t>(cache_ttl.default_value.storage) > 0U);

   const auto& request_deadline = require_field(*descriptor, "request-deadline-ms");
   BOOST_TEST(request_deadline.has_default);
   BOOST_TEST(std::get<std::uint64_t>(request_deadline.default_value.storage) == 0U);

   const auto& max_peers = require_field(*descriptor, "max-cached-peers");
   BOOST_TEST(max_peers.has_default);
   BOOST_TEST(std::get<std::uint64_t>(max_peers.default_value.storage) > 0U);

   const auto& max_apis = require_field(*descriptor, "max-apis-per-peer");
   BOOST_TEST(max_apis.has_default);
   BOOST_TEST(std::get<std::uint64_t>(max_apis.default_value.storage) > 0U);
}

BOOST_AUTO_TEST_CASE(p2p_api_resolver_rejects_facade_calls_before_initialize) {
   auto runtime = forge::asio::runtime{};
   auto plugin = forge::plugins::p2p::resolver::plugin{};
   auto apis = forge::api::core::registry{};
   auto provider = forge::api::core::installer{apis};
   forge::asio::blocking::run(runtime, plugin.provide(provider));

   auto resolver = apis.get<forge::plugins::p2p::resolver::api>(
       {.id = {"forge.plugins.p2p.resolver"}, .major = 2, .min_revision = 0});

   auto plan = forge::api::core::binding().serve(apis).build();
   BOOST_CHECK_THROW(static_cast<void>(resolver->publish_api(
                         std::move(plan), forge::net::p2p::protocol_id{.value = "/forge/api/node-test/1"})),
                     forge::plugins::p2p::resolver::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW((void)resolver->local_apis(), forge::plugins::p2p::resolver::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, resolver->peer_apis(test_peer(40))),
                     forge::plugins::p2p::resolver::exceptions::plugin_not_initialized);
}

BOOST_AUTO_TEST_CASE(p2p_api_resolver_rejects_invalid_typed_config_before_startup) {
   {
      auto config = test_p2p_config(test_peer(45));
      config.set("plugins.p2p.resolver.protocol-id", std::string{"forge/api/resolver/1"});
      auto app = resolver_only_application{};
      BOOST_CHECK_THROW(app.configure(config), forge::plugins::p2p::resolver::exceptions::invalid_config);
   }

   {
      auto config = test_p2p_config(test_peer(46));
      config.set("plugins.p2p.resolver.max-cached-peers", std::uint64_t{0});
      auto app = resolver_only_application{};
      BOOST_CHECK_THROW(app.configure(config), forge::plugins::p2p::resolver::exceptions::invalid_config);
   }
}

BOOST_AUTO_TEST_CASE(p2p_api_resolver_publishes_metadata_and_delegates_route_mounting) {
   auto app = resolver_plugin_application{};
   app.configure(test_p2p_config(test_peer(50)));
   forge::asio::blocking::run(app.runtime(), app.initialize());

   auto resolver = app.apis().get<forge::plugins::p2p::resolver::api>(
       {.id = {"forge.plugins.p2p.resolver"}, .major = 2, .min_revision = 0});
   const auto entries = resolver->local_apis();
   BOOST_REQUIRE_EQUAL(entries.size(), 1U);
   BOOST_TEST(entries.front().id.value == "node.test");
   BOOST_TEST(entries.front().version.major == 1U);
   BOOST_TEST(entries.front().version.revision == 0U);
   BOOST_TEST(entries.front().protocol == "/forge/api/node-test/1");
   BOOST_TEST(entries.front().codec.value == "forge.raw");
   BOOST_REQUIRE_EQUAL(entries.front().methods.size(), 1U);
   BOOST_TEST(entries.front().methods.front().name == "ping");
   BOOST_TEST(static_cast<int>(entries.front().methods.front().kind) ==
              static_cast<int>(forge::api::core::method_kind::unary));

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_api_resolver_rejects_duplicate_api_and_resolver_protocol_conflict) {
   {
      auto app = duplicate_resolver_plugin_application{};
      app.configure(test_p2p_config(test_peer(60)));
      BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), app.initialize()),
                        forge::plugins::p2p::resolver::exceptions::duplicate_api);
   }

   {
      auto app = resolver_protocol_conflict_application{};
      app.configure(test_p2p_config(test_peer(61)));
      BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), app.initialize()),
                        forge::plugins::p2p::resolver::exceptions::duplicate_api);
   }
}

BOOST_AUTO_TEST_CASE(p2p_api_resolver_resolves_remote_api_and_opens_typed_remote) {
   const auto server_peer = test_peer(70);
   auto server_config = test_p2p_config(server_peer);
   server_config.set("plugins.p2p.node.listen", forge::config::core::value::array_type{
                                                    forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});

   auto server = resolver_plugin_application{};
   server.configure(server_config);
   forge::asio::blocking::run(server.runtime(), server.startup());

   auto server_p2p = server.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   const auto server_endpoint = server_p2p->local_endpoint();
   BOOST_REQUIRE(server_endpoint.has_value());

   auto client_config = test_p2p_config(test_peer(71));
   client_config.set("plugins.p2p.node.bootstrap",
                     forge::config::core::value::array_type{forge::config::core::value{server_endpoint->to_string()}});
   auto client = resolver_only_application{};
   client.configure(client_config);
   forge::asio::blocking::run(client.runtime(), client.startup());

   auto resolver = client.apis().get<forge::plugins::p2p::resolver::api>(
       {.id = {"forge.plugins.p2p.resolver"}, .major = 2, .min_revision = 0});
   const auto remote_entries = forge::asio::blocking::run(client.runtime(), resolver->peer_apis(server_peer));
   BOOST_REQUIRE_EQUAL(remote_entries.size(), 1U);
   BOOST_TEST(remote_entries.front().protocol == "/forge/api/node-test/1");

   auto resolved = forge::asio::blocking::run(
       client.runtime(), resolver->resolve(server_peer, {.id = {"node.test"}, .major = 1, .min_revision = 0}));
   BOOST_TEST(resolved.api.protocol == "/forge/api/node-test/1");

   auto remote = forge::asio::blocking::run(client.runtime(), resolver->remote<node_test_api>(server_peer));
   const auto response = forge::asio::blocking::run(client.runtime(), remote->ping(41));
   BOOST_TEST(response == 42);

   forge::asio::blocking::run(client.runtime(), client.shutdown());
   forge::asio::blocking::run(server.runtime(), server.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_api_resolver_remote_applies_request_deadline_when_node_default_is_unbounded) {
   const auto server_peer = test_peer(76);
   auto server_config = test_p2p_config(server_peer);
   server_config.set("plugins.p2p.node.listen", forge::config::core::value::array_type{
                                                    forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});

   auto handler_state = std::make_shared<nonresponding_node_test_state>();
   auto server = resolver_plugin_application{std::make_shared<nonresponding_node_test_api_impl>(handler_state)};
   server.configure(server_config);
   forge::asio::blocking::run(server.runtime(), server.startup());

   auto server_p2p = server.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   const auto server_endpoint = server_p2p->local_endpoint();
   BOOST_REQUIRE(server_endpoint.has_value());

   auto client_config = test_p2p_config(test_peer(77));
   client_config.set("plugins.p2p.node.api.deadline-ms", std::uint64_t{0});
   client_config.set("plugins.p2p.resolver.request-deadline-ms", std::uint64_t{250});
   client_config.set("plugins.p2p.node.bootstrap",
                     forge::config::core::value::array_type{forge::config::core::value{server_endpoint->to_string()}});
   auto client = resolver_only_application{};
   client.configure(client_config);
   forge::asio::blocking::run(client.runtime(), client.startup());

   auto resolver = client.apis().get<forge::plugins::p2p::resolver::api>(
       {.id = {"forge.plugins.p2p.resolver"}, .major = 2, .min_revision = 0});
   auto remote = forge::asio::blocking::run(client.runtime(), resolver->remote<node_test_api>(server_peer));

   BOOST_CHECK_THROW(forge::asio::blocking::run(client.runtime(), remote->ping(41)),
                     forge::api::core::exceptions::deadline_exceeded);
   BOOST_TEST(handler_state->started.load(std::memory_order_acquire));
   BOOST_TEST(forge::asio::blocking::run(
       server.runtime(),
       async_wait_for_condition([&] { return handler_state->cancelled.load(std::memory_order_acquire); },
                                std::chrono::seconds{2})));

   forge::asio::blocking::run(client.runtime(), client.shutdown());
   forge::asio::blocking::run(server.runtime(), server.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_api_resolver_remote_honors_advertised_transport_options) {
   const auto server_peer = test_peer(72);
   auto server_config = test_p2p_config(server_peer);
   server_config.set("plugins.p2p.node.listen", forge::config::core::value::array_type{
                                                    forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});

   auto server = resolver_custom_transport_application{};
   server.configure(server_config);
   forge::asio::blocking::run(server.runtime(), server.startup());

   auto server_p2p = server.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   const auto server_endpoint = server_p2p->local_endpoint();
   BOOST_REQUIRE(server_endpoint.has_value());

   auto client_config = test_p2p_config(test_peer(73));
   client_config.set("plugins.p2p.node.bootstrap",
                     forge::config::core::value::array_type{forge::config::core::value{server_endpoint->to_string()}});
   auto client = resolver_only_application{};
   client.configure(client_config);
   forge::asio::blocking::run(client.runtime(), client.startup());

   auto resolver = client.apis().get<forge::plugins::p2p::resolver::api>(
       {.id = {"forge.plugins.p2p.resolver"}, .major = 2, .min_revision = 0});
   const auto remote_entries = forge::asio::blocking::run(client.runtime(), resolver->peer_apis(server_peer));
   BOOST_REQUIRE_EQUAL(remote_entries.size(), 1U);
   BOOST_TEST(remote_entries.front().codec.value == "forge.test.raw");
   BOOST_TEST(remote_entries.front().max_inflight == 7U);
   BOOST_TEST(remote_entries.front().max_frame_size == 512U * 1024U);

   auto remote = forge::asio::blocking::run(client.runtime(), resolver->remote<node_test_api>(server_peer));
   const auto response = forge::asio::blocking::run(client.runtime(), remote->ping(41));
   BOOST_TEST(response == 42);

   forge::asio::blocking::run(client.runtime(), client.shutdown());
   forge::asio::blocking::run(server.runtime(), server.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_api_resolver_supports_receipt_based_product_api) {
   const auto server_peer = test_peer(74);
   auto server_config = test_p2p_config(server_peer);
   server_config.set("plugins.p2p.node.listen", forge::config::core::value::array_type{
                                                    forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});

   auto server_state = std::make_shared<receipt_test_state>();
   auto server = receipt_resolver_application{server_state};
   server.configure(server_config);
   forge::asio::blocking::run(server.runtime(), server.startup());

   auto server_p2p = server.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   const auto server_endpoint = server_p2p->local_endpoint();
   BOOST_REQUIRE(server_endpoint.has_value());

   auto client_config = test_p2p_config(test_peer(75));
   client_config.set("plugins.p2p.node.bootstrap",
                     forge::config::core::value::array_type{forge::config::core::value{server_endpoint->to_string()}});
   auto client = resolver_only_application{};
   client.configure(client_config);
   forge::asio::blocking::run(client.runtime(), client.startup());

   auto resolver = client.apis().get<forge::plugins::p2p::resolver::api>(
       {.id = {"forge.plugins.p2p.resolver"}, .major = 2, .min_revision = 0});
   const auto resolved = forge::asio::blocking::run(
       client.runtime(), resolver->resolve(server_peer, {.id = {"receipt.test"}, .major = 1, .min_revision = 0}));
   BOOST_TEST(resolved.api.protocol == "/forge/api/receipt-test/1");

   auto remote = forge::asio::blocking::run(client.runtime(), resolver->remote<receipt_test_api>(server_peer));
   const auto request = operation_request{.request_id = "request-1", .subject = "neutral-operation", .revision = 7};

   const auto first = forge::asio::blocking::run(client.runtime(), remote->apply(request));
   BOOST_TEST(first.accepted);
   BOOST_TEST(first.request_id == request.request_id);
   BOOST_TEST(first.applied_revision == 1U);
   BOOST_TEST(first.authority == "receipt-test");
   BOOST_TEST(first.evidence == "neutral-operation:7:1");

   const auto repeated = forge::asio::blocking::run(client.runtime(), remote->apply(request));
   BOOST_TEST(repeated.request_id == first.request_id);
   BOOST_TEST(repeated.accepted == first.accepted);
   BOOST_TEST(repeated.applied_revision == first.applied_revision);
   BOOST_TEST(repeated.authority == first.authority);
   BOOST_TEST(repeated.evidence == first.evidence);
   {
      auto lock = std::scoped_lock{server_state->mutex};
      BOOST_TEST(server_state->applied == 1U);
      BOOST_TEST(server_state->receipts.size() == 1U);
   }

   forge::asio::blocking::run(client.runtime(), client.shutdown());
   forge::asio::blocking::run(server.runtime(), server.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_api_resolver_enforces_version_compatibility) {
   const auto server_peer = test_peer(80);
   auto server_config = test_p2p_config(server_peer);
   server_config.set("plugins.p2p.node.listen", forge::config::core::value::array_type{
                                                    forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});

   auto server = resolver_plugin_application{};
   server.configure(server_config);
   forge::asio::blocking::run(server.runtime(), server.startup());

   auto server_p2p = server.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   const auto server_endpoint = server_p2p->local_endpoint();
   BOOST_REQUIRE(server_endpoint.has_value());

   auto client_config = test_p2p_config(test_peer(81));
   client_config.set("plugins.p2p.node.bootstrap",
                     forge::config::core::value::array_type{forge::config::core::value{server_endpoint->to_string()}});
   auto client = resolver_only_application{};
   client.configure(client_config);
   forge::asio::blocking::run(client.runtime(), client.startup());

   auto resolver = client.apis().get<forge::plugins::p2p::resolver::api>(
       {.id = {"forge.plugins.p2p.resolver"}, .major = 2, .min_revision = 0});
   BOOST_CHECK_NO_THROW(forge::asio::blocking::run(
       client.runtime(), resolver->resolve(server_peer, {.id = {"node.test"}, .major = 1, .min_revision = 0})));
   BOOST_CHECK_THROW(
       forge::asio::blocking::run(
           client.runtime(), resolver->resolve(server_peer, {.id = {"node.test"}, .major = 1, .min_revision = 10})),
       forge::plugins::p2p::resolver::exceptions::incompatible_api);

   forge::asio::blocking::run(client.runtime(), client.shutdown());
   forge::asio::blocking::run(server.runtime(), server.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_api_resolver_rejects_malformed_remote_metadata_without_caching_it) {
   const auto server_peer = test_peer(85);
   auto bad = resolver_test_entry("/forge/api/node-test/1");
   auto duplicate = forge::plugins::p2p::resolver::response{.apis = {bad, bad}};
   auto good = forge::plugins::p2p::resolver::response{
       .apis = {resolver_test_entry("/forge/api/node-test/1")},
   };
   auto state = std::make_shared<scripted_resolver_state>(
       scripted_resolver_state{.responses = {std::move(duplicate), std::move(good)}});

   auto server_config = test_p2p_config(server_peer);
   server_config.set("plugins.p2p.node.listen", forge::config::core::value::array_type{
                                                    forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});

   auto server = scripted_resolver_application{state};
   server.configure(server_config);
   forge::asio::blocking::run(server.runtime(), server.startup());

   auto server_p2p = server.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   const auto server_endpoint = server_p2p->local_endpoint();
   BOOST_REQUIRE(server_endpoint.has_value());

   auto client_config = test_p2p_config(test_peer(86));
   client_config.set("plugins.p2p.node.bootstrap",
                     forge::config::core::value::array_type{forge::config::core::value{server_endpoint->to_string()}});
   auto client = resolver_only_application{};
   client.configure(client_config);
   forge::asio::blocking::run(client.runtime(), client.startup());

   auto resolver = client.apis().get<forge::plugins::p2p::resolver::api>(
       {.id = {"forge.plugins.p2p.resolver"}, .major = 2, .min_revision = 0});
   BOOST_CHECK_THROW(forge::asio::blocking::run(client.runtime(), resolver->peer_apis(server_peer)),
                     forge::plugins::p2p::resolver::exceptions::protocol_error);
   const auto entries = forge::asio::blocking::run(client.runtime(), resolver->peer_apis(server_peer));
   BOOST_REQUIRE_EQUAL(entries.size(), 1U);
   BOOST_TEST(state->calls == 2U);

   forge::asio::blocking::run(client.runtime(), client.shutdown());
   forge::asio::blocking::run(server.runtime(), server.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_api_resolver_cache_ttl_and_force_refresh_are_behavioral) {
   const auto server_peer = test_peer(90);
   auto server_config = test_p2p_config(server_peer);
   server_config.set("plugins.p2p.node.listen", forge::config::core::value::array_type{
                                                    forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"}});

   auto server = resolver_plugin_application{};
   server.configure(server_config);
   forge::asio::blocking::run(server.runtime(), server.startup());

   auto server_p2p = server.apis().get<forge::plugins::p2p::node::api>(
       {.id = {"forge.plugins.p2p.node"}, .major = 2, .min_revision = 0});
   const auto server_endpoint = server_p2p->local_endpoint();
   BOOST_REQUIRE(server_endpoint.has_value());

   auto client_config = test_p2p_config(test_peer(91));
   client_config.set("plugins.p2p.node.bootstrap",
                     forge::config::core::value::array_type{forge::config::core::value{server_endpoint->to_string()}});
   client_config.set("plugins.p2p.resolver.cache-ttl-ms", std::uint64_t{200});
   auto client = resolver_only_application{};
   client.configure(client_config);
   forge::asio::blocking::run(client.runtime(), client.startup());

   auto resolver = client.apis().get<forge::plugins::p2p::resolver::api>(
       {.id = {"forge.plugins.p2p.resolver"}, .major = 2, .min_revision = 0});
   BOOST_REQUIRE_EQUAL(forge::asio::blocking::run(client.runtime(), resolver->peer_apis(server_peer)).size(), 1U);

   forge::asio::blocking::run(server.runtime(), server.shutdown());
   BOOST_REQUIRE_EQUAL(forge::asio::blocking::run(client.runtime(), resolver->peer_apis(server_peer)).size(), 1U);

   BOOST_CHECK_THROW(
       forge::asio::blocking::run(client.runtime(), resolver->peer_apis(server_peer, {.force_refresh = true})),
       forge::exceptions::base);

   std::this_thread::sleep_for(std::chrono::milliseconds{250});
   BOOST_CHECK_THROW(forge::asio::blocking::run(client.runtime(), resolver->peer_apis(server_peer)),
                     forge::exceptions::base);

   forge::asio::blocking::run(client.runtime(), client.shutdown());
}
