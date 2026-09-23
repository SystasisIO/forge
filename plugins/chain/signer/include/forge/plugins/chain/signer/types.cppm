module;

#include <boost/describe.hpp>
#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module forge.plugins.chain.signer.types;

export import forge.api.auth.authenticated_caller;
export import forge.chain.protocol.time;
export import forge.chain.protocol.values;
export import forge.chain.transaction.types;
export import forge.crypto.bls.signer.provider;
export import forge.crypto.signer.provider;

import forge.schema.enums;
import forge.schema.object;

export namespace forge::plugins::chain::signer {

struct named_provider {
   std::string name;
   std::shared_ptr<forge::crypto::signer::provider> value;
};

struct named_finality_provider {
   std::string name;
   std::shared_ptr<forge::crypto::bls::signer::provider> value;
};

struct caller_rule {
   forge::api::auth::caller_source source = forge::api::auth::caller_source::p2p_peer;
   std::string fingerprint;
};

struct action_rule {
   std::string account;
   std::string action;
   std::string actor;
   std::string permission;
};

struct context_free_action_rule {
   std::string account;
   std::string action;
};

struct key_binding {
   std::string provider;
   std::string key_id;
   std::string expected_public_key;
};

enum class remote_authorization : std::uint8_t {
   semantic,
   profile_only,
};

// Product composition owns ABI-aware payload authorization; this plugin owns only signing policy.
class semantic_authorization_provider {
 public:
   virtual ~semantic_authorization_provider() = default;

   virtual boost::asio::awaitable<bool>
   authorize_remote(std::string_view profile, const forge::chain::transaction::unsigned_transaction& transaction,
                    const forge::api::auth::authenticated_caller& caller) = 0;
};

struct transaction_profile {
   std::string name;
   std::string chain_id;
   key_binding signing;
   remote_authorization authorization = remote_authorization::semantic;
   bool allow_local = false;
   std::vector<caller_rule> callers;
   std::vector<action_rule> actions;
   std::vector<context_free_action_rule> context_free_actions;
   std::uint32_t max_expiration_seconds = 120;
   std::uint32_t max_delay_seconds = 0;
   std::uint32_t max_actions = 16;
   std::uint64_t max_packed_bytes = 1024U * 1024U;
   bool allow_context_free_data = false;
   bool allow_context_free_actions = false;
   bool allow_extensions = false;
};

struct block_profile {
   std::string name;
   std::string chain_id;
   std::string producer;
   std::vector<key_binding> signing;
   bool allow_local = false;
   std::vector<caller_rule> callers;
   std::uint64_t max_header_bytes = 1024U * 1024U;
};

struct finality_binding {
   std::string provider;
   std::string expected_public_key;
};

struct config {
   std::vector<transaction_profile> transaction_profiles;
   std::vector<block_profile> block_profiles;
   std::optional<finality_binding> finality;
   std::uint64_t max_inflight = 64;
   std::uint64_t max_queued = 256;
   std::uint64_t max_queued_bytes = 64U * 1024U * 1024U;
   std::uint64_t max_per_caller = 64;
   std::uint64_t shutdown_timeout_ms = 5000;
};

enum class audit_decision : std::uint8_t {
   allowed,
   denied,
   failed,
};

enum class audit_operation : std::uint8_t {
   transaction,
   block,
   finality,
};

struct audit_entry {
   audit_operation operation = audit_operation::transaction;
   bool local = false;
   forge::api::auth::caller_source caller_source = forge::api::auth::caller_source::p2p_peer;
   forge::crypto::digest::sha256 caller_fingerprint;
   std::string profile;
   forge::chain::protocol::chain_id chain;
   forge::chain::protocol::transaction_id transaction;
   forge::chain::protocol::block_id block;
   forge::chain::protocol::account_name producer;
   std::uint32_t key_count = 0;
   audit_decision decision = audit_decision::failed;
   std::string error_category;
   std::int32_t error_code = 0;
};

class audit_sink {
 public:
   virtual ~audit_sink() = default;
   virtual void record(const audit_entry& value) noexcept = 0;
};

struct plugin_options {
   std::vector<named_provider> providers;
   std::vector<named_finality_provider> finality_providers;
   std::shared_ptr<semantic_authorization_provider> semantic_authorization;
   config initial_config;
   std::shared_ptr<audit_sink> audit;
   std::function<forge::chain::protocol::time_point_sec()> now;
};

BOOST_DESCRIBE_STRUCT(caller_rule, (), (source, fingerprint))
BOOST_DESCRIBE_STRUCT(action_rule, (), (account, action, actor, permission))
BOOST_DESCRIBE_STRUCT(context_free_action_rule, (), (account, action))
BOOST_DESCRIBE_STRUCT(key_binding, (), (provider, key_id, expected_public_key))
BOOST_DESCRIBE_STRUCT(transaction_profile, (),
                      (name, chain_id, signing, authorization, allow_local, callers, actions, context_free_actions,
                       max_expiration_seconds, max_delay_seconds, max_actions, max_packed_bytes,
                       allow_context_free_data, allow_context_free_actions, allow_extensions))
BOOST_DESCRIBE_STRUCT(block_profile, (), (name, chain_id, producer, signing, allow_local, callers, max_header_bytes))
BOOST_DESCRIBE_STRUCT(finality_binding, (), (provider, expected_public_key))
BOOST_DESCRIBE_STRUCT(config, (),
                      (transaction_profiles, block_profiles, finality, max_inflight, max_queued, max_queued_bytes, max_per_caller,
                       shutdown_timeout_ms))
BOOST_DESCRIBE_ENUM(audit_decision, allowed, denied, failed)
BOOST_DESCRIBE_ENUM(audit_operation, transaction, block, finality)
BOOST_DESCRIBE_ENUM(remote_authorization, semantic, profile_only)

} // namespace forge::plugins::chain::signer

export template <> struct forge::schema::rules<forge::plugins::chain::signer::caller_rule> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::chain::signer::caller_rule> define() {
      auto schema = forge::schema::object<forge::plugins::chain::signer::caller_rule>();
      schema.field<&forge::plugins::chain::signer::caller_rule::source>("source").default_value(
          forge::api::auth::caller_source::p2p_peer);
      schema.field<&forge::plugins::chain::signer::caller_rule::fingerprint>("fingerprint").required().non_empty();
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::chain::signer::action_rule> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::chain::signer::action_rule> define() {
      auto schema = forge::schema::object<forge::plugins::chain::signer::action_rule>();
      schema.field<&forge::plugins::chain::signer::action_rule::account>("account").required().non_empty();
      schema.field<&forge::plugins::chain::signer::action_rule::action>("action").required().non_empty();
      schema.field<&forge::plugins::chain::signer::action_rule::actor>("actor").required().non_empty();
      schema.field<&forge::plugins::chain::signer::action_rule::permission>("permission").required().non_empty();
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::chain::signer::context_free_action_rule> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::chain::signer::context_free_action_rule> define() {
      auto schema = forge::schema::object<forge::plugins::chain::signer::context_free_action_rule>();
      schema.field<&forge::plugins::chain::signer::context_free_action_rule::account>("account").required().non_empty();
      schema.field<&forge::plugins::chain::signer::context_free_action_rule::action>("action").required().non_empty();
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::chain::signer::key_binding> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::chain::signer::key_binding> define() {
      auto schema = forge::schema::object<forge::plugins::chain::signer::key_binding>();
      schema.field<&forge::plugins::chain::signer::key_binding::provider>("provider")
          .required()
          .non_empty();
      schema.field<&forge::plugins::chain::signer::key_binding::key_id>("key-id").required().non_empty();
      schema.field<&forge::plugins::chain::signer::key_binding::expected_public_key>("expected-public-key")
          .required()
          .non_empty();
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::chain::signer::transaction_profile> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::chain::signer::transaction_profile> define() {
      auto schema = forge::schema::object<forge::plugins::chain::signer::transaction_profile>();
      schema.field<&forge::plugins::chain::signer::transaction_profile::name>("name").required().non_empty();
      schema.field<&forge::plugins::chain::signer::transaction_profile::chain_id>("chain-id").required().non_empty();
      schema.field<&forge::plugins::chain::signer::transaction_profile::signing>("signing").required();
      schema.field<&forge::plugins::chain::signer::transaction_profile::authorization>("remote-authorization")
          .default_value(forge::plugins::chain::signer::remote_authorization::semantic);
      schema.field<&forge::plugins::chain::signer::transaction_profile::allow_local>("allow-local")
          .default_value(false);
      schema.field<&forge::plugins::chain::signer::transaction_profile::callers>("callers")
          .items<forge::plugins::chain::signer::caller_rule>();
      schema.field<&forge::plugins::chain::signer::transaction_profile::actions>("actions")
          .items<forge::plugins::chain::signer::action_rule>()
          .min_items(1);
      schema.field<&forge::plugins::chain::signer::transaction_profile::context_free_actions>("context-free-actions")
          .items<forge::plugins::chain::signer::context_free_action_rule>();
      schema
          .field<&forge::plugins::chain::signer::transaction_profile::max_expiration_seconds>("max-expiration-seconds")
          .default_value(std::uint32_t{120})
          .range(1, 86'400);
      schema.field<&forge::plugins::chain::signer::transaction_profile::max_delay_seconds>("max-delay-seconds")
          .default_value(std::uint32_t{0})
          .range(0, 86'400);
      schema.field<&forge::plugins::chain::signer::transaction_profile::max_actions>("max-actions")
          .default_value(std::uint32_t{16})
          .range(1, 1024);
      schema.field<&forge::plugins::chain::signer::transaction_profile::max_packed_bytes>("max-packed-bytes")
          .default_value(std::uint64_t{1024U * 1024U})
          .range(1, 64U * 1024U * 1024U);
      schema
          .field<&forge::plugins::chain::signer::transaction_profile::allow_context_free_data>(
              "allow-context-free-data")
          .default_value(false);
      schema
          .field<&forge::plugins::chain::signer::transaction_profile::allow_context_free_actions>(
              "allow-context-free-actions")
          .default_value(false);
      schema.field<&forge::plugins::chain::signer::transaction_profile::allow_extensions>("allow-extensions")
          .default_value(false);
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::chain::signer::block_profile> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::chain::signer::block_profile> define() {
      auto schema = forge::schema::object<forge::plugins::chain::signer::block_profile>();
      schema.field<&forge::plugins::chain::signer::block_profile::name>("name").required().non_empty();
      schema.field<&forge::plugins::chain::signer::block_profile::chain_id>("chain-id").required().non_empty();
      schema.field<&forge::plugins::chain::signer::block_profile::producer>("producer").required().non_empty();
      schema.field<&forge::plugins::chain::signer::block_profile::signing>("signing")
          .items<forge::plugins::chain::signer::key_binding>().min_items(1).max_items(64);
      schema.field<&forge::plugins::chain::signer::block_profile::allow_local>("allow-local").default_value(false);
      schema.field<&forge::plugins::chain::signer::block_profile::callers>("callers")
          .items<forge::plugins::chain::signer::caller_rule>();
      schema.field<&forge::plugins::chain::signer::block_profile::max_header_bytes>("max-header-bytes")
          .default_value(std::uint64_t{1024U * 1024U}).range(1, 64U * 1024U * 1024U);
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::chain::signer::finality_binding> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::chain::signer::finality_binding> define() {
      auto schema = forge::schema::object<forge::plugins::chain::signer::finality_binding>();
      schema.field<&forge::plugins::chain::signer::finality_binding::provider>("provider").required().non_empty();
      schema.field<&forge::plugins::chain::signer::finality_binding::expected_public_key>("expected-public-key")
          .required()
          .non_empty();
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::chain::signer::config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::chain::signer::config> define() {
      auto schema = forge::schema::object<forge::plugins::chain::signer::config>();
      schema.field<&forge::plugins::chain::signer::config::transaction_profiles>("transaction-profiles")
          .items<forge::plugins::chain::signer::transaction_profile>()
          .unique_by<&forge::plugins::chain::signer::transaction_profile::name>();
      schema.field<&forge::plugins::chain::signer::config::block_profiles>("block-profiles")
          .items<forge::plugins::chain::signer::block_profile>()
          .unique_by<&forge::plugins::chain::signer::block_profile::name>();
      schema.field<&forge::plugins::chain::signer::config::finality>("finality");
      schema.field<&forge::plugins::chain::signer::config::max_inflight>("max-inflight")
          .default_value(std::uint64_t{64})
          .range(1, 65'536);
      schema.field<&forge::plugins::chain::signer::config::max_queued>("max-queued")
          .default_value(std::uint64_t{256})
          .range(0, 1'000'000);
      schema.field<&forge::plugins::chain::signer::config::max_queued_bytes>("max-queued-bytes")
          .default_value(std::uint64_t{64U * 1024U * 1024U})
          .range(0, 1024U * 1024U * 1024U);
      schema.field<&forge::plugins::chain::signer::config::max_per_caller>("max-per-caller")
          .default_value(std::uint64_t{64})
          .range(1, 65'536);
      schema.field<&forge::plugins::chain::signer::config::shutdown_timeout_ms>("shutdown-timeout-ms")
          .default_value(std::uint64_t{5000})
          .range(1, 300'000);
      return schema;
   }
};
