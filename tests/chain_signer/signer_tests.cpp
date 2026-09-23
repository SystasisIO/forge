#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/test/unit_test.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

import forge.api.auth.authenticated_caller;
import forge.api.core.descriptor;
import forge.api.core.exceptions;
import forge.api.core.registry;
import forge.api.core.trusted_invocation;
import forge.api.core.types;
import forge.app.events;
import forge.app.plugin_context;
import forge.app.signals;
import forge.asio.blocking;
import forge.asio.notification;
import forge.asio.runtime;
import forge.asio.task;
import forge.chain.api.exceptions;
import forge.chain.api.block_signer;
import forge.chain.api.finality_signer;
import forge.chain.api.transaction_signer;
import forge.chain.protocol.transaction;
import forge.chain.protocol.block;
import forge.chain.protocol.block_signing;
import forge.chain.protocol.producer_authority;
import forge.chain.savanna.admission;
import forge.chain.savanna.exceptions;
import forge.chain.savanna.vote;
import forge.chain.transaction.types;
import forge.config.core.component;
import forge.config.core.decode;
import forge.config.core.document;
import forge.config.core.value;
import forge.crypto.asymmetric;
import forge.crypto.bls;
import forge.crypto.bls.signer.configured_provider;
import forge.crypto.core.secret_string;
import forge.crypto.digest.sha256;
import forge.crypto.signer.configured_provider;
import forge.crypto.signer.provider;
import forge.plugins.chain.signer.plugin;
import forge.plugins.chain.signer.exceptions;
import forge.plugins.chain.signer.types;
import forge.raw.raw;

namespace chain_api = forge::chain::api;
namespace chain_signer = forge::plugins::chain::signer;
namespace protocol = forge::chain::protocol;
namespace savanna = forge::chain::savanna;
namespace transaction = forge::chain::transaction;
namespace asymmetric = forge::crypto::asymmetric;
namespace bls = forge::crypto::bls;
namespace digest = forge::crypto::digest;
namespace crypto_signer = forge::crypto::signer;

namespace {

constexpr auto now_seconds = std::uint32_t{1'700'000'000U};

static_assert(forge::api::core::local_interface<chain_api::transaction_signer>);
static_assert(forge::api::core::remote_interface<chain_api::transaction_signer>);
static_assert(forge::api::core::local_interface<chain_api::block_signer>);
static_assert(forge::api::core::remote_interface<chain_api::block_signer>);
static_assert(forge::api::core::local_interface<chain_api::finality_signer>);
static_assert(!forge::api::core::remote_interface<chain_api::finality_signer>);
static_assert(forge::api::core::server_supplied<forge::api::auth::authenticated_caller>::required);
static_assert(std::same_as<decltype(std::declval<chain_api::transaction_signer&>().sign(
                               std::declval<transaction::unsigned_transaction>(),
                               std::declval<forge::api::auth::authenticated_caller>())),
                           boost::asio::awaitable<transaction::prepared_transaction>>);
static_assert(std::same_as<decltype(std::declval<chain_api::finality_signer&>().sign_vote(
                               std::declval<savanna::block_ref>(), std::declval<savanna::vote_kind>())),
                           boost::asio::awaitable<savanna::finalizer_vote>>);

template <typename T>
concept exposes_payload = requires(T value) { value.payload; };

template <typename T>
concept exposes_signature = requires(T value) { value.signature; };

template <typename T>
concept exposes_token = requires(T value) { value.token; };

template <typename T>
concept exposes_access_token = requires(T value) { value.access_token; };

template <typename T>
concept exposes_bearer_token = requires(T value) { value.bearer_token; };

static_assert(!exposes_payload<chain_signer::audit_entry>);
static_assert(!exposes_signature<chain_signer::audit_entry>);
static_assert(!exposes_token<chain_signer::audit_entry>);
static_assert(!exposes_access_token<chain_signer::audit_entry>);
static_assert(!exposes_bearer_token<chain_signer::audit_entry>);

[[nodiscard]] const forge::config::core::field_descriptor&
require_field(const forge::config::core::component_descriptor& descriptor, std::string_view name) {
   const auto found = std::ranges::find_if(descriptor.fields, [&](const auto& field) { return field.name == name; });
   BOOST_REQUIRE(found != descriptor.fields.end());
   return *found;
}

[[nodiscard]] bool has_field(const forge::config::core::component_descriptor& descriptor, std::string_view name) {
   return std::ranges::any_of(descriptor.fields, [&](const auto& field) { return field.name == name; });
}

[[nodiscard]] forge::config::core::document chain_signer_document(bool include_actions,
                                                                  std::string_view authorization = {}) {
   auto signing = forge::config::core::value::object_type{};
   signing["provider"] = "transaction";
   signing["key-id"] = "writer-key";
   signing["expected-public-key"] = "PUB_K1_TEST";

   auto action = forge::config::core::value::object_type{};
   action["account"] = "storage";
   action["action"] = "write";
   action["actor"] = "writer";
   action["permission"] = "active";

   auto profile = forge::config::core::value::object_type{};
   profile["name"] = "storage-writer";
   profile["chain-id"] = std::string(64U, '1');
   profile["signing"] = std::move(signing);
   if (!authorization.empty()) {
      profile["remote-authorization"] = std::string{authorization};
   }
   profile["allow-local"] = true;
   profile["actions"] = include_actions
                            ? forge::config::core::value::array_type{forge::config::core::value{std::move(action)}}
                            : forge::config::core::value::array_type{};

   auto document = forge::config::core::document{};
   document.set("plugins.chain.signer.transaction-profiles",
                forge::config::core::value::array_type{forge::config::core::value{std::move(profile)}});
   document.set("plugins.chain.signer.max-inflight", std::uint64_t{3});
   document.set("plugins.chain.signer.max-queued", std::uint64_t{5});
   document.set("plugins.chain.signer.max-queued-bytes", std::uint64_t{8192});
   document.set("plugins.chain.signer.max-per-caller", std::uint64_t{2});
   document.set("plugins.chain.signer.shutdown-timeout-ms", std::uint64_t{750});
   return document;
}

[[nodiscard]] digest::sha256 test_digest(const std::string& label) {
   return digest::sha256::hash(label);
}

struct transaction_material {
   crypto_signer::key_id id;
   asymmetric::public_key public_key;
   std::shared_ptr<crypto_signer::configured_provider> provider;
};

[[nodiscard]] transaction_material make_transaction_material(std::string id, const std::string& seed_label) {
   const auto secret = test_digest(seed_label);
   const auto private_key = asymmetric::private_key::regenerate(secret);
   const auto public_key = private_key.get_public_key();
   const auto encoded = asymmetric::encoding::forge().format(private_key);
   auto provider =
       crypto_signer::configured_provider::from_private_key({.value = id}, forge::crypto::core::secret_string{encoded});
   return {
       .id = {.value = std::move(id)},
       .public_key = public_key,
       .provider = std::move(provider),
   };
}

struct finality_material {
   bls::public_key public_key;
   std::shared_ptr<bls::signer::configured_provider> provider;
};

[[nodiscard]] finality_material make_finality_material(std::uint8_t seed_base) {
   auto seed = std::array<std::uint8_t, 32>{};
   for (auto index = std::size_t{}; index < seed.size(); ++index) {
      seed[index] = static_cast<std::uint8_t>(seed_base + index);
   }
   auto private_key = bls::private_key{std::span<const std::uint8_t>{seed}};
   auto public_key = private_key.get_public_key();
   return {
       .public_key = std::move(public_key),
       .provider = bls::signer::configured_provider::create(std::move(private_key)),
   };
}

class collecting_audit final : public chain_signer::audit_sink {
 public:
   void record(const chain_signer::audit_entry& value) noexcept override {
      const auto lock = std::scoped_lock{mutex_};
      entries_.push_back(value);
   }

   [[nodiscard]] std::vector<chain_signer::audit_entry> entries() const {
      const auto lock = std::scoped_lock{mutex_};
      return entries_;
   }

 private:
   mutable std::mutex mutex_;
   std::vector<chain_signer::audit_entry> entries_;
};

[[nodiscard]] chain_signer::transaction_profile exact_profile(const transaction_material& material,
                                                              protocol::chain_id chain, bool allow_local,
                                                              std::vector<chain_signer::caller_rule> callers = {}) {
   return {
       .name = "storage-writer",
       .chain_id = chain.str(),
       .signing =
           {
               .provider = "transaction",
               .key_id = material.id.value,
               .expected_public_key = asymmetric::encoding::forge().format(material.public_key),
           },
       .allow_local = allow_local,
       .callers = std::move(callers),
       .actions = {{
           .account = "storage",
           .action = "write",
           .actor = "writer",
           .permission = "active",
       }},
       .max_expiration_seconds = 120,
       .max_actions = 4,
       .max_packed_bytes = 4096,
   };
}

[[nodiscard]] chain_signer::config exact_config(const transaction_material& material, protocol::chain_id chain,
                                                bool allow_local, std::vector<chain_signer::caller_rule> callers = {}) {
   return {
       .transaction_profiles =
           {
               exact_profile(material, std::move(chain), allow_local, std::move(callers)),
           },
       .max_inflight = 4,
       .max_queued = 8,
   };
}

[[nodiscard]] chain_signer::plugin_options
plugin_options(chain_signer::config config, std::shared_ptr<crypto_signer::provider> transaction_provider,
               std::shared_ptr<collecting_audit> audit = {},
               std::shared_ptr<bls::signer::provider> finality_provider = {},
               std::shared_ptr<chain_signer::semantic_authorization_provider> semantic_authorization = {}) {
   auto result = chain_signer::plugin_options{
       .providers = {{
           .name = "transaction",
           .value = std::move(transaction_provider),
       }},
       .semantic_authorization = std::move(semantic_authorization),
       .initial_config = std::move(config),
       .audit = std::move(audit),
       .now = [] { return protocol::time_point_sec{now_seconds}; },
   };
   if (finality_provider) {
      result.finality_providers.push_back({
          .name = "finality",
          .value = std::move(finality_provider),
      });
   }
   return result;
}

class signer_harness {
 public:
   explicit signer_harness(chain_signer::plugin_options options)
       : runtime_{forge::asio::runtime_options{.worker_threads = 1, .thread_name = "chain-signer-test"}},
         scheduler_{runtime_}, plugin_{std::move(options)} {
      auto provider = forge::api::core::installer{apis_};
      forge::asio::blocking::run(runtime_, plugin_.provide(provider));
      auto context = forge::app::plugin_context{scheduler_, apis_, signals_, events_};
      forge::asio::blocking::run(runtime_, plugin_.initialize(context));
      forge::asio::blocking::run(runtime_, plugin_.startup());
   }

   ~signer_harness() {
      plugin_.request_stop();
      try {
         forge::asio::blocking::run(runtime_, plugin_.shutdown());
      } catch (...) {
      }
   }

   signer_harness(const signer_harness&) = delete;
   signer_harness& operator=(const signer_harness&) = delete;

   [[nodiscard]] forge::asio::runtime& runtime() noexcept {
      return runtime_;
   }

   [[nodiscard]] forge::api::core::registry& apis() noexcept {
      return apis_;
   }

   [[nodiscard]] chain_signer::plugin& plugin() noexcept {
      return plugin_;
   }

   [[nodiscard]] forge::api::core::handle<chain_api::transaction_signer> transactions() const {
      return apis_.get<chain_api::transaction_signer>(chain_api::transaction_signer::ref());
   }

   [[nodiscard]] forge::api::core::handle<chain_api::block_signer> blocks() const {
      return apis_.get<chain_api::block_signer>(chain_api::block_signer::ref());
   }

   [[nodiscard]] forge::api::core::handle<chain_api::finality_signer> finality() const {
      return apis_.get<chain_api::finality_signer>(chain_api::finality_signer::ref());
   }

 private:
   forge::asio::runtime runtime_;
   forge::asio::task::scheduler scheduler_;
   forge::api::core::registry apis_;
   forge::app::signal_bus signals_;
   forge::app::event_bus events_;
   chain_signer::plugin plugin_;
};

[[nodiscard]] protocol::action allowed_action() {
   auto result = protocol::action{};
   result.account = protocol::account_name{"storage"};
   result.name = protocol::action_name{"write"};
   result.authorization = {{
       .actor = protocol::account_name{"writer"},
       .permission = protocol::permission_name{"active"},
   }};
   result.data = {0x01U, 0x02U, 0x03U};
   return result;
}

[[nodiscard]] transaction::unsigned_transaction allowed_transaction(protocol::chain_id chain) {
   auto value = protocol::transaction{};
   value.expiration = protocol::time_point_sec{now_seconds + 30U};
   value.actions.push_back(allowed_action());
   return {
       .chain = std::move(chain),
       .value = std::move(value),
   };
}

[[nodiscard]] protocol::block_header allowed_block_header() {
   auto header = protocol::block_header{};
   header.producer = protocol::account_name{"writer"};
   return header;
}

[[nodiscard]] chain_signer::block_profile exact_block_profile(
    protocol::chain_id chain, const std::vector<transaction_material>& materials, bool allow_local,
    std::vector<chain_signer::caller_rule> callers = {}) {
   auto profile = chain_signer::block_profile{
       .name = "writer-blocks",
       .chain_id = chain.str(),
       .producer = "writer",
       .allow_local = allow_local,
       .callers = std::move(callers),
   };
   for (auto index = std::size_t{}; index < materials.size(); ++index) {
      profile.signing.push_back({
          .provider = "key-" + std::to_string(index),
          .key_id = materials[index].id.value,
          .expected_public_key = asymmetric::encoding::forge().format(materials[index].public_key),
      });
   }
   return profile;
}

class counting_provider final : public crypto_signer::provider {
 public:
   explicit counting_provider(std::shared_ptr<crypto_signer::provider> inner) : inner_{std::move(inner)} {}

   boost::asio::awaitable<std::vector<crypto_signer::key_info>> keys() override {
      co_return co_await inner_->keys();
   }
   boost::asio::awaitable<crypto_signer::key_info> describe(const crypto_signer::key_id& id) override {
      describes.fetch_add(1U);
      co_return co_await inner_->describe(id);
   }
   boost::asio::awaitable<crypto_signer::sign_digest_response>
   sign_digest(crypto_signer::sign_digest_request request) override {
      signs.fetch_add(1U);
      co_return co_await inner_->sign_digest(std::move(request));
   }

   std::atomic_size_t describes{0};
   std::atomic_size_t signs{0};

 private:
   std::shared_ptr<crypto_signer::provider> inner_;
};

void check_prepared_transaction(const transaction::unsigned_transaction& source,
                                const transaction::prepared_transaction& prepared,
                                const asymmetric::public_key& expected_key) {
   const auto signed_value = prepared.packed.get_signed_transaction();
   BOOST_TEST(prepared.packed.id() == source.value.id());
   BOOST_REQUIRE_EQUAL(signed_value.signatures.size(), 1U);
   BOOST_REQUIRE_EQUAL(prepared.packed.signatures.size(), 1U);
   const auto signing_digest = source.value.sig_digest(source.chain, source.context_free_data);
   BOOST_TEST(asymmetric::recover(signed_value.signatures.front(), signing_digest) == expected_key);
}

[[nodiscard]] forge::api::core::frame dispatch_transaction(signer_harness& harness,
                                                           const transaction::unsigned_transaction& value,
                                                           forge::api::auth::authenticated_caller wire_caller,
                                                           forge::api::auth::authenticated_caller trusted_caller,
                                                           std::uint64_t call_id) {
   auto request = forge::api::core::frame{
       .kind = forge::api::core::frame_kind::request,
       .id = {.value = call_id},
       .api = chain_api::transaction_signer::ref(),
       .method = "sign",
       .codec = {.value = "forge.raw"},
       .payload = forge::api::core::pack_body(std::tuple{value, std::move(wire_caller)}),
   };
   auto trusted = forge::api::core::trusted_invocation_builder{}.set(std::move(trusted_caller)).build();
   return forge::asio::blocking::run(harness.runtime(),
                                     harness.apis().dispatch_contextual(std::move(request), std::move(trusted)));
}

[[nodiscard]] forge::api::core::frame dispatch_block(signer_harness& harness,
                                                     const protocol::block_sign_request& value,
                                                     forge::api::auth::authenticated_caller wire_caller,
                                                     forge::api::auth::authenticated_caller trusted_caller,
                                                     std::uint64_t call_id) {
   auto request = forge::api::core::frame{
       .kind = forge::api::core::frame_kind::request,
       .id = {.value = call_id},
       .api = chain_api::block_signer::ref(),
       .method = "sign",
       .codec = {.value = "forge.raw"},
       .payload = forge::api::core::pack_body(std::tuple{value, std::move(wire_caller)}),
   };
   auto trusted = forge::api::core::trusted_invocation_builder{}.set(std::move(trusted_caller)).build();
   return forge::asio::blocking::run(harness.runtime(),
                                     harness.apis().dispatch_contextual(std::move(request), std::move(trusted)));
}

void check_authorization_error(const forge::api::core::frame& response) {
   BOOST_REQUIRE(response.kind == forge::api::core::frame_kind::error);
   const auto error = forge::api::core::unpack_body<forge::api::core::error_payload>(response.payload);
   BOOST_TEST(error.error == "authorization_denied");
   BOOST_TEST(error.identity.category == "forge.chain.api");
   BOOST_TEST(error.identity.code == static_cast<std::uint32_t>(chain_api::exceptions::code::authorization_denied));
}

template <typename Mutation>
void check_policy_denied(signer_harness& harness, const transaction::unsigned_transaction& baseline, const char* label,
                         Mutation mutate) {
   auto candidate = baseline;
   mutate(candidate);
   BOOST_TEST_CONTEXT(label) {
      BOOST_CHECK_THROW(
          forge::asio::blocking::run(harness.runtime(), harness.transactions()->sign(std::move(candidate), {})),
          chain_api::exceptions::authorization_denied);
   }
}

class mismatched_signature_provider final : public crypto_signer::provider {
 public:
   mismatched_signature_provider(std::shared_ptr<crypto_signer::configured_provider> expected,
                                 std::shared_ptr<crypto_signer::configured_provider> signing,
                                 asymmetric::public_key expected_key)
       : expected_{std::move(expected)}, signing_{std::move(signing)}, expected_key_{std::move(expected_key)} {}

   boost::asio::awaitable<std::vector<crypto_signer::key_info>> keys() override {
      co_return co_await expected_->keys();
   }

   boost::asio::awaitable<crypto_signer::key_info> describe(const crypto_signer::key_id& id) override {
      co_return co_await expected_->describe(id);
   }

   boost::asio::awaitable<crypto_signer::sign_digest_response>
   sign_digest(crypto_signer::sign_digest_request request) override {
      auto result = co_await signing_->sign_digest(std::move(request));
      result.public_key = expected_key_;
      co_return result;
   }

 private:
   std::shared_ptr<crypto_signer::configured_provider> expected_;
   std::shared_ptr<crypto_signer::configured_provider> signing_;
   asymmetric::public_key expected_key_;
};

class mismatched_identity_provider final : public crypto_signer::provider {
 public:
   mismatched_identity_provider(std::shared_ptr<crypto_signer::provider> inner,
                                asymmetric::public_key replacement)
       : inner_{std::move(inner)}, replacement_{std::move(replacement)} {}

   boost::asio::awaitable<std::vector<crypto_signer::key_info>> keys() override {
      co_return co_await inner_->keys();
   }
   boost::asio::awaitable<crypto_signer::key_info> describe(const crypto_signer::key_id& id) override {
      auto identity = co_await inner_->describe(id);
      identity.public_key = replacement_;
      co_return identity;
   }
   boost::asio::awaitable<crypto_signer::sign_digest_response>
   sign_digest(crypto_signer::sign_digest_request request) override {
      signs.fetch_add(1U);
      co_return co_await inner_->sign_digest(std::move(request));
   }

   std::atomic_size_t signs{0};

 private:
   std::shared_ptr<crypto_signer::provider> inner_;
   asymmetric::public_key replacement_;
};

class gated_transaction_provider final : public crypto_signer::provider {
 public:
   explicit gated_transaction_provider(std::shared_ptr<crypto_signer::configured_provider> inner,
                                       bool ignore_cancellation = false)
       : inner_{std::move(inner)}, ignore_cancellation_{ignore_cancellation} {}

   boost::asio::awaitable<std::vector<crypto_signer::key_info>> keys() override {
      co_return co_await inner_->keys();
   }

   boost::asio::awaitable<crypto_signer::key_info> describe(const crypto_signer::key_id& id) override {
      co_return co_await inner_->describe(id);
   }

   boost::asio::awaitable<crypto_signer::sign_digest_response>
   sign_digest(crypto_signer::sign_digest_request request) override {
      if (ignore_cancellation_) {
         co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
      }
      entered_.fetch_add(1U, std::memory_order_release);

      auto observed = released_changed_.epoch();
      while (!released_.load(std::memory_order_acquire)) {
         observed = co_await released_changed_.async_wait(observed);
      }
      co_return co_await inner_->sign_digest(std::move(request));
   }

   [[nodiscard]] bool wait_for_entered(std::size_t count) const {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
      while (entered_.load(std::memory_order_acquire) < count && std::chrono::steady_clock::now() < deadline) {
         std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
      return entered_.load(std::memory_order_acquire) >= count;
   }

   void release() noexcept {
      released_.store(true, std::memory_order_release);
      released_changed_.notify();
   }

 private:
   std::shared_ptr<crypto_signer::configured_provider> inner_;
   bool ignore_cancellation_ = false;
   std::atomic_size_t entered_{0};
   std::atomic_bool released_{false};
   forge::asio::notification released_changed_;
};

class payload_semantic_authorizer final : public chain_signer::semantic_authorization_provider {
 public:
   explicit payload_semantic_authorizer(protocol::bytes approved) : approved_{std::move(approved)} {}

   boost::asio::awaitable<bool> authorize_remote(std::string_view profile,
                                                 const transaction::unsigned_transaction& value,
                                                 const forge::api::auth::authenticated_caller& caller) override {
      calls_.fetch_add(1U, std::memory_order_release);
      co_return profile == "storage-writer" && caller.transport_authenticated() && value.value.actions.size() == 1U &&
          value.value.actions.front().data == approved_;
   }

   [[nodiscard]] std::size_t calls() const noexcept {
      return calls_.load(std::memory_order_acquire);
   }

 private:
   protocol::bytes approved_;
   std::atomic_size_t calls_{0};
};

struct provider_release_guard {
   std::shared_ptr<gated_transaction_provider> provider;

   ~provider_release_guard() {
      provider->release();
   }
};

void flush_runtime(forge::asio::runtime& runtime) {
   auto reached = std::promise<void>{};
   auto ready = reached.get_future();
   boost::asio::post(runtime.context(), [&reached] { reached.set_value(); });
   BOOST_REQUIRE(static_cast<int>(ready.wait_for(std::chrono::seconds{2})) ==
                 static_cast<int>(std::future_status::ready));
}

} // namespace

BOOST_AUTO_TEST_SUITE(chain_signer_contract_tests)

BOOST_AUTO_TEST_CASE(config_schema_exposes_policy_bindings_without_key_material) {
   const auto descriptor = forge::config::core::describe_component<chain_signer::config>("plugins.chain.signer");

   BOOST_TEST(descriptor.section == "plugins.chain.signer");
   BOOST_TEST(require_field(descriptor, "transaction-profiles").required == false);
   BOOST_TEST(require_field(descriptor, "block-profiles").required == false);
   BOOST_TEST(require_field(descriptor, "max-inflight").has_default);
   BOOST_TEST(require_field(descriptor, "max-queued").has_default);
   BOOST_TEST(require_field(descriptor, "max-queued-bytes").has_default);
   BOOST_TEST(require_field(descriptor, "max-per-caller").has_default);
   BOOST_TEST(require_field(descriptor, "shutdown-timeout-ms").has_default);
   BOOST_TEST(!has_field(descriptor, "private-key"));
   BOOST_TEST(!has_field(descriptor, "private-keys"));
   BOOST_TEST(!has_field(descriptor, "keys"));
   BOOST_TEST(!has_field(descriptor, "secrets"));
}

BOOST_AUTO_TEST_CASE(config_decodes_exact_transaction_policy) {
   const auto document = chain_signer_document(true);
   const auto decoded = forge::config::core::decode<chain_signer::config>(document, "plugins.chain.signer");

   BOOST_REQUIRE(decoded.ok());
   BOOST_REQUIRE_EQUAL(decoded.value.transaction_profiles.size(), 1U);
   const auto& profile = decoded.value.transaction_profiles.front();
   BOOST_TEST(profile.name == "storage-writer");
   BOOST_TEST(profile.chain_id == std::string(64U, '1'));
   BOOST_TEST(profile.signing.provider == "transaction");
   BOOST_TEST(profile.signing.key_id == "writer-key");
   BOOST_TEST(profile.signing.expected_public_key == "PUB_K1_TEST");
   BOOST_TEST(profile.allow_local);
   BOOST_REQUIRE_EQUAL(profile.actions.size(), 1U);
   BOOST_TEST(profile.actions.front().account == "storage");
   BOOST_TEST(profile.actions.front().action == "write");
   BOOST_TEST(profile.actions.front().actor == "writer");
   BOOST_TEST(profile.actions.front().permission == "active");
   BOOST_TEST(profile.max_delay_seconds == 0U);
   BOOST_CHECK(profile.authorization == chain_signer::remote_authorization::semantic);
   BOOST_TEST(decoded.value.max_inflight == 3U);
   BOOST_TEST(decoded.value.max_queued == 5U);
   BOOST_TEST(decoded.value.max_queued_bytes == 8192U);
   BOOST_TEST(decoded.value.max_per_caller == 2U);
   BOOST_TEST(decoded.value.shutdown_timeout_ms == 750U);
}

BOOST_AUTO_TEST_CASE(config_rejects_profile_without_exact_actions) {
   const auto document = chain_signer_document(false);
   const auto decoded = forge::config::core::decode<chain_signer::config>(document, "plugins.chain.signer");

   BOOST_TEST(!decoded.ok());
   BOOST_TEST(std::ranges::any_of(decoded.diagnostics.entries, [](const auto& entry) {
      return entry.path == "plugins.chain.signer.transaction-profiles[0].actions" && entry.code == "schema.min_items";
   }));
}

BOOST_AUTO_TEST_CASE(config_decodes_block_bindings_and_explicit_profile_only_mode) {
   auto document = chain_signer_document(true, "profile_only");
   auto signing = forge::config::core::value::object_type{};
   signing["provider"] = "key-0";
   signing["key-id"] = "producer-key";
   signing["expected-public-key"] = "PUB_K1_TEST";
   auto caller = forge::config::core::value::object_type{};
   caller["source"] = "p2p_peer";
   caller["fingerprint"] = std::string(64U, 'a');
   auto block = forge::config::core::value::object_type{};
   block["name"] = "writer-block";
   block["chain-id"] = std::string(64U, '1');
   block["producer"] = "writer";
   block["signing"] = forge::config::core::value::array_type{forge::config::core::value{std::move(signing)}};
   block["callers"] = forge::config::core::value::array_type{forge::config::core::value{std::move(caller)}};
   block["max-header-bytes"] = std::uint64_t{2U * 1024U * 1024U};
   document.set("plugins.chain.signer.block-profiles",
                forge::config::core::value::array_type{forge::config::core::value{std::move(block)}});
   const auto decoded = forge::config::core::decode<chain_signer::config>(document, "plugins.chain.signer");
   BOOST_REQUIRE(decoded.ok());
   BOOST_CHECK(decoded.value.transaction_profiles.front().authorization ==
               chain_signer::remote_authorization::profile_only);
   BOOST_REQUIRE_EQUAL(decoded.value.block_profiles.size(), 1U);
   const auto& profile = decoded.value.block_profiles.front();
   BOOST_TEST(profile.producer == "writer");
   BOOST_TEST(!profile.allow_local);
   BOOST_TEST(profile.max_header_bytes == 2U * 1024U * 1024U);
   BOOST_REQUIRE_EQUAL(profile.signing.size(), 1U);
   BOOST_TEST(profile.signing.front().provider == "key-0");
   BOOST_TEST(profile.signing.front().key_id == "producer-key");
   BOOST_REQUIRE_EQUAL(profile.callers.size(), 1U);
   BOOST_CHECK(profile.callers.front().source == forge::api::auth::caller_source::p2p_peer);
   BOOST_TEST(profile.callers.front().fingerprint == std::string(64U, 'a'));

   auto invalid_document = chain_signer_document(true, "unknown_mode");
   const auto invalid = forge::config::core::decode<chain_signer::config>(invalid_document, "plugins.chain.signer");
   BOOST_TEST(!invalid.ok());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(chain_signer_runtime_tests)

BOOST_AUTO_TEST_CASE(exact_local_profile_prepares_a_valid_canonical_transaction) {
   const auto material = make_transaction_material("writer-key", "chain-signer-local-key");
   const auto chain = test_digest("chain-signer-local-chain");
   auto harness = signer_harness{plugin_options(exact_config(material, chain, true), material.provider)};
   const auto value = allowed_transaction(chain);

   const auto prepared = forge::asio::blocking::run(harness.runtime(), harness.transactions()->sign(value, {}));

   check_prepared_transaction(value, prepared, material.public_key);
}

BOOST_AUTO_TEST_CASE(block_signing_orders_multiple_keys_and_uses_block_id) {
   auto first = make_transaction_material("first-block-key", "chain-signer-block-first");
   auto second = make_transaction_material("second-block-key", "chain-signer-block-second");
   const auto chain = test_digest("chain-signer-block-chain");
   auto options = plugin_options({}, first.provider);
   options.providers = {{.name = "key-0", .value = first.provider},
                        {.name = "key-1", .value = second.provider}};
   options.initial_config.block_profiles.push_back(exact_block_profile(chain, {first, second}, true));
   auto audit = std::make_shared<collecting_audit>();
   options.audit = audit;
   auto harness = signer_harness{std::move(options)};
   auto header = allowed_block_header();
   auto request = protocol::block_sign_request{
       .chain = chain,
       .header = header,
       .keys = {second.public_key, first.public_key},
   };

   const auto signatures = forge::asio::blocking::run(harness.runtime(), harness.blocks()->sign(request, {}));
   BOOST_REQUIRE_EQUAL(signatures.size(), 2U);
   const auto block_id = protocol::calculate_block_id(header);
   BOOST_TEST(asymmetric::recover(signatures[0], block_id) == second.public_key);
   BOOST_TEST(asymmetric::recover(signatures[1], block_id) == first.public_key);
   auto signed_block = protocol::signed_block{};
   static_cast<protocol::block_header&>(signed_block) = header;
   signed_block.producer_signature = signatures[0];
   auto admission = savanna::prepared_admission{
       .id = block_id,
       .block = {.additional_signatures = {signatures[1]}},
       .producer = {
           .producer_name = header.producer,
           .authority = protocol::block_signing_authority_v0{
               .threshold = 3,
               .keys = {{.key = second.public_key, .weight = 2},
                        {.key = first.public_key, .weight = 1}},
           },
       },
   };
   savanna::verify_signature(signed_block, admission);
   admission.block.additional_signatures.clear();
   BOOST_CHECK_THROW(savanna::verify_signature(signed_block, admission), savanna::exceptions::invalid_header);
   const auto entries = audit->entries();
   BOOST_REQUIRE_EQUAL(entries.size(), 1U);
   BOOST_CHECK(entries[0].operation == chain_signer::audit_operation::block);
   BOOST_TEST(entries[0].block == block_id);
   BOOST_CHECK(entries[0].producer == header.producer);
   BOOST_TEST(entries[0].key_count == 2U);
}

BOOST_AUTO_TEST_CASE(block_profile_denies_unlisted_duplicate_and_oversize_requests_before_crypto) {
   auto material = make_transaction_material("block-key", "chain-signer-block-policy");
   auto counted = std::make_shared<counting_provider>(material.provider);
   const auto chain = test_digest("chain-signer-block-policy-chain");
   auto options = plugin_options({}, counted);
   options.providers = {{.name = "key-0", .value = counted}};
   auto profile = exact_block_profile(chain, {material}, true);
   profile.max_header_bytes = forge::raw::pack_size(allowed_block_header());
   options.initial_config.block_profiles.push_back(profile);
   auto harness = signer_harness{std::move(options)};
   auto request = protocol::block_sign_request{.chain = chain,
                                                .header = allowed_block_header(),
                                                .keys = {material.public_key}};
   BOOST_REQUIRE_EQUAL(forge::asio::blocking::run(harness.runtime(), harness.blocks()->sign(request, {})).size(), 1U);
   const auto baseline_describes = counted->describes.load();
   const auto baseline_signs = counted->signs.load();
   request.keys.push_back(material.public_key);
   BOOST_CHECK_THROW(forge::asio::blocking::run(harness.runtime(), harness.blocks()->sign(request, {})),
                     chain_api::exceptions::authorization_denied);
   request.keys.pop_back();
   request.header.header_extensions.push_back({});
   BOOST_CHECK_THROW(forge::asio::blocking::run(harness.runtime(), harness.blocks()->sign(request, {})),
                     chain_api::exceptions::authorization_denied);
   BOOST_TEST(counted->describes.load() == baseline_describes);
   BOOST_TEST(counted->signs.load() == baseline_signs);
}

BOOST_AUTO_TEST_CASE(block_provider_metadata_and_later_signature_fail_without_partial_response) {
   auto first = make_transaction_material("first-key", "chain-signer-block-mismatch-first");
   auto second = make_transaction_material("second-key", "chain-signer-block-mismatch-second");
   auto replacement = make_transaction_material("second-key", "chain-signer-block-mismatch-replacement");
   const auto chain = test_digest("chain-signer-block-mismatch-chain");
   auto request = protocol::block_sign_request{
       .chain = chain,
       .header = allowed_block_header(),
       .keys = {first.public_key, second.public_key},
   };

   auto bad_identity = std::make_shared<mismatched_identity_provider>(second.provider, replacement.public_key);
   auto metadata_first = std::make_shared<counting_provider>(first.provider);
   auto metadata_options = plugin_options({}, metadata_first);
   metadata_options.providers = {{.name = "key-0", .value = metadata_first},
                                 {.name = "key-1", .value = bad_identity}};
   metadata_options.initial_config.block_profiles.push_back(exact_block_profile(chain, {first, second}, true));
   auto metadata_harness = signer_harness{std::move(metadata_options)};
   BOOST_CHECK_THROW(forge::asio::blocking::run(metadata_harness.runtime(), metadata_harness.blocks()->sign(request, {})),
                     chain_api::exceptions::signing_failed);
   BOOST_TEST(metadata_first->signs.load() == 0U);
   BOOST_TEST(bad_identity->signs.load() == 0U);

   auto first_counted = std::make_shared<counting_provider>(first.provider);
   auto bad_signature = std::make_shared<mismatched_signature_provider>(second.provider, replacement.provider,
                                                                        second.public_key);
   auto signature_options = plugin_options({}, first_counted);
   signature_options.providers = {{.name = "key-0", .value = first_counted},
                                  {.name = "key-1", .value = bad_signature}};
   signature_options.initial_config.block_profiles.push_back(exact_block_profile(chain, {first, second}, true));
   auto signature_harness = signer_harness{std::move(signature_options)};
   BOOST_CHECK_THROW(forge::asio::blocking::run(signature_harness.runtime(), signature_harness.blocks()->sign(request, {})),
                     chain_api::exceptions::signing_failed);
   BOOST_TEST(first_counted->signs.load() == 1U);
}

BOOST_AUTO_TEST_CASE(block_configuration_rejects_cross_chain_producer_key_reuse) {
   auto material = make_transaction_material("block-key", "chain-signer-block-reuse");
   auto first = test_digest("chain-signer-block-reuse-first");
   auto second = test_digest("chain-signer-block-reuse-second");
   auto options = plugin_options({}, material.provider);
   options.providers = {{.name = "key-0", .value = material.provider}};
   options.initial_config.block_profiles.push_back(exact_block_profile(first, {material}, true));
   auto duplicate = exact_block_profile(second, {material}, true);
   duplicate.name = "other-chain";
   duplicate.producer = "otherwriter";
   options.initial_config.block_profiles.push_back(std::move(duplicate));
   BOOST_CHECK_THROW(static_cast<void>(signer_harness{std::move(options)}),
                     chain_signer::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(block_header_limit_accepts_configured_nondefault_and_rejects_invalid_limits) {
   auto material = make_transaction_material("block-key", "chain-signer-block-limit");
   const auto chain = test_digest("chain-signer-block-limit-chain");
   auto options = plugin_options({}, material.provider);
   options.providers = {{.name = "key-0", .value = material.provider}};
   auto profile = exact_block_profile(chain, {material}, true);
   profile.max_header_bytes = 2U * 1024U * 1024U;
   options.initial_config.block_profiles.push_back(profile);
   auto harness = signer_harness{options};
   auto request = protocol::block_sign_request{.chain = chain,
                                                .header = allowed_block_header(),
                                                .keys = {material.public_key}};
   BOOST_REQUIRE_EQUAL(forge::asio::blocking::run(harness.runtime(), harness.blocks()->sign(request, {})).size(), 1U);
   options.initial_config.block_profiles.front().max_header_bytes = 0U;
   BOOST_CHECK_THROW(static_cast<void>(signer_harness{options}), chain_signer::exceptions::invalid_config);
   options.initial_config.block_profiles.front().max_header_bytes = 64U * 1024U * 1024U + 1U;
   BOOST_CHECK_THROW(static_cast<void>(signer_harness{options}), chain_signer::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(block_policy_denies_nonexact_dimensions_without_provider_calls) {
   auto material = make_transaction_material("block-key", "chain-signer-block-nonexact");
   auto unlisted = make_transaction_material("unlisted", "chain-signer-block-unlisted");
   const auto chain = test_digest("chain-signer-block-nonexact-chain");
   auto counted = std::make_shared<counting_provider>(material.provider);
   auto options = plugin_options({}, counted);
   options.providers = {{.name = "key-0", .value = counted}};
   options.initial_config.block_profiles.push_back(exact_block_profile(chain, {material}, true));
   auto harness = signer_harness{options};
   const auto baseline = protocol::block_sign_request{.chain = chain,
                                                       .header = allowed_block_header(),
                                                       .keys = {material.public_key}};
   const auto denied = [&](auto mutate) {
      auto request = baseline;
      mutate(request);
      BOOST_CHECK_THROW(forge::asio::blocking::run(harness.runtime(), harness.blocks()->sign(request, {})),
                        chain_api::exceptions::authorization_denied);
   };
   denied([](auto& request) { request.chain = test_digest("wrong-block-chain"); });
   denied([](auto& request) { request.header.producer = protocol::account_name{"otherwriter"}; });
   denied([](auto& request) { request.keys.clear(); });
   denied([&](auto& request) { request.keys = std::vector<protocol::public_key>(65U, unlisted.public_key); });
   denied([&](auto& request) { request.keys = {unlisted.public_key}; });
   BOOST_TEST(counted->describes.load() == 0U);
   BOOST_TEST(counted->signs.load() == 0U);

   options.initial_config.block_profiles.front().allow_local = false;
   auto remote_only = signer_harness{options};
   BOOST_CHECK_THROW(forge::asio::blocking::run(remote_only.runtime(), remote_only.blocks()->sign(baseline, {})),
                     chain_api::exceptions::authorization_denied);
   check_authorization_error(dispatch_block(remote_only, baseline, {},
                                             {forge::api::auth::caller_source::p2p_peer, test_digest("unlisted-caller")},
                                             451U));
   BOOST_TEST(counted->describes.load() == 0U);
   BOOST_TEST(counted->signs.load() == 0U);
}

BOOST_AUTO_TEST_CASE(block_config_rejects_ambiguous_profiles_and_unknown_binding) {
   auto material = make_transaction_material("block-key", "chain-signer-block-ambiguous");
   const auto chain = test_digest("chain-signer-block-ambiguous-chain");
   auto options = plugin_options({}, material.provider);
   options.providers = {{.name = "key-0", .value = material.provider}};
   options.initial_config.block_profiles.push_back(exact_block_profile(chain, {material}, true));
   auto duplicate = exact_block_profile(chain, {material}, true);
   duplicate.name = "different-name";
   options.initial_config.block_profiles.push_back(duplicate);
   BOOST_CHECK_THROW(static_cast<void>(signer_harness{options}), chain_signer::exceptions::invalid_config);
   options.initial_config.block_profiles.pop_back();
   options.initial_config.block_profiles.front().signing.front().provider = "unknown-provider";
   BOOST_CHECK_THROW(static_cast<void>(signer_harness{options}), chain_signer::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(block_remote_dispatch_uses_only_trusted_caller) {
   auto material = make_transaction_material("block-key", "chain-signer-block-remote");
   const auto chain = test_digest("chain-signer-block-remote-chain");
   const auto permitted = test_digest("chain-signer-block-permitted");
   const auto impostor = test_digest("chain-signer-block-impostor");
   auto options = plugin_options({}, material.provider);
   options.providers = {{.name = "key-0", .value = material.provider}};
   options.initial_config.block_profiles.push_back(exact_block_profile(
       chain, {material}, false,
       {{.source = forge::api::auth::caller_source::p2p_peer, .fingerprint = permitted.str()}}));
   auto harness = signer_harness{std::move(options)};
   const auto request = protocol::block_sign_request{.chain = chain,
                                                      .header = allowed_block_header(),
                                                      .keys = {material.public_key}};
   const auto accepted = dispatch_block(harness, request,
                                        {forge::api::auth::caller_source::p2p_peer, impostor},
                                        {forge::api::auth::caller_source::p2p_peer, permitted}, 401U);
   BOOST_REQUIRE(accepted.kind == forge::api::core::frame_kind::response);
   const auto signatures = forge::api::core::unpack_body<std::vector<protocol::signature>>(accepted.payload);
   BOOST_REQUIRE_EQUAL(signatures.size(), 1U);
   BOOST_TEST(asymmetric::recover(signatures.front(), protocol::calculate_block_id(request.header)) ==
              material.public_key);
   check_authorization_error(dispatch_block(harness, request,
                                             {forge::api::auth::caller_source::p2p_peer, permitted},
                                             {forge::api::auth::caller_source::p2p_peer, impostor}, 402U));
}

BOOST_AUTO_TEST_CASE(block_signing_preserves_unavailable_after_plugin_stop) {
   auto material = make_transaction_material("block-key", "chain-signer-block-stopped");
   const auto chain = test_digest("chain-signer-block-stopped-chain");
   auto options = plugin_options({}, material.provider);
   options.providers = {{.name = "key-0", .value = material.provider}};
   options.initial_config.block_profiles.push_back(exact_block_profile(chain, {material}, true));
   auto harness = signer_harness{std::move(options)};
   harness.plugin().request_stop();
   const auto request = protocol::block_sign_request{.chain = chain,
                                                      .header = allowed_block_header(),
                                                      .keys = {material.public_key}};
   BOOST_CHECK_THROW(forge::asio::blocking::run(harness.runtime(), harness.blocks()->sign(request, {})),
                     chain_api::exceptions::unavailable);
}

BOOST_AUTO_TEST_CASE(transaction_policy_enforces_the_signed_packed_size) {
   const auto material = make_transaction_material("writer-key", "chain-signer-packed-limit-key");
   const auto chain = test_digest("chain-signer-packed-limit-chain");
   const auto value = allowed_transaction(chain);
   auto baseline = signer_harness{plugin_options(exact_config(material, chain, true), material.provider)};
   const auto prepared = forge::asio::blocking::run(baseline.runtime(), baseline.transactions()->sign(value, {}));
   const auto unsigned_size = forge::raw::pack(value).size();
   const auto packed_size = forge::raw::pack(prepared.packed).size();
   BOOST_REQUIRE_GT(packed_size, unsigned_size);

   auto config = exact_config(material, chain, true);
   config.transaction_profiles.front().max_packed_bytes = packed_size - 1U;
   auto limited = signer_harness{plugin_options(std::move(config), material.provider)};

   BOOST_CHECK_THROW(forge::asio::blocking::run(limited.runtime(), limited.transactions()->sign(value, {})),
                     chain_api::exceptions::authorization_denied);
}

BOOST_AUTO_TEST_CASE(transaction_policy_applies_packed_limit_after_selected_zlib_compression) {
   const auto material = make_transaction_material("writer-key", "chain-signer-zlib-limit-key");
   const auto chain = test_digest("chain-signer-zlib-limit-chain");
   auto value = allowed_transaction(chain);
   value.value.actions.front().data.resize(32U * 1024U, 0x41U);
   value.compression = protocol::packed_transaction::compression::zlib;

   auto baseline_config = exact_config(material, chain, true);
   baseline_config.transaction_profiles.front().max_packed_bytes = 64U * 1024U;
   auto baseline = signer_harness{plugin_options(std::move(baseline_config), material.provider)};
   const auto prepared = forge::asio::blocking::run(baseline.runtime(), baseline.transactions()->sign(value, {}));
   const auto unsigned_size = forge::raw::pack(value).size();
   const auto packed_size = forge::raw::pack(prepared.packed).size();
   BOOST_REQUIRE_LT(packed_size, unsigned_size);

   auto limited_config = exact_config(material, chain, true);
   limited_config.transaction_profiles.front().max_packed_bytes = packed_size;
   auto limited = signer_harness{plugin_options(std::move(limited_config), material.provider)};

   const auto result = forge::asio::blocking::run(limited.runtime(), limited.transactions()->sign(value, {}));
   check_prepared_transaction(value, result, material.public_key);
}

BOOST_AUTO_TEST_CASE(trusted_dispatch_replaces_spoofed_caller_and_audits_only_safe_identity) {
   const auto material = make_transaction_material("writer-key", "chain-signer-remote-key");
   const auto chain = test_digest("chain-signer-remote-chain");
   const auto allowed_fingerprint = test_digest("allowed-caller");
   const auto spoofed_fingerprint = test_digest("spoofed-caller");
   const auto wrong_fingerprint = test_digest("wrong-caller");
   auto audit = std::make_shared<collecting_audit>();
   auto config = exact_config(material, chain, false,
                              {{
                                  .source = forge::api::auth::caller_source::p2p_peer,
                                  .fingerprint = allowed_fingerprint.str(),
                              }});
   auto semantic_authorizer = std::make_shared<payload_semantic_authorizer>(allowed_action().data);
   auto harness = signer_harness{plugin_options(std::move(config), material.provider, audit, {}, semantic_authorizer)};
   const auto value = allowed_transaction(chain);

   BOOST_CHECK_THROW(
       forge::asio::blocking::run(
           harness.runtime(),
           harness.transactions()->sign(value, {forge::api::auth::caller_source::p2p_peer, allowed_fingerprint})),
       chain_api::exceptions::authorization_denied);

   const auto response =
       dispatch_transaction(harness, value, {forge::api::auth::caller_source::p2p_peer, spoofed_fingerprint},
                            {forge::api::auth::caller_source::p2p_peer, allowed_fingerprint}, 101U);
   BOOST_REQUIRE(response.kind == forge::api::core::frame_kind::response);
   const auto prepared = forge::api::core::unpack_body<transaction::prepared_transaction>(response.payload);
   check_prepared_transaction(value, prepared, material.public_key);

   check_authorization_error(
       dispatch_transaction(harness, value, {forge::api::auth::caller_source::p2p_peer, allowed_fingerprint},
                            {forge::api::auth::caller_source::p2p_peer, wrong_fingerprint}, 102U));

   const auto entries = audit->entries();
   BOOST_REQUIRE_EQUAL(entries.size(), 3U);
   BOOST_CHECK(entries[0].decision == chain_signer::audit_decision::denied);
   BOOST_TEST(entries[0].local);
   BOOST_CHECK(entries[1].decision == chain_signer::audit_decision::allowed);
   BOOST_TEST(!entries[1].local);
   BOOST_TEST(entries[1].profile == "storage-writer");
   BOOST_CHECK(entries[1].caller_source == forge::api::auth::caller_source::p2p_peer);
   BOOST_TEST(entries[1].caller_fingerprint == allowed_fingerprint);
   BOOST_TEST(entries[1].caller_fingerprint != spoofed_fingerprint);
   BOOST_TEST(entries[1].chain == chain);
   BOOST_TEST(entries[1].transaction == value.value.id());
   BOOST_CHECK(entries[2].decision == chain_signer::audit_decision::denied);
   BOOST_TEST(semantic_authorizer->calls() == 1U);
}

BOOST_AUTO_TEST_CASE(remote_payloads_require_an_injected_semantic_authorizer) {
   const auto material = make_transaction_material("writer-key", "chain-signer-semantic-key");
   const auto chain = test_digest("chain-signer-semantic-chain");
   const auto caller = test_digest("chain-signer-semantic-caller");
   const auto callers = std::vector<chain_signer::caller_rule>{{
       .source = forge::api::auth::caller_source::p2p_peer,
       .fingerprint = caller.str(),
   }};

   auto missing_authorizer = plugin_options(exact_config(material, chain, false, callers), material.provider);
   BOOST_CHECK_THROW(static_cast<void>(signer_harness{std::move(missing_authorizer)}),
                     chain_signer::exceptions::invalid_config);

   auto authorizer = std::make_shared<payload_semantic_authorizer>(allowed_action().data);
   auto harness = signer_harness{
       plugin_options(exact_config(material, chain, false, callers), material.provider, {}, {}, authorizer)};
   const auto approved = allowed_transaction(chain);

   const auto response = dispatch_transaction(
       harness, approved, {forge::api::auth::caller_source::p2p_peer, test_digest("untrusted-wire-caller")},
       {forge::api::auth::caller_source::p2p_peer, caller}, 201U);
   BOOST_REQUIRE(response.kind == forge::api::core::frame_kind::response);

   auto rejected = approved;
   rejected.value.actions.front().data = {0xFFU};
   check_authorization_error(dispatch_transaction(
       harness, rejected, {forge::api::auth::caller_source::p2p_peer, test_digest("untrusted-wire-caller")},
       {forge::api::auth::caller_source::p2p_peer, caller}, 202U));
   BOOST_TEST(authorizer->calls() == 2U);
}

BOOST_AUTO_TEST_CASE(remote_profile_only_authorization_is_explicit_and_keeps_envelope_policy) {
   const auto material = make_transaction_material("writer-key", "chain-signer-profile-only-key");
   const auto chain = test_digest("chain-signer-profile-only-chain");
   const auto fingerprint = test_digest("chain-signer-profile-only-caller");
   auto config = exact_config(material, chain, false,
                              {{.source = forge::api::auth::caller_source::p2p_peer,
                                .fingerprint = fingerprint.str()}});
   config.transaction_profiles.front().authorization = chain_signer::remote_authorization::profile_only;
   auto harness = signer_harness{plugin_options(std::move(config), material.provider)};
   auto value = allowed_transaction(chain);
   auto response = dispatch_transaction(harness, value, {},
                                        {forge::api::auth::caller_source::p2p_peer, fingerprint}, 301U);
   BOOST_REQUIRE(response.kind == forge::api::core::frame_kind::response);
   check_prepared_transaction(value,
                              forge::api::core::unpack_body<transaction::prepared_transaction>(response.payload),
                              material.public_key);
   value.value.actions.front().account = protocol::account_name{"unlisted"};
   check_authorization_error(dispatch_transaction(
       harness, value, {}, {forge::api::auth::caller_source::p2p_peer, fingerprint}, 302U));
}

BOOST_AUTO_TEST_CASE(remote_authorization_unknown_enum_cannot_downgrade_semantic_gate) {
   const auto material = make_transaction_material("writer-key", "chain-signer-unknown-auth-key");
   const auto chain = test_digest("chain-signer-unknown-auth-chain");
   auto config = exact_config(material, chain, false,
                              {{.source = forge::api::auth::caller_source::p2p_peer,
                                .fingerprint = test_digest("caller").str()}});
   config.transaction_profiles.front().authorization = static_cast<chain_signer::remote_authorization>(255);
   BOOST_CHECK_THROW(static_cast<void>(signer_harness{plugin_options(std::move(config), material.provider)}),
                     chain_signer::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(transaction_packed_limit_preflight_rejects_without_provider_calls_for_none_and_zlib) {
   for (const auto compression : {protocol::packed_transaction::compression::none,
                                  protocol::packed_transaction::compression::zlib}) {
      const auto material = make_transaction_material("writer-key", "chain-signer-preflight-key");
      auto counted = std::make_shared<counting_provider>(material.provider);
      const auto chain = test_digest("chain-signer-preflight-chain");
      auto value = allowed_transaction(chain);
      value.compression = compression;
      if (compression == protocol::packed_transaction::compression::zlib) {
         value.value.actions.front().data.resize(4096U, 0x41U);
      }
      auto baseline_config = exact_config(material, chain, true);
      baseline_config.transaction_profiles.front().max_packed_bytes = 1024U * 1024U;
      auto baseline = signer_harness{plugin_options(std::move(baseline_config), material.provider)};
      const auto prepared = forge::asio::blocking::run(baseline.runtime(), baseline.transactions()->sign(value, {}));
      const auto exact = forge::raw::pack_size(prepared.packed);
      BOOST_REQUIRE_GT(exact, 1U);

      auto config = exact_config(material, chain, true);
      config.transaction_profiles.front().max_packed_bytes = exact - 1U;
      auto limited = signer_harness{plugin_options(std::move(config), counted)};
      BOOST_CHECK_THROW(forge::asio::blocking::run(limited.runtime(), limited.transactions()->sign(value, {})),
                        chain_api::exceptions::authorization_denied);
      BOOST_TEST(counted->describes.load() == 0U);
      BOOST_TEST(counted->signs.load() == 0U);

      auto exact_config_value = exact_config(material, chain, true);
      exact_config_value.transaction_profiles.front().max_packed_bytes = exact;
      auto equal = signer_harness{plugin_options(std::move(exact_config_value), counted)};
      BOOST_REQUIRE_EQUAL(forge::raw::pack_size(
                              forge::asio::blocking::run(equal.runtime(), equal.transactions()->sign(value, {})).packed),
                          exact);
      BOOST_TEST(counted->signs.load() == 1U);
   }
}

BOOST_AUTO_TEST_CASE(transaction_policy_denies_every_non_exact_dimension) {
   const auto material = make_transaction_material("writer-key", "chain-signer-policy-key");
   const auto chain = test_digest("chain-signer-policy-chain");
   auto harness = signer_harness{plugin_options(exact_config(material, chain, true), material.provider)};
   const auto baseline = allowed_transaction(chain);

   check_policy_denied(harness, baseline, "wrong chain", [](auto& value) { value.chain = test_digest("other-chain"); });
   check_policy_denied(harness, baseline, "wrong action",
                       [](auto& value) { value.value.actions.front().name = protocol::action_name{"read"}; });
   check_policy_denied(harness, baseline, "wrong permission", [](auto& value) {
      value.value.actions.front().authorization.front().permission = protocol::permission_name{"owner"};
   });
   check_policy_denied(harness, baseline, "expiration outside profile window",
                       [](auto& value) { value.value.expiration = protocol::time_point_sec{now_seconds + 121U}; });
   check_policy_denied(harness, baseline, "deferred transaction is denied by default",
                       [](auto& value) { value.value.delay_sec = 1U; });
   check_policy_denied(harness, baseline, "context-free data",
                       [](auto& value) { value.context_free_data.push_back({0xCAU, 0xFEU}); });
   check_policy_denied(harness, baseline, "context-free action", [](auto& value) {
      auto action = protocol::action{};
      action.account = protocol::account_name{"storage"};
      action.name = protocol::action_name{"inspect"};
      action.data = {0x01U};
      value.value.context_free_actions.push_back(std::move(action));
   });
   check_policy_denied(harness, baseline, "transaction extension", [](auto& value) {
      value.value.transaction_extensions.emplace_back(std::uint16_t{42U}, protocol::bytes{0x01U});
   });
}

BOOST_AUTO_TEST_CASE(transaction_policy_allows_only_explicitly_bounded_delay) {
   const auto material = make_transaction_material("writer-key", "chain-signer-delay-key");
   const auto chain = test_digest("chain-signer-delay-chain");
   auto config = exact_config(material, chain, true);
   config.transaction_profiles.front().max_delay_seconds = 10U;
   auto harness = signer_harness{plugin_options(std::move(config), material.provider)};

   auto allowed = allowed_transaction(chain);
   allowed.value.delay_sec = 10U;
   const auto prepared = forge::asio::blocking::run(harness.runtime(), harness.transactions()->sign(allowed, {}));
   check_prepared_transaction(allowed, prepared, material.public_key);

   check_policy_denied(harness, allowed, "delay exceeds profile limit",
                       [](auto& value) { value.value.delay_sec = 11U; });
}

BOOST_AUTO_TEST_CASE(overlapping_profiles_fail_closed) {
   const auto material = make_transaction_material("writer-key", "chain-signer-overlap-key");
   const auto chain = test_digest("chain-signer-overlap-chain");
   auto first = exact_profile(material, chain, true);
   auto second = first;
   second.name = "storage-writer-overlap";
   auto config = chain_signer::config{
       .transaction_profiles = {std::move(first), std::move(second)},
       .max_inflight = 2,
       .max_queued = 2,
   };
   auto harness = signer_harness{plugin_options(std::move(config), material.provider)};

   BOOST_CHECK_THROW(
       forge::asio::blocking::run(harness.runtime(), harness.transactions()->sign(allowed_transaction(chain), {})),
       chain_api::exceptions::authorization_denied);
}

BOOST_AUTO_TEST_CASE(provider_key_and_signature_mismatches_fail_signing) {
   const auto expected = make_transaction_material("writer-key", "chain-signer-expected-key");
   const auto other = make_transaction_material("writer-key", "chain-signer-other-key");
   const auto chain = test_digest("chain-signer-mismatch-chain");

   auto key_mismatch_config = exact_config(expected, chain, true);
   key_mismatch_config.transaction_profiles.front().signing.expected_public_key =
       asymmetric::encoding::forge().format(other.public_key);
   {
      auto harness = signer_harness{plugin_options(std::move(key_mismatch_config), expected.provider)};
      BOOST_CHECK_THROW(
          forge::asio::blocking::run(harness.runtime(), harness.transactions()->sign(allowed_transaction(chain), {})),
          chain_api::exceptions::signing_failed);
   }

   auto malformed =
       std::make_shared<mismatched_signature_provider>(expected.provider, other.provider, expected.public_key);
   auto harness = signer_harness{plugin_options(exact_config(expected, chain, true), std::move(malformed))};
   BOOST_CHECK_THROW(
       forge::asio::blocking::run(harness.runtime(), harness.transactions()->sign(allowed_transaction(chain), {})),
       chain_api::exceptions::signing_failed);
}

BOOST_AUTO_TEST_CASE(finality_identity_pop_and_typed_vote_are_valid) {
   const auto tx_material = make_transaction_material("writer-key", "chain-signer-finality-transaction-key");
   const auto finality_material = make_finality_material(17U);
   const auto chain = test_digest("chain-signer-finality-chain");
   auto config = exact_config(tx_material, chain, true);
   config.finality = chain_signer::finality_binding{
       .provider = "finality",
       .expected_public_key = bls::encoding::format(finality_material.public_key),
   };
   auto harness =
       signer_harness{plugin_options(std::move(config), tx_material.provider, {}, finality_material.provider)};

   const auto identity = forge::asio::blocking::run(harness.runtime(), harness.finality()->identity());
   BOOST_CHECK(identity.public_key == finality_material.public_key);
   BOOST_TEST(static_cast<bool>(bls::verify_proof_of_possession(identity.public_key, identity.proof_of_possession)));

   const auto block = savanna::block_ref{
       .num = 9U,
       .id = test_digest("finality-block-id"),
       .slot = 12U,
       .finality_digest = test_digest("finality-vote-digest"),
       .active_policy_generation = 3U,
   };
   const auto vote =
       forge::asio::blocking::run(harness.runtime(), harness.finality()->sign_vote(block, savanna::vote_kind::weak));
   BOOST_TEST(vote.block == block.id);
   BOOST_CHECK(vote.finalizer == finality_material.public_key);
   BOOST_CHECK(vote.kind == savanna::vote_kind::weak);
   BOOST_TEST(bls::verify(vote.finalizer, savanna::message_for_vote(block.finality_digest, vote.kind), vote.signature));
}

BOOST_AUTO_TEST_CASE(admission_is_bounded_cancelable_and_drains_active_signing_on_stop) {
   const auto material = make_transaction_material("writer-key", "chain-signer-admission-key");
   const auto chain = test_digest("chain-signer-admission-chain");
   auto config = exact_config(material, chain, true);
   config.max_inflight = 1;
   config.max_queued = 1;
   auto gated = std::make_shared<gated_transaction_provider>(material.provider);
   auto harness = signer_harness{plugin_options(std::move(config), gated)};
   auto release_on_exit = provider_release_guard{gated};
   const auto value = allowed_transaction(chain);

   auto active = boost::asio::co_spawn(harness.runtime().context(), harness.transactions()->sign(value, {}),
                                       boost::asio::use_future);
   BOOST_REQUIRE(gated->wait_for_entered(1U));

   auto cancellation = boost::asio::cancellation_signal{};
   auto queued =
       boost::asio::co_spawn(harness.runtime().context(), harness.transactions()->sign(value, {}),
                             boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
   flush_runtime(harness.runtime());
   BOOST_TEST(static_cast<int>(queued.wait_for(std::chrono::milliseconds{0})) ==
              static_cast<int>(std::future_status::timeout));
   BOOST_CHECK_THROW(forge::asio::blocking::run(harness.runtime(), harness.transactions()->sign(value, {})),
                     chain_api::exceptions::resource_exhausted);

   cancellation.emit(boost::asio::cancellation_type::all);
   BOOST_CHECK_THROW(static_cast<void>(queued.get()), forge::api::core::exceptions::cancelled);

   auto stop_queued = boost::asio::co_spawn(harness.runtime().context(), harness.transactions()->sign(value, {}),
                                            boost::asio::use_future);
   flush_runtime(harness.runtime());
   harness.plugin().request_stop();
   BOOST_CHECK_THROW(static_cast<void>(stop_queued.get()), forge::api::core::exceptions::cancelled);
   BOOST_CHECK_THROW(forge::asio::blocking::run(harness.runtime(), harness.transactions()->sign(value, {})),
                     chain_api::exceptions::unavailable);

   auto shutdown =
       boost::asio::co_spawn(harness.runtime().context(), harness.plugin().shutdown(), boost::asio::use_future);
   BOOST_REQUIRE(active.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(active.get()), forge::api::core::exceptions::cancelled);
   BOOST_CHECK_NO_THROW(shutdown.get());
}

BOOST_AUTO_TEST_CASE(block_transaction_and_finality_share_admission_and_cancellation) {
   auto material = make_transaction_material("writer-key", "chain-signer-mixed-admission-key");
   auto finality_material = make_finality_material(71U);
   const auto chain = test_digest("chain-signer-mixed-admission-chain");
   auto config = exact_config(material, chain, true);
   config.block_profiles.push_back(exact_block_profile(chain, {material}, true));
   config.block_profiles.front().signing.front().provider = "transaction";
   config.finality = chain_signer::finality_binding{
       .provider = "finality",
       .expected_public_key = bls::encoding::format(finality_material.public_key),
   };
   config.max_inflight = 1;
   config.max_queued = 1;
   auto gated = std::make_shared<gated_transaction_provider>(material.provider);
   auto harness = signer_harness{plugin_options(std::move(config), gated, {}, finality_material.provider)};
   auto release_on_exit = provider_release_guard{gated};
   const auto block = protocol::block_sign_request{.chain = chain,
                                                    .header = allowed_block_header(),
                                                    .keys = {material.public_key}};
   auto active = boost::asio::co_spawn(harness.runtime().context(), harness.blocks()->sign(block, {}),
                                       boost::asio::use_future);
   BOOST_REQUIRE(gated->wait_for_entered(1U));
   auto cancellation = boost::asio::cancellation_signal{};
   auto queued = boost::asio::co_spawn(
       harness.runtime().context(), harness.transactions()->sign(allowed_transaction(chain), {}),
       boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
   flush_runtime(harness.runtime());
   BOOST_CHECK_THROW(forge::asio::blocking::run(harness.runtime(), harness.finality()->identity()),
                     chain_api::exceptions::resource_exhausted);
   cancellation.emit(boost::asio::cancellation_type::all);
   BOOST_CHECK_THROW(static_cast<void>(queued.get()), forge::api::core::exceptions::cancelled);
   harness.plugin().request_stop();
   BOOST_CHECK_THROW(static_cast<void>(active.get()), forge::api::core::exceptions::cancelled);
   BOOST_CHECK_THROW(forge::asio::blocking::run(harness.runtime(), harness.finality()->identity()),
                     chain_api::exceptions::unavailable);
}

BOOST_AUTO_TEST_CASE(admission_rejects_unknown_callers_before_a_saturated_queue) {
   const auto material = make_transaction_material("writer-key", "chain-signer-pre-admission-key");
   const auto chain = test_digest("chain-signer-pre-admission-chain");
   const auto allowed_fingerprint = test_digest("chain-signer-pre-admission-allowed");
   const auto denied_fingerprint = test_digest("chain-signer-pre-admission-denied");
   auto config = exact_config(material, chain, true,
                              {{
                                  .source = forge::api::auth::caller_source::p2p_peer,
                                  .fingerprint = allowed_fingerprint.str(),
                              }});
   config.max_inflight = 1;
   config.max_queued = 0;
   auto gated = std::make_shared<gated_transaction_provider>(material.provider);
   auto authorizer = std::make_shared<payload_semantic_authorizer>(allowed_action().data);
   auto harness = signer_harness{plugin_options(std::move(config), gated, {}, {}, authorizer)};
   auto release_on_exit = provider_release_guard{gated};
   const auto value = allowed_transaction(chain);

   auto active = boost::asio::co_spawn(harness.runtime().context(), harness.transactions()->sign(value, {}),
                                       boost::asio::use_future);
   BOOST_REQUIRE(gated->wait_for_entered(1U));

   check_authorization_error(
       dispatch_transaction(harness, value, {forge::api::auth::caller_source::p2p_peer, allowed_fingerprint},
                            {forge::api::auth::caller_source::p2p_peer, denied_fingerprint}, 301U));

   gated->release();
   check_prepared_transaction(value, active.get(), material.public_key);
}

BOOST_AUTO_TEST_CASE(admission_enforces_queued_byte_and_per_caller_budgets) {
   const auto material = make_transaction_material("writer-key", "chain-signer-admission-budget-key");
   const auto chain = test_digest("chain-signer-admission-budget-chain");
   const auto value = allowed_transaction(chain);

   {
      auto config = exact_config(material, chain, true);
      config.max_inflight = 1;
      config.max_queued = 2;
      config.max_queued_bytes = 1;
      config.max_per_caller = 2;
      auto gated = std::make_shared<gated_transaction_provider>(material.provider);
      auto harness = signer_harness{plugin_options(std::move(config), gated)};
      auto release_on_exit = provider_release_guard{gated};
      auto active = boost::asio::co_spawn(harness.runtime().context(), harness.transactions()->sign(value, {}),
                                          boost::asio::use_future);
      BOOST_REQUIRE(gated->wait_for_entered(1U));

      BOOST_CHECK_THROW(forge::asio::blocking::run(harness.runtime(), harness.transactions()->sign(value, {})),
                        chain_api::exceptions::resource_exhausted);
      gated->release();
      check_prepared_transaction(value, active.get(), material.public_key);
   }

   {
      auto config = exact_config(material, chain, true);
      config.max_inflight = 1;
      config.max_queued = 2;
      config.max_per_caller = 1;
      auto gated = std::make_shared<gated_transaction_provider>(material.provider);
      auto harness = signer_harness{plugin_options(std::move(config), gated)};
      auto release_on_exit = provider_release_guard{gated};
      auto active = boost::asio::co_spawn(harness.runtime().context(), harness.transactions()->sign(value, {}),
                                          boost::asio::use_future);
      BOOST_REQUIRE(gated->wait_for_entered(1U));

      BOOST_CHECK_THROW(forge::asio::blocking::run(harness.runtime(), harness.transactions()->sign(value, {})),
                        chain_api::exceptions::resource_exhausted);
      gated->release();
      check_prepared_transaction(value, active.get(), material.public_key);
   }
}

BOOST_AUTO_TEST_CASE(shutdown_has_a_plugin_owned_deadline_for_non_cancelable_providers) {
   const auto material = make_transaction_material("writer-key", "chain-signer-shutdown-deadline-key");
   const auto chain = test_digest("chain-signer-shutdown-deadline-chain");
   auto config = exact_config(material, chain, true);
   config.max_inflight = 1;
   config.shutdown_timeout_ms = 20;
   auto gated = std::make_shared<gated_transaction_provider>(material.provider, true);
   auto harness = signer_harness{plugin_options(std::move(config), gated)};
   auto release_on_exit = provider_release_guard{gated};
   const auto value = allowed_transaction(chain);

   auto active = boost::asio::co_spawn(harness.runtime().context(), harness.transactions()->sign(value, {}),
                                       boost::asio::use_future);
   BOOST_REQUIRE(gated->wait_for_entered(1U));

   BOOST_CHECK_THROW(forge::asio::blocking::run(harness.runtime(), harness.plugin().shutdown()),
                     chain_signer::exceptions::invalid_lifecycle);
   gated->release();
   BOOST_CHECK_THROW(static_cast<void>(active.get()), forge::api::core::exceptions::cancelled);
}

BOOST_AUTO_TEST_CASE(initialize_and_stop_race_publishes_only_a_closed_admission) {
   const auto material = make_transaction_material("writer-key", "chain-signer-lifecycle-key");
   const auto chain = test_digest("chain-signer-lifecycle-chain");

   for (auto iteration = 0U; iteration < 32U; ++iteration) {
      auto runtime =
          forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1, .thread_name = "chain-signer-race"}};
      auto scheduler = forge::asio::task::scheduler{runtime};
      auto apis = forge::api::core::registry{};
      auto signals = forge::app::signal_bus{};
      auto events = forge::app::event_bus{};
      auto plugin = chain_signer::plugin{plugin_options(exact_config(material, chain, true), material.provider)};
      auto provider = forge::api::core::installer{apis};
      forge::asio::blocking::run(runtime, plugin.provide(provider));
      auto context = forge::app::plugin_context{scheduler, apis, signals, events};

      auto start = std::barrier{2};
      auto stopper = std::thread{[&] {
         start.arrive_and_wait();
         plugin.request_stop();
      }};
      start.arrive_and_wait();
      BOOST_CHECK_NO_THROW(forge::asio::blocking::run(runtime, plugin.initialize(context)));
      stopper.join();

      BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, plugin.startup()),
                        chain_signer::exceptions::invalid_lifecycle);
      const auto transactions = apis.get<chain_api::transaction_signer>(chain_api::transaction_signer::ref());
      BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, transactions->sign(allowed_transaction(chain), {})),
                        chain_api::exceptions::unavailable);
      BOOST_CHECK_NO_THROW(forge::asio::blocking::run(runtime, plugin.shutdown()));
   }
}

BOOST_AUTO_TEST_SUITE_END()
