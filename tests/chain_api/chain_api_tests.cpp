#include <boost/test/unit_test.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/system_error.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <future>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <typeindex>
#include <utility>
#include <variant>
#include <vector>

#include <forge/exceptions/macros.hpp>

import forge.api.core.connection;
import forge.api.core.binding;
import forge.api.core.exceptions;
import forge.api.core.registry;
import forge.api.http.binding;
import forge.api.http.client_response;
import forge.api.http.mapping;
import forge.api.http.openapi;
import forge.api.http.proxy;
import forge.asio.blocking;
import forge.asio.exceptions;
import forge.asio.runtime;
import forge.chain.api.admin;
import forge.chain.api.authenticated_audit_verifier;
import forge.chain.api.block;
import forge.chain.api.contract_table_projection_verifier;
import forge.chain.api.exceptions;
import forge.chain.api.finality;
import forge.chain.api.info;
import forge.chain.api.limits;
import forge.chain.api.raw_client;
import forge.chain.api.state;
import forge.chain.api.submission;
import forge.chain.api.submission_client;
import forge.chain.api.table_key;
import forge.chain.api.transaction;
import forge.chain.api.verified_client;
import forge.chain.core.merkle;
import forge.chain.protocol.audit;
import forge.chain.protocol.account_authority;
import forge.chain.protocol.contract_commitment;
import forge.chain.protocol.producer_rewards;
import forge.codec.hex;
import forge.codec.json;
import forge.crypto.asymmetric;
import forge.crypto.digest.sha256;
import forge.db.authenticated.codec;
import forge.db.authenticated.hash;
import forge.exceptions;
import forge.net.http.base_url;
import forge.net.http.client;
import forge.net.http.router;
import forge.net.http.server;
import forge.net.http.types;
import forge.raw.raw;
import forge.schema.exceptions;
import forge.schema.scalar;
import forge.variant.described;
import forge.variant.value;

namespace {

template <typename Client>
concept exposes_raw_client = requires(Client& client) { client.raw(); };

template <typename Client>
concept exposes_submission = requires(Client& client, forge::chain::protocol::transaction_submit_request request) {
   client.submit(std::move(request));
};

template <typename Client>
concept exposes_indirect_submission =
    requires(Client& client, forge::chain::protocol::transaction_submit_request request) {
       client.transactions().submit(std::move(request));
    };

template <typename Client>
concept exposes_administration = requires(Client& client) { client.admin(); };

template <typename State>
concept exposes_state_point = requires { &State::get_point; };
template <typename State>
concept exposes_state_range = requires { &State::get_range; };
template <typename State>
concept exposes_state_changes = requires { &State::get_changes; };

static_assert(!exposes_raw_client<forge::chain::api::verified_client>);
static_assert(!exposes_administration<forge::chain::api::raw_client>);
static_assert(!exposes_submission<forge::chain::api::verified_client>);
static_assert(exposes_submission<forge::chain::api::submission_client>);
static_assert(!exposes_submission<forge::chain::api::transaction>);
static_assert(!exposes_indirect_submission<forge::chain::api::raw_client>);
static_assert(!exposes_state_point<forge::chain::api::state>);
static_assert(!exposes_state_range<forge::chain::api::state>);
static_assert(!exposes_state_changes<forge::chain::api::state>);

using forge::api::http::cache_policy;
using forge::api::http::route;
using forge::net::http::method;
namespace authenticated = forge::db::authenticated;

static_assert(std::same_as<decltype(forge::chain::protocol::transaction_read_only_request{}.transaction),
                           forge::chain::protocol::packed_transaction>);

template <typename T> T run(boost::asio::awaitable<T> operation) {
   auto context = boost::asio::io_context{};
   auto result = boost::asio::co_spawn(context, std::move(operation), boost::asio::use_future);
   context.run();
   return result.get();
}

forge::chain::protocol::account_request account_by_name(forge::chain::protocol::account_name value,
                                                        std::optional<forge::chain::protocol::block_id> anchor = {}) {
   auto request = forge::chain::protocol::account_request{};
   request.key = value;
   request.anchor = anchor;
   return request;
}

class block_service final : public forge::chain::api::block {
 public:
   explicit block_service(forge::chain::protocol::block_response response) : response_{std::move(response)} {}
   explicit block_service(forge::chain::protocol::block_header_response response)
       : header_response_{std::move(response)} {}

   boost::asio::awaitable<forge::chain::protocol::block_response>
   get_block(forge::chain::protocol::block_request) override {
      co_return response_;
   }

   boost::asio::awaitable<forge::chain::protocol::block_header_response>
   get_header(forge::chain::protocol::block_request) override {
      co_return header_response_;
   }

   boost::asio::awaitable<forge::chain::protocol::block_state_response>
   get_block_state(forge::chain::protocol::block_request) override {
      co_return forge::chain::protocol::block_state_response{};
   }

   boost::asio::awaitable<forge::chain::protocol::block_range_response>
   get_canonical_range(forge::chain::protocol::block_range_request) override {
      co_return forge::chain::protocol::block_range_response{};
   }

   boost::asio::awaitable<forge::chain::protocol::protocol_features_response>
   get_activated_protocol_features(forge::chain::protocol::protocol_features_request) override {
      co_return forge::chain::protocol::protocol_features_response{};
   }

   boost::asio::awaitable<forge::chain::protocol::consensus_parameters_response>
   get_consensus_parameters(forge::chain::protocol::anchored_request) override {
      co_return forge::chain::protocol::consensus_parameters_response{};
   }

   boost::asio::awaitable<forge::chain::protocol::producers_response>
   get_producers(forge::chain::protocol::producers_request) override {
      if (throw_producers_not_found) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::not_found, "producer table is not available");
      }
      co_return forge::chain::protocol::producers_response{};
   }

   boost::asio::awaitable<forge::chain::protocol::producer_rewards_response>
   get_producer_rewards(forge::chain::protocol::producer_rewards_request) override {
      co_return rewards_response;
   }

   boost::asio::awaitable<forge::chain::protocol::producer_schedule_response>
   get_producer_schedule(forge::chain::protocol::anchored_request) override {
      co_return forge::chain::protocol::producer_schedule_response{};
   }

   boost::asio::awaitable<forge::chain::protocol::finalizer_info_response>
   get_finalizer_info(forge::chain::protocol::anchored_request) override {
      co_return forge::chain::protocol::finalizer_info_response{};
   }

   bool throw_producers_not_found = false;
   forge::chain::protocol::producer_rewards_response rewards_response;

 private:
   forge::chain::protocol::block_response response_;
   forge::chain::protocol::block_header_response header_response_;
};

class snapshot_admin_service final : public forge::chain::api::admin {
 public:
   explicit snapshot_admin_service(forge::chain::protocol::snapshot_response response)
       : response_{std::move(response)} {}

   boost::asio::awaitable<forge::chain::protocol::push_block_response>
   push_block(forge::chain::protocol::signed_block) override {
      co_return forge::chain::protocol::push_block_response{};
   }

   boost::asio::awaitable<forge::chain::protocol::snapshot_response> create_snapshot(std::string name) override {
      auto response = response_;
      response.name = std::move(name);
      co_return response;
   }

   boost::asio::awaitable<forge::chain::protocol::snapshot_lifecycle_response>
   request_snapshot(forge::chain::protocol::snapshot_request request) override {
      if (request.request_id == lost_request_id) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::snapshot_lost,
                               "snapshot request anchor is no longer canonical");
      }
      if (accepted_request && accepted_request->request_id == request.request_id &&
          accepted_request->name != request.name) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::conflict,
                               "snapshot request id was already used with another name");
      }
      accepted_request = request;
      co_return lifecycle(request);
   }

   boost::asio::awaitable<forge::chain::protocol::snapshot_lifecycle_response>
   snapshot_status(forge::chain::protocol::snapshot_status_request request) override {
      if (!accepted_request || request.request_id != accepted_request->request_id) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::not_found, "snapshot identity is not known");
      }
      co_return lifecycle(*accepted_request);
   }

   boost::asio::awaitable<forge::chain::protocol::prune_response>
   prune(forge::chain::protocol::prune_request) override {
      co_return forge::chain::protocol::prune_response{};
   }

   boost::asio::awaitable<forge::chain::protocol::producer_status_response>
   producer_status(forge::chain::protocol::admin_query) override {
      co_return forge::chain::protocol::producer_status_response{};
   }

   boost::asio::awaitable<forge::chain::protocol::supported_protocol_features_response>
   supported_protocol_features(forge::chain::protocol::supported_protocol_features_request) override {
      co_return forge::chain::protocol::supported_protocol_features_response{};
   }

   boost::asio::awaitable<forge::chain::protocol::ram_corrections_response>
   account_ram_corrections(forge::chain::protocol::ram_corrections_request) override {
      co_return forge::chain::protocol::ram_corrections_response{};
   }

   boost::asio::awaitable<forge::chain::protocol::unapplied_transactions_response>
   unapplied_transactions(forge::chain::protocol::unapplied_transactions_request) override {
      co_return forge::chain::protocol::unapplied_transactions_response{};
   }

   boost::asio::awaitable<forge::chain::protocol::snapshot_requests_response>
   snapshot_requests(forge::chain::protocol::admin_query) override {
      co_return forge::chain::protocol::snapshot_requests_response{};
   }

   boost::asio::awaitable<bool> configure_pause(forge::chain::protocol::producer_pause_request) override {
      co_return false;
   }

   boost::asio::awaitable<bool> update_runtime_options(forge::chain::protocol::producer_runtime_options) override {
      co_return false;
   }

   boost::asio::awaitable<bool> update_greylist(forge::chain::protocol::greylist_update_request) override {
      co_return false;
   }

   boost::asio::awaitable<bool> set_access_policy(forge::chain::protocol::producer_access_policy) override {
      co_return false;
   }

   boost::asio::awaitable<forge::chain::protocol::snapshot_schedule>
   schedule_snapshot(forge::chain::protocol::snapshot_schedule_request) override {
      co_return forge::chain::protocol::snapshot_schedule{};
   }

   boost::asio::awaitable<forge::chain::protocol::snapshot_schedule>
   unschedule_snapshot(forge::chain::protocol::snapshot_schedule_id) override {
      co_return forge::chain::protocol::snapshot_schedule{};
   }

   boost::asio::awaitable<forge::chain::protocol::integrity_hash_response>
   integrity_hash(forge::chain::protocol::admin_query) override {
      co_return forge::chain::protocol::integrity_hash_response{};
   }

   boost::asio::awaitable<bool> schedule_protocol_features(std::vector<forge::chain::protocol::digest>) override {
      co_return false;
   }

   forge::chain::protocol::snapshot_state state = forge::chain::protocol::snapshot_state::pending;
   std::optional<forge::chain::protocol::snapshot_request> accepted_request;
   std::string lost_request_id;

 private:
   forge::chain::protocol::snapshot_lifecycle_response
   lifecycle(const forge::chain::protocol::snapshot_request& request) const {
      return {
          .request_id = request.request_id,
          .name = request.name,
          .state = state,
          .head = response_.head,
          .head_num = response_.head_num,
      };
   }

   forge::chain::protocol::snapshot_response response_;
};

class info_service final : public forge::chain::api::info {
 public:
   explicit info_service(forge::chain::protocol::info_response response) : response_{std::move(response)} {}

   boost::asio::awaitable<forge::chain::protocol::info_response>
   get(forge::chain::protocol::anchored_request) override {
      co_return response_;
   }

 private:
   forge::chain::protocol::info_response response_;
};

class state_service final : public forge::chain::api::state {
 public:
   explicit state_service(forge::chain::protocol::table_changes_response response)
       : table_changes_response_{std::move(response)} {}
   explicit state_service(forge::chain::protocol::account_changes_response response)
       : account_changes_response_{std::move(response)} {}
   explicit state_service(forge::chain::protocol::account_response response) : account_response_{std::move(response)} {}

   boost::asio::awaitable<forge::chain::protocol::account_response>
   get_account(forge::chain::protocol::account_request request) override {
      last_account_request = std::move(request);
      if (account_failure == failure::standard) {
         throw std::runtime_error{"test state service failure"};
      }
      if (account_failure == failure::nonstandard) {
         throw 7;
      }
      if (account_failure == failure::canceled) {
         throw boost::system::system_error{boost::asio::error::operation_aborted};
      }
      if (account_failure == failure::timed_out) {
         throw boost::system::system_error{boost::asio::error::timed_out};
      }
      if (account_failure == failure::api_canceled) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "test API transport cancellation");
      }
      if (account_failure == failure::api_deadline) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::deadline_exceeded, "test API transport deadline");
      }
      if (account_failure == failure::foreign_forge) {
         FORGE_THROW_EXCEPTION(forge::asio::exceptions::internal, "test foreign Forge service failure");
      }
      if (account_failure == failure::chain_api) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::invalid_request, "test Chain API failure");
      }
      co_return account_response_;
   }

   boost::asio::awaitable<forge::chain::protocol::account_changes_response>
   get_account_changes(forge::chain::protocol::account_changes_request) override {
      co_return account_changes_response_;
   }

   boost::asio::awaitable<forge::chain::protocol::code_response>
   get_code(forge::chain::protocol::code_request) override {
      co_return forge::chain::protocol::code_response{};
   }

   boost::asio::awaitable<forge::chain::protocol::permission_links_response>
   get_permission_links(forge::chain::protocol::permission_links_request) override {
      co_return forge::chain::protocol::permission_links_response{};
   }

   boost::asio::awaitable<forge::chain::protocol::table_rows_response>
   get_table_rows(forge::chain::protocol::table_rows_request) override {
      co_return forge::chain::protocol::table_rows_response{};
   }

   boost::asio::awaitable<forge::chain::protocol::table_changes_response>
   get_table_changes(forge::chain::protocol::table_changes_request) override {
      co_return table_changes_response_;
   }

   boost::asio::awaitable<forge::chain::protocol::table_scope_response>
   get_table_scope(forge::chain::protocol::table_scope_request) override {
      co_return forge::chain::protocol::table_scope_response{};
   }

   boost::asio::awaitable<forge::chain::protocol::currency_balance_response>
   get_currency_balance(forge::chain::protocol::currency_balance_request) override {
      co_return forge::chain::protocol::currency_balance_response{};
   }

   boost::asio::awaitable<forge::chain::protocol::currency_stats_response>
   get_currency_stats(forge::chain::protocol::currency_stats_request) override {
      co_return forge::chain::protocol::currency_stats_response{};
   }

   boost::asio::awaitable<forge::chain::protocol::scheduled_response>
   get_scheduled_transactions(forge::chain::protocol::scheduled_request) override {
      co_return forge::chain::protocol::scheduled_response{};
   }

   boost::asio::awaitable<forge::chain::protocol::authorizers_response>
   get_accounts_by_authorizers(forge::chain::protocol::authorizers_request) override {
      co_return forge::chain::protocol::authorizers_response{};
   }

   std::optional<forge::chain::protocol::account_request> last_account_request;
   enum class failure : std::uint8_t {
      none,
      standard,
      nonstandard,
      canceled,
      timed_out,
      api_canceled,
      api_deadline,
      foreign_forge,
      chain_api
   };
   failure account_failure = failure::none;

 private:
   forge::chain::protocol::account_response account_response_;
   forge::chain::protocol::account_changes_response account_changes_response_;
   forge::chain::protocol::table_changes_response table_changes_response_;
};

class transaction_service final : public forge::chain::api::transaction {
 public:
   explicit transaction_service(forge::chain::protocol::transaction_status_response response)
       : status_responses_{response}, await_responses_{std::move(response)} {}

   transaction_service(std::vector<forge::chain::protocol::transaction_status_response> status_responses,
                       std::vector<forge::chain::protocol::transaction_status_response> await_responses)
       : status_responses_{std::move(status_responses)}, await_responses_{std::move(await_responses)} {}

   boost::asio::awaitable<forge::chain::protocol::transaction_status_response>
   get_status(forge::chain::protocol::transaction_status_request request) override {
      status_requests.push_back(std::move(request));
      co_return next_response(status_responses_, next_status_response_);
   }

   boost::asio::awaitable<forge::chain::protocol::transaction_status_response>
   await_transaction(forge::chain::protocol::transaction_await_request request) override {
      await_requests.push_back(std::move(request));
      co_return next_response(await_responses_, next_await_response_);
   }

   boost::asio::awaitable<std::vector<forge::chain::protocol::public_key>>
   get_required_keys(forge::chain::protocol::transaction_required_keys_request) override {
      co_return std::vector<forge::chain::protocol::public_key>{};
   }

   boost::asio::awaitable<forge::chain::protocol::transaction_read_only_response>
   compute_transaction(forge::chain::protocol::transaction_read_only_request) override {
      co_return forge::chain::protocol::transaction_read_only_response{};
   }

   boost::asio::awaitable<forge::chain::protocol::transaction_read_only_response>
   send_read_only_transaction(forge::chain::protocol::transaction_read_only_request) override {
      co_return forge::chain::protocol::transaction_read_only_response{};
   }

   std::vector<forge::chain::protocol::transaction_status_request> status_requests;
   std::vector<forge::chain::protocol::transaction_await_request> await_requests;

 private:
   static forge::chain::protocol::transaction_status_response
   next_response(const std::vector<forge::chain::protocol::transaction_status_response>& responses, std::size_t& next) {
      if (responses.empty()) {
         return {};
      }
      const auto index = std::min(next, responses.size() - 1U);
      ++next;
      return responses[index];
   }

   std::vector<forge::chain::protocol::transaction_status_response> status_responses_;
   std::vector<forge::chain::protocol::transaction_status_response> await_responses_;
   std::size_t next_status_response_ = 0;
   std::size_t next_await_response_ = 0;
};

class submission_service final : public forge::chain::api::submission {
 public:
   explicit submission_service(std::vector<forge::chain::protocol::transaction_submit_response> responses)
       : responses_{std::move(responses)} {}

   boost::asio::awaitable<forge::chain::protocol::transaction_submit_response>
   submit(forge::chain::protocol::transaction_submit_request) override {
      if (throw_standard) {
         throw std::runtime_error{"test submission service failure"};
      }
      if (throw_forge) {
         FORGE_THROW_EXCEPTION(forge::asio::exceptions::internal, "test foreign Forge submission failure");
      }
      if (throw_timed_out) {
         throw boost::system::system_error{boost::asio::error::timed_out};
      }
      if (throw_api_canceled) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "test API submission cancellation");
      }
      if (throw_api_deadline) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::deadline_exceeded, "test API submission deadline");
      }
      if (responses_.empty()) {
         co_return forge::chain::protocol::transaction_submit_response{};
      }
      co_return responses_.front();
   }

   boost::asio::awaitable<std::vector<forge::chain::protocol::transaction_submit_response>>
   submit_batch(forge::chain::protocol::transaction_submit_batch_request) override {
      if (throw_api_canceled) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "test API batch submission cancellation");
      }
      if (throw_api_deadline) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::deadline_exceeded, "test API batch submission deadline");
      }
      co_return responses_;
   }

 private:
   std::vector<forge::chain::protocol::transaction_submit_response> responses_;

 public:
   bool throw_standard = false;
   bool throw_forge = false;
   bool throw_timed_out = false;
   bool throw_api_canceled = false;
   bool throw_api_deadline = false;
};

class deadline_remote_invoker final : public forge::api::core::remote_invoker {
 public:
   boost::asio::awaitable<forge::api::core::response> async_call(forge::api::core::request value) override {
      ++calls;
      const auto descriptor = forge::chain::api::transaction::describe();
      const auto* method = forge::api::core::find_method(descriptor, value.method);
      if (method == nullptr) {
         throw forge::api::core::exceptions::protocol_error{"remote test method descriptor is missing"};
      }
      const auto error =
          forge::chain::api::exceptions::deadline_exceeded{"remote transaction wait reached its deadline"};
      co_return forge::api::core::response{
          .api = std::move(value.api),
          .method = std::move(value.method),
          .error = forge::api::core::project_error(*method, error),
      };
   }

   std::size_t calls = 0;
};

class trust_required_remote_invoker final : public forge::api::core::remote_invoker {
 public:
   boost::asio::awaitable<forge::api::core::response> async_call(forge::api::core::request value) override {
      ++calls;
      const auto descriptor = forge::chain::api::info::describe();
      const auto* method = forge::api::core::find_method(descriptor, value.method);
      if (method == nullptr) {
         throw forge::api::core::exceptions::protocol_error{"remote test method descriptor is missing"};
      }
      const auto error = forge::chain::api::exceptions::trust_required{"remote finality trust is outside retention"};
      co_return forge::api::core::response{
          .api = std::move(value.api),
          .method = std::move(value.method),
          .error = forge::api::core::project_error(*method, error),
      };
   }

   std::size_t calls = 0;
};

class accepting_audit_verifier final : public forge::chain::api::audit_verifier {
 public:
   [[nodiscard]] std::optional<forge::chain::protocol::block_id> preferred_finality_anchor() const override {
      if (throw_standard_preferred_anchor) {
         throw std::runtime_error{"test preferred anchor failure"};
      }
      return preferred_anchor;
   }

   [[nodiscard]] std::optional<forge::chain::protocol::block_id>
   finality_anchor_at_or_before(forge::chain::protocol::block_num target) const override {
      finality_anchor_targets.push_back(target);
      return retained_anchor ? retained_anchor : preferred_finality_anchor();
   }

   void verify_context(const forge::chain::protocol::response_context&) override {
      if (throw_standard_context) {
         throw std::runtime_error{"test context verifier failure"};
      }
   }
   void verify_finality(const forge::chain::protocol::state_anchor&,
                        const forge::chain::protocol::proof_blob&) override {}
   std::optional<forge::chain::protocol::bytes> verify_state_point(const forge::chain::protocol::state_anchor&,
                                                                   const forge::chain::protocol::bytes&,
                                                                   const forge::chain::protocol::proof_blob&) override {
      ++state_point_verifications;
      if (throw_nonstandard_state_point) {
         throw 7;
      }
      return point_value;
   }
   forge::chain::api::authenticated_state_range verify_state_range(const forge::chain::protocol::state_anchor&,
                                                                   const forge::chain::api::authenticated_range_query&,
                                                                   const forge::chain::protocol::proof_blob&) override {
      ++state_range_verifications;
      if (!state_ranges.empty()) {
         return state_ranges.at(state_range_verifications - 1U);
      }
      return range_result;
   }
   forge::chain::api::authenticated_change_range
   verify_state_changes(const forge::chain::protocol::state_anchor& anchor,
                        const forge::chain::api::authenticated_range_query&,
                        const forge::chain::protocol::proof_blob& proof) override {
      ++state_change_verifications;
      if (expected_state_change_proofs) {
         const auto index = state_change_anchors.size();
         if (index >= expected_state_change_proofs->size() ||
             (*expected_state_change_proofs)[index] != std::pair{anchor.block_num, proof.scheme}) {
            FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::invalid_state_proof,
                                  "test state change proof is associated with the wrong block anchor");
         }
      }
      state_change_anchors.push_back(anchor);
      if (state_change_result) {
         return *state_change_result;
      }
      return forge::chain::api::authenticated_change_range{};
   }
   void verify_ancestry(const forge::chain::protocol::state_anchor& finalized,
                        std::span<const forge::chain::protocol::state_anchor> intermediate,
                        const forge::chain::protocol::proof_blob& proof) override {
      ++ancestry_verifications;
      ancestry_finalized = finalized;
      ancestry_intermediate.assign(intermediate.begin(), intermediate.end());
      ancestry_proof = proof;
   }
   void verify_transaction(const forge::chain::protocol::state_anchor&, const forge::chain::protocol::transaction_id&,
                           const forge::chain::protocol::transaction_status_response&,
                           const forge::chain::protocol::transaction_inclusion_proof&) override {
      ++transaction_verifications;
      if (throw_standard_transaction) {
         throw std::runtime_error{"test transaction verifier failure"};
      }
   }

   std::size_t state_point_verifications = 0;
   std::size_t state_range_verifications = 0;
   std::size_t state_change_verifications = 0;
   std::size_t transaction_verifications = 0;
   std::size_t ancestry_verifications = 0;
   std::optional<forge::chain::protocol::bytes> point_value;
   forge::chain::api::authenticated_state_range range_result;
   std::vector<forge::chain::api::authenticated_state_range> state_ranges;
   std::optional<forge::chain::api::authenticated_change_range> state_change_result;
   std::optional<std::vector<std::pair<std::uint32_t, std::string>>> expected_state_change_proofs;
   std::optional<forge::chain::protocol::state_anchor> ancestry_finalized;
   std::vector<forge::chain::protocol::state_anchor> ancestry_intermediate;
   std::vector<forge::chain::protocol::state_anchor> state_change_anchors;
   std::optional<forge::chain::protocol::proof_blob> ancestry_proof;
   bool throw_standard_context = false;
   bool throw_nonstandard_state_point = false;
   bool throw_standard_transaction = false;
   bool throw_standard_preferred_anchor = false;
   std::optional<forge::chain::protocol::block_id> preferred_anchor = forge::chain::protocol::block_id{};
   std::optional<forge::chain::protocol::block_id> retained_anchor;
   mutable std::vector<forge::chain::protocol::block_num> finality_anchor_targets;
};

class account_projection_verifier final : public forge::chain::api::projection_verifier {
 public:
   void verify(const forge::chain::protocol::account_request& request,
               const forge::chain::protocol::account_response& response,
               const forge::chain::protocol::audit_bundle& audit,
               forge::chain::api::audit_verifier& verifier) override {
      if (!request.key || response.account.name != *request.key || !response.context.anchor ||
          audit.state.size() != 1U) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::invalid_state_proof,
                               "test account projection requires the requested account and one authenticated source");
      }
      ++verifications;
      const auto value =
          verifier.verify_state_point(*response.context.anchor, forge::chain::protocol::bytes{9U}, audit.state.front());
      if (value != forge::chain::protocol::bytes{1U, 2U}) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::invalid_state_proof,
                               "test account projection rejected its authenticated source");
      }
   }

   std::size_t verifications = 0;
};

class throwing_account_projection_verifier final : public forge::chain::api::projection_verifier {
 public:
   void verify(const forge::chain::protocol::account_request&, const forge::chain::protocol::account_response&,
               const forge::chain::protocol::audit_bundle&, forge::chain::api::audit_verifier&) override {
      if (nonstandard) {
         throw 7;
      }
      throw std::runtime_error{"test projection verifier failure"};
   }

   bool nonstandard = false;
};

class typed_changes_projection_verifier final : public forge::chain::api::projection_verifier {
 public:
   void verify(const forge::chain::protocol::table_changes_request& request,
               const forge::chain::protocol::table_changes_response& response,
               const forge::chain::protocol::audit_bundle& audit,
               forge::chain::api::audit_verifier& verifier) override {
      require_change_envelope(response, audit);
      if (expected_chain && response.context.chain != *expected_chain) {
         reject("table changes cursor is bound to another chain");
      }
      if (expected_table_cursor != request.cursor || expected_next != response.next) {
         reject("table changes cursor binding is invalid");
      }
      if (response.context.anchor->block_num != request.to_block) {
         reject("table changes cursor is bound to another target anchor");
      }
      if (expected_anchor && response.context.anchor->block != *expected_anchor) {
         reject("table changes cursor is bound to another target block");
      }
      if (expected_tables && request.tables != *expected_tables) {
         reject("table changes cursor is bound to another selector set");
      }
      if (expected_table_blocks && response.blocks != *expected_table_blocks) {
         reject("table changes projection is incomplete or reordered");
      }

      if (expected_table_proofs_per_batch && expected_table_proofs_per_batch->size() != response.blocks.size()) {
         reject("table changes proof layout does not match its block batches");
      }
      auto proof_position = std::size_t{};
      for (std::size_t index = 0; index < response.blocks.size(); ++index) {
         const auto& block = response.blocks[index];
         auto identities = std::set<std::tuple<forge::chain::protocol::account_name, forge::chain::protocol::name,
                                               forge::chain::protocol::name, std::uint64_t>>{};
         for (const auto& mutation : block.mutations) {
            if (!identities.emplace(mutation.table.code, mutation.table.scope, mutation.table.table, mutation.primary)
                     .second) {
               reject("table change batch violates last-write-wins identity uniqueness");
            }
         }
         const auto proof_count = expected_table_proofs_per_batch ? (*expected_table_proofs_per_batch)[index] : 1U;
         for (auto proof = std::size_t{}; proof < proof_count; ++proof) {
            if (proof_position >= audit.state.size()) {
               reject("table changes response omits a block-bound authenticated proof");
            }
            static_cast<void>(verifier.verify_state_changes(
                block.anchor, forge::chain::api::authenticated_range_query{.limit = request.limit},
                audit.state[proof_position++]));
         }
      }
      if (proof_position != audit.state.size()) {
         reject("table changes response contains an unauthenticated trailing proof");
      }
      last_table_request = request;
      ++table_verifications;
   }

   void verify(const forge::chain::protocol::account_changes_request& request,
               const forge::chain::protocol::account_changes_response& response,
               const forge::chain::protocol::audit_bundle& audit,
               forge::chain::api::audit_verifier& verifier) override {
      require_change_envelope(response, audit);
      if (expected_chain && response.context.chain != *expected_chain) {
         reject("account changes cursor is bound to another chain");
      }
      if (expected_account_cursor != request.cursor || expected_next != response.next) {
         reject("account changes cursor binding is invalid");
      }
      if (response.context.anchor->block_num != request.to_block) {
         reject("account changes cursor is bound to another target anchor");
      }
      if (expected_anchor && response.context.anchor->block != *expected_anchor) {
         reject("account changes cursor is bound to another target block");
      }
      if (expected_accounts && request.accounts != *expected_accounts) {
         reject("account changes cursor is bound to another account set");
      }
      if (expected_account_blocks && response.blocks != *expected_account_blocks) {
         reject("account changes projection is incomplete or reordered");
      }

      if (expected_account_proofs_per_batch && expected_account_proofs_per_batch->size() != response.blocks.size()) {
         reject("account changes proof layout does not match its block batches");
      }
      auto proof_position = std::size_t{};
      for (std::size_t index = 0; index < response.blocks.size(); ++index) {
         const auto& block = response.blocks[index];
         auto identities = std::set<forge::chain::protocol::account_name>{};
         for (const auto& mutation : block.mutations) {
            if (!identities.insert(mutation.account).second) {
               reject("account change batch violates last-write-wins identity uniqueness");
            }
         }
         const auto proof_count = expected_account_proofs_per_batch ? (*expected_account_proofs_per_batch)[index] : 1U;
         for (auto proof = std::size_t{}; proof < proof_count; ++proof) {
            if (proof_position >= audit.state.size()) {
               reject("account changes response omits a block-bound authenticated proof");
            }
            static_cast<void>(verifier.verify_state_changes(
                block.anchor, forge::chain::api::authenticated_range_query{.limit = request.limit},
                audit.state[proof_position++]));
         }
      }
      if (proof_position != audit.state.size()) {
         reject("account changes response contains an unauthenticated trailing proof");
      }
      last_account_request = request;
      ++account_verifications;
   }

   std::optional<forge::chain::protocol::chain_id> expected_chain;
   std::optional<forge::chain::protocol::block_id> expected_anchor;
   std::optional<std::vector<forge::chain::protocol::table_change_selector>> expected_tables;
   std::optional<std::vector<forge::chain::protocol::table_change_batch>> expected_table_blocks;
   std::optional<std::vector<std::size_t>> expected_table_proofs_per_batch;
   std::optional<std::vector<forge::chain::protocol::account_name>> expected_accounts;
   std::optional<std::vector<forge::chain::protocol::account_change_batch>> expected_account_blocks;
   std::optional<std::vector<std::size_t>> expected_account_proofs_per_batch;
   std::optional<forge::chain::protocol::bytes> expected_table_cursor;
   std::optional<forge::chain::protocol::bytes> expected_account_cursor;
   std::optional<forge::chain::protocol::bytes> expected_next;
   std::optional<forge::chain::protocol::table_changes_request> last_table_request;
   std::optional<forge::chain::protocol::account_changes_request> last_account_request;
   std::size_t table_verifications = 0;
   std::size_t account_verifications = 0;

 private:
   [[noreturn]] static void reject(std::string_view message) {
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::invalid_state_proof, message);
   }

   template <typename Response>
   static void require_change_envelope(const Response& response, const forge::chain::protocol::audit_bundle& audit) {
      if (!response.context.anchor || response.blocks.empty() || audit.state.empty()) {
         reject("typed changes projection requires block batches and authenticated sources");
      }
   }
};

class recording_finality_verifier final : public forge::chain::api::finality_verifier {
 public:
   void verify(const forge::chain::protocol::state_anchor&, const forge::chain::protocol::proof_blob&) override {
      ++verify_calls;
      if (throw_nonstandard) {
         throw 7;
      }
      if (failures_remaining != 0U) {
         --failures_remaining;
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::invalid_finality,
                               "test finality delegate rejected anchor");
      }
   }

   void verify_ancestry(const forge::chain::protocol::state_anchor& finalized,
                        std::span<const forge::chain::protocol::state_anchor> intermediate,
                        const forge::chain::protocol::proof_blob& proof) override {
      ++ancestry_calls;
      if (throw_nonstandard) {
         throw 7;
      }
      ancestry_finalized = finalized;
      ancestry_intermediate.assign(intermediate.begin(), intermediate.end());
      ancestry_proof = proof;
   }

   std::size_t verify_calls = 0;
   std::size_t ancestry_calls = 0;
   std::size_t failures_remaining = 0;
   bool throw_nonstandard = false;
   std::optional<forge::chain::protocol::state_anchor> ancestry_finalized;
   std::vector<forge::chain::protocol::state_anchor> ancestry_intermediate;
   std::optional<forge::chain::protocol::proof_blob> ancestry_proof;
};

class blocking_finality_verifier final : public forge::chain::api::finality_verifier {
 public:
   void verify(const forge::chain::protocol::state_anchor&, const forge::chain::protocol::proof_blob&) override {
      const auto call = verify_calls.fetch_add(1U) + 1U;
      if (call != 1U) {
         return;
      }

      auto lock = std::unique_lock{mutex_};
      entered_ = true;
      entered_condition_.notify_all();
      release_condition_.wait(lock, [this] { return released_; });
   }

   void verify_ancestry(const forge::chain::protocol::state_anchor&,
                        std::span<const forge::chain::protocol::state_anchor>,
                        const forge::chain::protocol::proof_blob&) override {}

   void wait_until_entered() {
      auto lock = std::unique_lock{mutex_};
      entered_condition_.wait(lock, [this] { return entered_; });
   }

   void release() {
      {
         const auto lock = std::lock_guard{mutex_};
         released_ = true;
      }
      release_condition_.notify_all();
   }

   std::atomic<std::size_t> verify_calls = 0;

 private:
   std::mutex mutex_;
   std::condition_variable entered_condition_;
   std::condition_variable release_condition_;
   bool entered_ = false;
   bool released_ = false;
};

class standard_throwing_finality_verifier final : public forge::chain::api::finality_verifier {
 public:
   void verify(const forge::chain::protocol::state_anchor&, const forge::chain::protocol::proof_blob&) override {
      throw std::runtime_error{"test finality implementation failure"};
   }

   void verify_ancestry(const forge::chain::protocol::state_anchor&,
                        std::span<const forge::chain::protocol::state_anchor>,
                        const forge::chain::protocol::proof_blob&) override {
      throw std::runtime_error{"test ancestry implementation failure"};
   }
};

forge::chain::protocol::state_anchor make_finality_anchor() {
   auto anchor = forge::chain::protocol::state_anchor{};
   anchor.chain._hash[0] = 1U;
   anchor.block._hash[0] = 2U;
   anchor.block_num = 3U;
   anchor.transaction_root._hash[0] = 4U;
   anchor.state_root._hash[0] = 5U;
   anchor.state_size = 6U;
   anchor.change_root._hash[0] = 7U;
   anchor.change_count = 8U;
   return anchor;
}

authenticated::bytes authenticated_bytes(std::string_view value) {
   auto result = authenticated::bytes{};
   result.reserve(value.size());
   for (const auto character : value) {
      result.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
   }
   return result;
}

forge::chain::protocol::bytes protocol_bytes(std::span<const std::byte> value) {
   auto result = forge::chain::protocol::bytes{};
   result.reserve(value.size());
   for (const auto byte : value) {
      result.push_back(std::to_integer<std::uint8_t>(byte));
   }
   return result;
}

forge::chain::protocol::bytes protocol_bytes(std::string_view value) {
   const auto encoded = authenticated_bytes(value);
   return protocol_bytes(encoded);
}

authenticated::proof_leaf authenticated_leaf(std::string_view key, std::string_view value) {
   auto encoded = authenticated_bytes(value);
   return {
       .key = authenticated_bytes(key),
       .value_hash = authenticated::hash_value(encoded),
       .value = std::move(encoded),
   };
}

authenticated::proof_leaf authenticated_change_leaf(const authenticated::mutation& mutation) {
   auto encoded = authenticated::encode_change_value(mutation);
   return {
       .key = mutation.key,
       .value_hash = authenticated::hash_value(encoded),
       .value = std::move(encoded),
   };
}

authenticated::digest authenticated_leaf_hash(std::string_view tree_domain, const authenticated::proof_leaf& value) {
   return authenticated::hash_leaf(tree_domain, value.key, value.value_hash);
}

authenticated::digest authenticated_branch_hash(std::string_view tree_domain,
                                                const authenticated::proof_branch& value) {
   return authenticated::hash_inner(tree_domain, value.height, value.size, value.min_key, value.max_key,
                                    value.separator, value.left_hash, value.right_hash);
}

forge::chain::protocol::chain_id authenticated_chain() {
   auto chain = forge::chain::protocol::chain_id{};
   chain._hash[0] = 0x41U;
   return chain;
}

forge::chain::protocol::state_anchor authenticated_anchor(const authenticated::root& value) {
   auto result = forge::chain::protocol::state_anchor{
       .chain = authenticated_chain(),
       .block_num = static_cast<std::uint32_t>(value.version),
       .state_root = value.state_root,
       .state_size = value.state_size,
       .change_root = value.change_root,
       .change_count = value.change_count,
   };
   result.block._hash[0] = 0x42U;
   return result;
}

template <typename Proof>
forge::chain::protocol::proof_blob authenticated_proof_blob(std::string scheme, const Proof& proof) {
   const auto encoded = authenticated::encode(proof);
   return {
       .scheme = std::move(scheme),
       .version = 1U,
       .payload = protocol_bytes(encoded),
   };
}

struct authenticated_point_fixture {
   std::string domain;
   authenticated::proof_leaf alpha;
   authenticated::proof_leaf gamma;
   authenticated::root root;
   std::vector<authenticated::proof_step> path;
};

authenticated_point_fixture make_authenticated_point_fixture() {
   auto fixture = authenticated_point_fixture{
       .domain = "forge.test.chain-api.authenticated-point.v3",
       .alpha = authenticated_leaf("alpha", "one"),
       .gamma = authenticated_leaf("gamma", "three"),
   };
   const auto state_domain = authenticated::canonical_tree_domain(fixture.domain, authenticated::proof_tree::state);
   const auto change_domain = authenticated::canonical_tree_domain(fixture.domain, authenticated::proof_tree::changes);
   fixture.root = {
       .version = 41U,
       .state_root = authenticated::hash_inner(state_domain, 1U, 2U, fixture.alpha.key, fixture.gamma.key,
                                               fixture.gamma.key, authenticated_leaf_hash(state_domain, fixture.alpha),
                                               authenticated_leaf_hash(state_domain, fixture.gamma)),
       .state_size = 2U,
       .change_root = authenticated::hash_empty(change_domain),
   };
   fixture.path = {{
       .child = authenticated::branch_side::left,
       .height = 1U,
       .subtree_size = 2U,
       .min_key = fixture.alpha.key,
       .max_key = fixture.gamma.key,
       .separator = fixture.gamma.key,
       .sibling = fixture.gamma,
   }};
   return fixture;
}

authenticated::point_proof authenticated_point_proof(const authenticated_point_fixture& fixture, std::string_view key) {
   return {
       .anchor = fixture.root,
       .key = authenticated_bytes(key),
       .terminal = fixture.alpha,
       .path = fixture.path,
   };
}

struct authenticated_ranked_fixture {
   std::string domain;
   std::string tree_domain;
   authenticated::proof_leaf a;
   authenticated::proof_leaf b;
   authenticated::proof_leaf c;
   authenticated::proof_leaf d;
   authenticated::proof_branch left;
   authenticated::proof_branch right;
   authenticated::root root;
};

authenticated_ranked_fixture make_authenticated_ranked_fixture() {
   auto fixture = authenticated_ranked_fixture{
       .domain = "forge.test.chain-api.authenticated-range.v3",
       .a = authenticated_leaf("a", "one"),
       .b = authenticated_leaf("b", "two"),
       .c = authenticated_leaf("c", "three"),
       .d = authenticated_leaf("d", "four"),
   };
   fixture.tree_domain = authenticated::canonical_tree_domain(fixture.domain, authenticated::proof_tree::state);
   fixture.left = {
       .height = 1U,
       .size = 2U,
       .min_key = fixture.a.key,
       .max_key = fixture.b.key,
       .separator = fixture.b.key,
       .left_hash = authenticated_leaf_hash(fixture.tree_domain, fixture.a),
       .right_hash = authenticated_leaf_hash(fixture.tree_domain, fixture.b),
   };
   fixture.right = {
       .height = 1U,
       .size = 2U,
       .min_key = fixture.c.key,
       .max_key = fixture.d.key,
       .separator = fixture.d.key,
       .left_hash = authenticated_leaf_hash(fixture.tree_domain, fixture.c),
       .right_hash = authenticated_leaf_hash(fixture.tree_domain, fixture.d),
   };
   fixture.root = {
       .version = 42U,
       .state_root = authenticated::hash_inner(fixture.tree_domain, 2U, 4U, fixture.a.key, fixture.d.key, fixture.c.key,
                                               authenticated_branch_hash(fixture.tree_domain, fixture.left),
                                               authenticated_branch_hash(fixture.tree_domain, fixture.right)),
       .state_size = 4U,
       .change_root = authenticated::hash_empty(
           authenticated::canonical_tree_domain(fixture.domain, authenticated::proof_tree::changes)),
   };
   return fixture;
}

authenticated::range_proof authenticated_ranked_proof(const authenticated_ranked_fixture& fixture,
                                                      authenticated::range_request request) {
   return {
       .anchor = fixture.root,
       .request = std::move(request),
       .nodes =
           {
               authenticated::range_inner{
                   .height = 2U,
                   .size = 4U,
                   .min_key = fixture.a.key,
                   .max_key = fixture.d.key,
                   .separator = fixture.c.key,
               },
               authenticated::range_inner{
                   .height = fixture.left.height,
                   .size = fixture.left.size,
                   .min_key = fixture.left.min_key,
                   .max_key = fixture.left.max_key,
                   .separator = fixture.left.separator,
               },
               fixture.a,
               fixture.b,
               authenticated::range_inner{
                   .height = fixture.right.height,
                   .size = fixture.right.size,
                   .min_key = fixture.right.min_key,
                   .max_key = fixture.right.max_key,
                   .separator = fixture.right.separator,
               },
               fixture.c,
               fixture.d,
           },
   };
}

struct authenticated_changes_fixture {
   std::string domain;
   authenticated::mutation erased;
   authenticated::mutation updated;
   authenticated::proof_leaf erased_leaf;
   authenticated::proof_leaf updated_leaf;
   authenticated::root root;
};

authenticated_changes_fixture make_authenticated_changes_fixture() {
   auto fixture = authenticated_changes_fixture{
       .domain = "forge.test.chain-api.authenticated-changes.v3",
       .erased = authenticated::mutation{.key = authenticated_bytes("alpha")},
       .updated =
           authenticated::mutation{
               .key = authenticated_bytes("beta"),
               .value = authenticated_bytes("updated"),
           },
   };
   fixture.erased_leaf = authenticated_change_leaf(fixture.erased);
   fixture.updated_leaf = authenticated_change_leaf(fixture.updated);
   const auto state_domain = authenticated::canonical_tree_domain(fixture.domain, authenticated::proof_tree::state);
   const auto change_domain = authenticated::canonical_tree_domain(fixture.domain, authenticated::proof_tree::changes);
   fixture.root = {
       .version = 43U,
       .state_root = authenticated::hash_empty(state_domain),
       .change_root = authenticated::hash_inner(change_domain, 1U, 2U, fixture.erased_leaf.key,
                                                fixture.updated_leaf.key, fixture.updated_leaf.key,
                                                authenticated_leaf_hash(change_domain, fixture.erased_leaf),
                                                authenticated_leaf_hash(change_domain, fixture.updated_leaf)),
       .change_count = 2U,
   };
   return fixture;
}

authenticated::range_proof authenticated_changes_proof(const authenticated_changes_fixture& fixture,
                                                       authenticated::range_request request) {
   return {
       .anchor = fixture.root,
       .tree = authenticated::proof_tree::changes,
       .request = std::move(request),
       .nodes =
           {
               authenticated::range_inner{
                   .height = 1U,
                   .size = 2U,
                   .min_key = fixture.erased_leaf.key,
                   .max_key = fixture.updated_leaf.key,
                   .separator = fixture.updated_leaf.key,
               },
               fixture.erased_leaf,
               fixture.updated_leaf,
           },
   };
}

const route& find_route(const std::vector<route>& routes, std::string_view name) {
   const auto result = std::ranges::find(routes, name, &route::method_name);
   BOOST_REQUIRE(result != routes.end());
   return *result;
}

void require_routes(const std::vector<route>& routes, method verb, std::initializer_list<std::string_view> names) {
   for (const auto name : names) {
      const auto& value = find_route(routes, name);
      BOOST_TEST(value.verb == verb);
      if (verb == method::get) {
         BOOST_TEST(static_cast<int>(value.cache) == static_cast<int>(cache_policy::no_store));
      }
   }
}

void require_audited_get_finality_anchor(const std::vector<route>& routes) {
   for (const auto& value : routes) {
      if (value.verb == method::get && value.target.find("audit={audit}") != std::string::npos) {
         BOOST_TEST(value.target.find("finality_from={finality_from}") != std::string::npos);
      }
   }
}

std::string openapi_verb(method value) {
   switch (value) {
   case method::delete_:
      return "delete";
   case method::get:
      return "get";
   case method::head:
      return "head";
   case method::options:
      return "options";
   case method::patch:
      return "patch";
   case method::post:
      return "post";
   case method::put:
      return "put";
   case method::unknown:
      return {};
   }
   return {};
}

std::string openapi_path(const route& value) {
   const auto query = value.target.find('?');
   return value.target.substr(0U, query);
}

template <typename Owner> std::vector<route> owner_routes() {
   return forge::api::http::traits<Owner>::routes();
}

template <typename Owner> forge::variant owner_openapi() {
   return forge::api::http::openapi<Owner>();
}

template <typename Owner> forge::api::core::descriptor owner_descriptor() {
   return Owner::describe();
}

struct owner_openapi_contract {
   std::string_view name;
   std::vector<route> (*routes)();
   forge::variant (*document)();
   forge::api::core::descriptor (*describe)();
};

} // namespace

BOOST_AUTO_TEST_CASE(table_key_codec_is_canonical_and_validates_index_contract) {
   using forge::chain::api::encode_table_key;
   using forge::chain::api::validate_table_index;
   using forge::chain::api::validate_table_key;
   using forge::chain::api::exceptions::invalid_request;
   namespace protocol = forge::chain::protocol;

   BOOST_TEST(encode_table_key(std::uint64_t{0x0102030405060708ULL}) ==
              (protocol::bytes{0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U}));
   BOOST_TEST(encode_table_key(-0.0) == encode_table_key(0.0));
   BOOST_TEST(encode_table_key(-1.0) < encode_table_key(0.0));
   BOOST_TEST(encode_table_key(0.0) < encode_table_key(1.0));
   BOOST_CHECK_THROW((void)encode_table_key(std::numeric_limits<double>::quiet_NaN()), invalid_request);

   auto negative_zero128 = std::array<std::uint8_t, 16>{};
   negative_zero128.front() = 0x80U;
   const auto positive_zero128 = std::array<std::uint8_t, 16>{};
   BOOST_TEST(encode_table_key(std::span<const std::uint8_t, 16>{negative_zero128}) ==
              encode_table_key(std::span<const std::uint8_t, 16>{positive_zero128}));
   auto nan128 = std::array<std::uint8_t, 16>{};
   nan128[0] = 0x7fU;
   nan128[1] = 0xffU;
   nan128.back() = 0x01U;
   BOOST_CHECK_THROW((void)encode_table_key(std::span<const std::uint8_t, 16>{nan128}), invalid_request);

   BOOST_CHECK_NO_THROW(validate_table_index({.kind = protocol::table_index_kind::primary, .position = 0U}));
   BOOST_CHECK_THROW(validate_table_index({.kind = protocol::table_index_kind::primary, .position = 1U}),
                     invalid_request);
   BOOST_CHECK_NO_THROW(validate_table_index({.kind = protocol::table_index_kind::secondary_u64, .position = 15U}));
   BOOST_CHECK_THROW(validate_table_index({.kind = protocol::table_index_kind::secondary_u64, .position = 16U}),
                     invalid_request);
   BOOST_CHECK_NO_THROW(validate_table_key(protocol::table_index_kind::secondary_u128, std::array<std::uint8_t, 16>{}));
   BOOST_CHECK_THROW(validate_table_key(protocol::table_index_kind::secondary_u128, std::array<std::uint8_t, 8>{}),
                     invalid_request);
}

BOOST_AUTO_TEST_CASE(chain_http_uses_resource_verbs) {
   const auto info = forge::api::http::traits<forge::chain::api::info>::routes();
   const auto blocks = forge::api::http::traits<forge::chain::api::block>::routes();
   const auto state = forge::api::http::traits<forge::chain::api::state>::routes();
   const auto transactions = forge::api::http::traits<forge::chain::api::transaction>::routes();
   const auto submissions = forge::api::http::traits<forge::chain::api::submission>::routes();
   const auto admin = forge::api::http::traits<forge::chain::api::admin>::routes();

   require_routes(info, method::get, {"get"});
   require_routes(blocks, method::get,
                  {"get_block", "get_header", "get_block_state", "get_canonical_range",
                   "get_activated_protocol_features", "get_consensus_parameters", "get_producers",
                   "get_producer_rewards", "get_producer_schedule", "get_finalizer_info"});
   require_routes(state, method::get,
                  {"get_account", "get_code", "get_permission_links", "get_table_rows", "get_table_scope",
                   "get_currency_balance", "get_currency_stats", "get_scheduled_transactions"});
   require_routes(state, method::post, {"get_account_changes", "get_table_changes", "get_accounts_by_authorizers"});
   require_routes(transactions, method::get, {"get_status", "await_transaction"});
   require_routes(transactions, method::post,
                  {"get_required_keys", "compute_transaction", "send_read_only_transaction"});
   require_routes(submissions, method::post, {"submit", "submit_batch"});
   require_routes(admin, method::get,
                  {"producer_status", "get_finalizer_status", "get_operator_identity", "get_node_status",
                   "supported_protocol_features", "account_ram_corrections", "unapplied_transactions",
                   "snapshot_status", "snapshot_requests", "integrity_hash"});
   require_routes(admin, method::post,
                  {"push_block", "create_snapshot", "request_snapshot", "prune", "schedule_snapshot"});
   require_routes(admin, method::put, {"configure_pause", "set_access_policy", "schedule_protocol_features"});
   require_routes(admin, method::patch, {"update_runtime_options", "update_greylist"});
   require_routes(admin, method::delete_, {"unschedule_snapshot"});

   require_audited_get_finality_anchor(info);
   require_audited_get_finality_anchor(blocks);
   require_audited_get_finality_anchor(state);
   require_audited_get_finality_anchor(transactions);
}

BOOST_AUTO_TEST_CASE(chain_block_info_admin_contracts_are_version_2) {
   const auto block = forge::chain::api::block::describe();
   const auto info = forge::chain::api::info::describe();
   const auto admin = forge::chain::api::admin::describe();
   BOOST_TEST(block.version.major == 2U);
   BOOST_TEST(block.version.revision == 2U);
   BOOST_TEST(info.version.major == 2U);
   BOOST_TEST(info.version.revision == 0U);
   BOOST_TEST(admin.version.major == 2U);
   BOOST_TEST(admin.version.revision == 4U);
   BOOST_TEST(forge::chain::api::block::ref().major == 2U);
   BOOST_TEST(forge::chain::api::block::ref().min_revision == 2U);
   BOOST_TEST(forge::chain::api::info::ref().major == 2U);
   BOOST_TEST(forge::chain::api::admin::ref().major == 2U);
}

BOOST_AUTO_TEST_CASE(chain_snapshot_lifecycle_descriptor_and_codecs_are_revision_2_compatible) {
   auto head = forge::chain::protocol::block_id{};
   head._hash[0] = 0x42U;
   const auto created = forge::chain::protocol::snapshot_response{
       .name = "snapshot-a",
       .head = head,
       .head_num = 42U,
   };
   const auto request = forge::chain::protocol::snapshot_request{
       .request_id = "request-42",
       .name = "snapshot-a",
   };
   const auto status_request = forge::chain::protocol::snapshot_status_request{.request_id = request.request_id};
   const auto lifecycle = forge::chain::protocol::snapshot_lifecycle_response{
       .request_id = request.request_id,
       .name = request.name,
       .state = forge::chain::protocol::snapshot_state::completed,
       .head = head,
       .head_num = 42U,
   };

   const auto descriptor = forge::chain::api::admin::describe();
   const auto* create = forge::api::core::find_method(descriptor, "create_snapshot");
   const auto* request_method = forge::api::core::find_method(descriptor, "request_snapshot");
   const auto* status = forge::api::core::find_method(descriptor, "snapshot_status");
   BOOST_REQUIRE(create != nullptr);
   BOOST_REQUIRE(request_method != nullptr);
   BOOST_REQUIRE(status != nullptr);
   BOOST_TEST(descriptor.version.revision == 4U);
   BOOST_TEST(request_method->since_revision == 2U);
   BOOST_TEST(status->since_revision == 2U);
   BOOST_CHECK(create->response_type == std::type_index{typeid(forge::chain::protocol::snapshot_response)});
   BOOST_CHECK(request_method->request_type == std::type_index{typeid(forge::chain::protocol::snapshot_request)});
   BOOST_CHECK(request_method->response_type ==
               std::type_index{typeid(forge::chain::protocol::snapshot_lifecycle_response)});
   BOOST_CHECK(status->request_type == std::type_index{typeid(forge::chain::protocol::snapshot_status_request)});
   BOOST_CHECK(status->response_type == std::type_index{typeid(forge::chain::protocol::snapshot_lifecycle_response)});

   const auto old_snapshot_wire = forge::codec::hex::decode(
       "0a736e617073686f742d6142000000000000000000000000000000000000000000000000000000000000002a000000");
   BOOST_CHECK(forge::raw::pack(created) == old_snapshot_wire);
   BOOST_CHECK(forge::raw::unpack_exact<forge::chain::protocol::snapshot_response>(old_snapshot_wire) == created);
   BOOST_CHECK(forge::raw::unpack_exact<forge::chain::protocol::snapshot_request>(forge::raw::pack(request)) ==
               request);
   BOOST_CHECK(forge::raw::unpack_exact<forge::chain::protocol::snapshot_status_request>(
                   forge::raw::pack(status_request)) == status_request);
   BOOST_CHECK(forge::raw::unpack_exact<forge::chain::protocol::snapshot_lifecycle_response>(
                   forge::raw::pack(lifecycle)) == lifecycle);

   const auto created_json = forge::codec::json::write(created);
   const auto lifecycle_request_json = forge::codec::json::write(request);
   const auto status_request_json = forge::codec::json::write(status_request);
   const auto response_json = forge::codec::json::write(lifecycle);
   BOOST_REQUIRE(created_json.ok());
   BOOST_REQUIRE(lifecycle_request_json.ok());
   BOOST_REQUIRE(status_request_json.ok());
   BOOST_REQUIRE(response_json.ok());
   BOOST_CHECK(created_json.text.find("\"state\"") == std::string::npos);
   const auto json_options = forge::codec::json::read_options{
       .unknown_fields = forge::codec::json::unknown_field_policy::error,
   };
   const auto created_round_trip =
       forge::codec::json::read<forge::chain::protocol::snapshot_response>(created_json.text, json_options);
   const auto lifecycle_request_round_trip =
       forge::codec::json::read<forge::chain::protocol::snapshot_request>(lifecycle_request_json.text, json_options);
   const auto status_request_round_trip = forge::codec::json::read<forge::chain::protocol::snapshot_status_request>(
       status_request_json.text, json_options);
   const auto response_round_trip =
       forge::codec::json::read<forge::chain::protocol::snapshot_lifecycle_response>(response_json.text, json_options);
   BOOST_REQUIRE(created_round_trip.ok());
   BOOST_REQUIRE(lifecycle_request_round_trip.ok());
   BOOST_REQUIRE(status_request_round_trip.ok());
   BOOST_REQUIRE(response_round_trip.ok());
   BOOST_CHECK(created_round_trip.value == created);
   BOOST_CHECK(lifecycle_request_round_trip.value == request);
   BOOST_CHECK(status_request_round_trip.value == status_request);
   BOOST_CHECK(response_round_trip.value == lifecycle);
}

BOOST_AUTO_TEST_CASE(chain_block_descriptor_declares_not_found_only_for_entity_lookups) {
   const auto descriptor = forge::chain::api::block::describe();
   const auto not_found = forge::api::core::exception_identity<forge::chain::api::exceptions::not_found>();

   for (const auto name : {"get_block", "get_header", "get_block_state", "get_producers", "get_producer_rewards"}) {
      const auto* method = forge::api::core::find_method(descriptor, name);
      BOOST_REQUIRE(method != nullptr);
      BOOST_CHECK(std::ranges::find(method->errors, not_found, &forge::api::core::error_descriptor::identity) !=
                  method->errors.end());
   }
   for (const auto name : {"get_canonical_range", "get_activated_protocol_features", "get_consensus_parameters",
                           "get_producer_schedule", "get_finalizer_info"}) {
      const auto* method = forge::api::core::find_method(descriptor, name);
      BOOST_REQUIRE(method != nullptr);
      BOOST_CHECK(std::ranges::find(method->errors, not_found, &forge::api::core::error_descriptor::identity) ==
                  method->errors.end());
   }
}

BOOST_AUTO_TEST_CASE(chain_producer_rewards_verify_one_finalized_anchor_and_canonical_table_sources) {
   namespace protocol = forge::chain::protocol;
   namespace api = forge::chain::api;

   const auto producer = protocol::account_name{"alice"};
   const auto core_symbol = protocol::symbol_code{"SYS"};
   const auto anchor_header = protocol::block_header{
       .timestamp = protocol::block_timestamp{2'000U},
       .producer = protocol::account_name{"eosio"},
   };
   const auto anchor = protocol::state_anchor{
       .block = protocol::calculate_block_id(anchor_header),
       .block_num = protocol::calculate_block_num(anchor_header),
   };
   const auto context = protocol::response_context{
       .chain = anchor.chain,
       .head = anchor.block,
       .finalized = anchor.block,
       .anchor = anchor,
   };
   const auto producer_row = protocol::producer_info{
       .owner = producer,
       .is_active = true,
       .unpaid_blocks = 17U,
       .last_claim_time = protocol::time_point{protocol::microseconds{100U}},
   };
   const auto claimable = protocol::asset{55'000, protocol::symbol{core_symbol, 4U}};
   const auto reward_row = protocol::bpay_reward{.owner = producer, .quantity = claimable};
   const auto stored_producer = protocol::primary_value{
       .payer = protocol::account_name{"eosio"},
       .row = forge::raw::pack(producer_row),
   };
   const auto stored_reward = protocol::primary_value{
       .payer = protocol::account_name{"eosio.bpay"},
       .row = forge::raw::pack(reward_row),
   };
   const auto source_response = [&](const protocol::primary_value& stored, std::string scheme) {
      auto response = protocol::table_rows_response{};
      response.context = context;
      response.audit = protocol::audit_bundle{
          .state = {protocol::proof_blob{.scheme = std::move(scheme)}},
      };
      response.rows = {{.value = stored.row, .payer = stored.payer}};
      return response;
   };
   const auto to_bytes = [](const protocol::commitment_bytes& value) {
      auto result = protocol::bytes{};
      result.reserve(value.size());
      for (const auto byte : value) {
         result.push_back(std::to_integer<std::uint8_t>(byte));
      }
      return result;
   };
   const auto source_range = [&](protocol::contract_table_location location, std::uint64_t primary,
                                 const protocol::primary_value& stored) {
      return api::authenticated_state_range{
          .rows = {{
              .key =
                  to_bytes(protocol::contract_primary_key(location, protocol::contract_table_family::primary, primary)),
              .value = forge::raw::pack(stored),
          }},
      };
   };
   const auto producer_location = protocol::contract_table_location{
       .code = protocol::account_name{"eosio"}.value,
       .scope = protocol::name{"eosio"}.value,
       .table = protocol::table_name{"producers"}.value,
   };
   const auto bpay_location = protocol::contract_table_location{
       .code = protocol::account_name{"eosio.bpay"}.value,
       .scope = protocol::name{"eosio.bpay"}.value,
       .table = protocol::table_name{"rewards"}.value,
   };
   const auto request = protocol::producer_rewards_request{
       .producer = producer,
       .anchor = anchor.block,
       .audit = protocol::audit_mode::required,
   };
   const auto reward = protocol::project_producer_reward(producer_row, std::optional{reward_row},
                                                         anchor_header.timestamp.to_time_point());
   BOOST_REQUIRE(reward.has_value());
   auto response = protocol::producer_rewards_response{};
   response.context = context;
   response.audit = protocol::audit_bundle{
       .finality = protocol::proof_blob{.scheme = "test.finality"},
   };
   response.anchor_header = anchor_header;
   response.reward = *reward;
   response.system = source_response(stored_producer, "test.system");
   response.bpay = source_response(stored_reward, "test.bpay");

   auto service = std::make_shared<block_service>(protocol::block_response{});
   service->rewards_response = response;
   auto registry = forge::api::core::registry{};
   registry.install<api::block>(api::block::describe(), service);
   auto verifier = std::make_shared<accepting_audit_verifier>();
   verifier->preferred_anchor = anchor.block;
   verifier->state_ranges = {
       source_range(producer_location, producer.value, stored_producer),
       source_range(bpay_location, producer.value, stored_reward),
   };
   auto client = api::verified_client{
       api::raw_client{api::service_handles{.blocks = registry.get<api::block>(api::block::ref())}}, verifier,
       api::make_contract_table_projection_verifier()};

   BOOST_CHECK(run(client.get_producer_rewards(request)) == response);
   BOOST_CHECK_EQUAL(verifier->state_range_verifications, 2U);

   auto claimed = response;
   claimed.reward.bpay.claimable.reset();
   claimed.bpay.rows.clear();
   service->rewards_response = claimed;
   verifier->state_range_verifications = 0U;
   verifier->state_ranges = {
       source_range(producer_location, producer.value, stored_producer),
       api::authenticated_state_range{},
   };
   BOOST_CHECK(run(client.get_producer_rewards(request)) == claimed);

   service->rewards_response = response;
   ++service->rewards_response.reward.bpay.claimable->amount;
   verifier->state_range_verifications = 0U;
   verifier->state_ranges = {
       source_range(producer_location, producer.value, stored_producer),
       source_range(bpay_location, producer.value, stored_reward),
   };
   BOOST_CHECK_THROW(run(client.get_producer_rewards(request)), api::exceptions::invalid_state_proof);

   service->rewards_response = response;
   service->rewards_response.reward.system.contract = protocol::account_name{"eosio.bpay"};
   verifier->state_range_verifications = 0U;
   verifier->state_ranges = {
       source_range(producer_location, producer.value, stored_producer),
       source_range(bpay_location, producer.value, stored_reward),
   };
   BOOST_CHECK_THROW(run(client.get_producer_rewards(request)), api::exceptions::invalid_state_proof);

   service->rewards_response = response;
   service->rewards_response.reward.bpay.contract = protocol::account_name{"eosio"};
   verifier->state_range_verifications = 0U;
   verifier->state_ranges = {
       source_range(producer_location, producer.value, stored_producer),
       source_range(bpay_location, producer.value, stored_reward),
   };
   BOOST_CHECK_THROW(run(client.get_producer_rewards(request)), api::exceptions::invalid_state_proof);

   service->rewards_response = response;
   ++service->rewards_response.anchor_header.timestamp.slot;
   BOOST_CHECK_THROW(run(client.get_producer_rewards(request)), api::exceptions::invalid_state_proof);

   const auto malformed_reward = protocol::bpay_reward{
       .owner = protocol::account_name{"bob"},
       .quantity = claimable,
   };
   const auto stored_malformed_reward = protocol::primary_value{
       .payer = protocol::account_name{"eosio.bpay"},
       .row = forge::raw::pack(malformed_reward),
   };
   service->rewards_response = response;
   service->rewards_response.bpay = source_response(stored_malformed_reward, "test.bpay");
   verifier->state_range_verifications = 0U;
   verifier->state_ranges = {
       source_range(producer_location, producer.value, stored_producer),
       source_range(bpay_location, producer.value, stored_malformed_reward),
   };
   BOOST_CHECK_THROW(run(client.get_producer_rewards(request)), api::exceptions::invalid_state_proof);
}

BOOST_AUTO_TEST_CASE(chain_producer_reward_projection_preserves_bpay_absence_and_rejects_malformed_rows) {
   namespace protocol = forge::chain::protocol;

   const auto producer = protocol::producer_info{
       .owner = protocol::account_name{"alice"},
       .is_active = true,
       .unpaid_blocks = 17U,
       .last_claim_time = protocol::time_point{protocol::microseconds{100U}},
   };
   const auto anchor_time = protocol::time_point{protocol::days(1) + protocol::microseconds{101U}};
   const auto absent = protocol::project_producer_reward(producer, std::nullopt, anchor_time);
   BOOST_REQUIRE(absent.has_value());
   BOOST_CHECK(absent->system.eligible);
   BOOST_CHECK(absent->system.active);
   BOOST_CHECK(absent->system.next_claim_time == anchor_time);
   BOOST_CHECK(absent->system.contract == protocol::account_name{"eosio"});
   BOOST_CHECK(absent->system.claim_action == protocol::action_name{"claimrewards"});
   BOOST_CHECK(!absent->bpay.claimable.has_value());
   BOOST_CHECK(absent->bpay.contract == protocol::account_name{"eosio.bpay"});
   BOOST_CHECK(absent->bpay.claim_action == protocol::action_name{"claimrewards"});

   const auto wrong_owner = protocol::bpay_reward{
       .owner = protocol::account_name{"bob"},
       .quantity = protocol::asset{1, protocol::make_symbol("SYS", 4U)},
   };
   BOOST_CHECK(!protocol::project_producer_reward(producer, wrong_owner, anchor_time).has_value());

   const auto invalid_quantity = protocol::bpay_reward{
       .owner = producer.owner,
       .quantity = protocol::asset{1},
   };
   BOOST_CHECK(!protocol::project_producer_reward(producer, invalid_quantity, anchor_time).has_value());

   auto overflow = producer;
   overflow.last_claim_time = protocol::time_point{protocol::microseconds{std::numeric_limits<std::int64_t>::max()}};
   BOOST_CHECK(!protocol::project_producer_reward(overflow, std::nullopt, anchor_time).has_value());
}

BOOST_AUTO_TEST_CASE(chain_producers_http_roundtrip_preserves_typed_not_found) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto service = std::make_shared<block_service>(forge::chain::protocol::block_response{});
   service->throw_producers_not_found = true;

   auto apis = forge::api::core::registry{};
   apis.install<forge::chain::api::block>(forge::chain::api::block::describe(), service);

   auto router = forge::net::http::router{};
   router.mount(forge::api::http::binding()
                    .use(forge::api::core::binding().serve(apis).build())
                    .bind<forge::chain::api::block>()
                    .build());

   auto server = forge::net::http::server{runtime, forge::net::http::server_config{}, std::move(router)};
   server.start();
   try {
      auto client = forge::net::http::client{
          runtime,
          forge::net::http::parse_base_url("http://127.0.0.1:" + std::to_string(server.port())),
      };
      auto remote = forge::asio::blocking::run(runtime, forge::api::http::remote<forge::chain::api::block>(client));
      BOOST_CHECK_THROW(
          forge::asio::blocking::run(runtime, remote->get_producers(forge::chain::protocol::producers_request{})),
          forge::chain::api::exceptions::not_found);
   } catch (...) {
      server.stop();
      throw;
   }
   server.stop();
}

BOOST_AUTO_TEST_CASE(chain_snapshot_http_roundtrip_preserves_lifecycle_and_typed_not_found) {
   auto head = forge::chain::protocol::block_id{};
   head._hash[0] = 0x42U;
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto service = std::make_shared<snapshot_admin_service>(forge::chain::protocol::snapshot_response{
       .name = "snapshot-a",
       .head = head,
       .head_num = 42U,
   });

   auto apis = forge::api::core::registry{};
   apis.install<forge::chain::api::admin>(forge::chain::api::admin::describe(), service);

   auto router = forge::net::http::router{};
   router.mount(forge::api::http::binding()
                    .use(forge::api::core::binding().serve(apis).build())
                    .bind<forge::chain::api::admin>()
                    .build());

   auto server = forge::net::http::server{runtime, forge::net::http::server_config{}, std::move(router)};
   server.start();
   try {
      auto client = forge::net::http::client{
          runtime,
          forge::net::http::parse_base_url("http://127.0.0.1:" + std::to_string(server.port())),
      };
      auto remote = forge::asio::blocking::run(runtime, forge::api::http::remote<forge::chain::api::admin>(client));

      const auto created = forge::asio::blocking::run(runtime, remote->create_snapshot("snapshot-a"));
      BOOST_TEST(created.head == head);
      BOOST_TEST(created.head_num == 42U);

      const auto request = forge::chain::protocol::snapshot_request{
          .request_id = "request-42",
          .name = "snapshot-a",
      };
      const auto pending = forge::asio::blocking::run(runtime, remote->request_snapshot(request));
      BOOST_CHECK(pending.state == forge::chain::protocol::snapshot_state::pending);
      BOOST_TEST(pending.request_id == request.request_id);
      BOOST_REQUIRE(pending.head.has_value());
      BOOST_TEST(*pending.head == head);

      const auto replay = forge::asio::blocking::run(runtime, remote->request_snapshot(request));
      BOOST_CHECK(replay == pending);

      auto mismatched = request;
      mismatched.name = "snapshot-b";
      BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, remote->request_snapshot(mismatched)),
                        forge::chain::api::exceptions::conflict);

      service->lost_request_id = "lost-request";
      auto lost = request;
      lost.request_id = service->lost_request_id;
      BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, remote->request_snapshot(lost)),
                        forge::chain::api::exceptions::snapshot_lost);

      const auto status_request = forge::chain::protocol::snapshot_status_request{.request_id = request.request_id};
      service->state = forge::chain::protocol::snapshot_state::completed;
      const auto completed = forge::asio::blocking::run(runtime, remote->snapshot_status(status_request));
      BOOST_CHECK(completed.state == forge::chain::protocol::snapshot_state::completed);
      BOOST_TEST(completed.request_id == request.request_id);

      auto unknown = status_request;
      unknown.request_id = "unknown-request";
      BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, remote->snapshot_status(unknown)),
                        forge::chain::api::exceptions::not_found);
   } catch (...) {
      server.stop();
      throw;
   }
   server.stop();
}

BOOST_AUTO_TEST_CASE(chain_state_v3_declares_only_typed_state_reads_and_public_history_error) {
   const auto descriptor = forge::chain::api::state::describe();
   BOOST_TEST(descriptor.version.major == 3U);
   BOOST_TEST(descriptor.version.revision == 0U);
   BOOST_TEST(forge::api::core::find_method(descriptor, "get_point") == nullptr);
   BOOST_TEST(forge::api::core::find_method(descriptor, "get_range") == nullptr);
   BOOST_TEST(forge::api::core::find_method(descriptor, "get_changes") == nullptr);

   const auto history = forge::api::core::exception_identity<forge::chain::api::exceptions::history_unavailable>();
   const auto not_found = forge::api::core::exception_identity<forge::chain::api::exceptions::not_found>();
   for (const auto name : {"get_table_changes", "get_account_changes"}) {
      const auto* method = forge::api::core::find_method(descriptor, name);
      BOOST_REQUIRE(method != nullptr);
      BOOST_CHECK(std::ranges::find(method->errors, history, &forge::api::core::error_descriptor::identity) !=
                  method->errors.end());
      BOOST_CHECK(std::ranges::none_of(method->errors, [](const auto& error) { return error.name == "history_lost"; }));
   }
   for (const auto name : {"get_account", "get_code", "get_permission_links"}) {
      const auto* method = forge::api::core::find_method(descriptor, name);
      BOOST_REQUIRE(method != nullptr);
      BOOST_CHECK(std::ranges::find(method->errors, not_found, &forge::api::core::error_descriptor::identity) !=
                  method->errors.end());
   }

   for (const auto& [owner, name] : {std::pair{forge::chain::api::block::describe(), "get_canonical_range"},
                                     std::pair{forge::chain::api::transaction::describe(), "get_status"}}) {
      const auto* method = forge::api::core::find_method(owner, name);
      BOOST_REQUIRE(method != nullptr);
      BOOST_CHECK(std::ranges::find(method->errors, history, &forge::api::core::error_descriptor::identity) !=
                  method->errors.end());
      BOOST_CHECK(std::ranges::none_of(method->errors, [](const auto& error) { return error.name == "history_lost"; }));
   }
}

BOOST_AUTO_TEST_CASE(chain_audit_class_names_and_numeric_values_remain_stable) {
   using forge::chain::protocol::audit_class;
   BOOST_TEST(static_cast<std::uint8_t>(audit_class::none) == 0U);
   BOOST_TEST(static_cast<std::uint8_t>(audit_class::finality) == 1U);
   BOOST_TEST(static_cast<std::uint8_t>(audit_class::state_point) == 2U);
   BOOST_TEST(static_cast<std::uint8_t>(audit_class::state_range) == 3U);
   BOOST_TEST(static_cast<std::uint8_t>(audit_class::state_changes) == 4U);
   BOOST_TEST(static_cast<std::uint8_t>(audit_class::transaction_inclusion) == 5U);
   BOOST_TEST(static_cast<std::uint8_t>(audit_class::deterministic_composite) == 6U);
   BOOST_TEST(static_cast<std::uint8_t>(audit_class::unsupported) == 7U);
}

BOOST_AUTO_TEST_CASE(chain_api_limits_bound_canonical_request_and_response_bytes) {
   auto limits = forge::chain::protocol::service_limits{};
   auto request = forge::chain::protocol::table_changes_request{
       .from_block = 10U,
       .to_block = 11U,
       .tables = {{.code = forge::chain::protocol::account_name{"tester"},
                   .scope = forge::chain::protocol::name{"scope"},
                   .table = forge::chain::protocol::name{"rows"}}},
   };
   limits.max_request_bytes = static_cast<std::uint32_t>(forge::raw::pack_size(request));
   BOOST_CHECK_NO_THROW(forge::chain::api::require_request_within_limits(request, limits));
   --limits.max_request_bytes;
   BOOST_CHECK_THROW(forge::chain::api::require_request_within_limits(request, limits),
                     forge::chain::api::exceptions::resource_exhausted);

   auto response = forge::chain::protocol::table_changes_response{
       .blocks = {{.mutations = {{.table = request.tables.front(),
                                  .primary = 7U,
                                  .row = forge::chain::protocol::table_row{.value = {4U, 5U}}}}}},
   };
   limits.max_response_bytes = static_cast<std::uint32_t>(forge::raw::pack_size(response));
   BOOST_CHECK_NO_THROW(forge::chain::api::require_response_within_limits(response, limits));
   --limits.max_response_bytes;
   BOOST_CHECK_THROW(forge::chain::api::require_response_within_limits(response, limits),
                     forge::chain::api::exceptions::resource_exhausted);

   limits = forge::chain::protocol::service_limits{};
   auto table = forge::chain::protocol::table_rows_request{
       .index = {.kind = forge::chain::protocol::table_index_kind::secondary_u128, .position = 1U},
       .lower_bound = forge::chain::protocol::bytes(16U, 0U),
       .upper_bound = forge::chain::protocol::bytes(16U, 1U),
       .cursor = forge::chain::protocol::bytes{0xffU},
   };
   BOOST_CHECK_NO_THROW(forge::chain::api::require_request_within_limits(table, limits));
   table.limit = 0U;
   BOOST_CHECK_NO_THROW(forge::chain::api::require_request_within_limits(table, limits));
   table.limit = 10U;
   table.lower_bound = forge::chain::protocol::bytes(8U, 0U);
   BOOST_CHECK_THROW(forge::chain::api::require_request_within_limits(table, limits),
                     forge::chain::api::exceptions::invalid_request);

   auto account_changes = forge::chain::protocol::account_changes_request{
       .from_block = 10U,
       .to_block = 11U,
       .accounts = {forge::chain::protocol::account_name{"alice"}},
       .limit = limits.max_page_size + 1U,
   };
   BOOST_CHECK_THROW(forge::chain::api::require_request_within_limits(account_changes, limits),
                     forge::chain::api::exceptions::resource_exhausted);

   account_changes.limit = 1U;
   account_changes.accounts = {
       forge::chain::protocol::account_name{"bob"},
       forge::chain::protocol::account_name{"alice"},
   };
   BOOST_CHECK_THROW(forge::chain::api::require_request_within_limits(account_changes, limits),
                     forge::chain::api::exceptions::invalid_request);
   account_changes.accounts = {forge::chain::protocol::account_name{"alice"}};
   account_changes.cursor = forge::chain::protocol::bytes{};
   BOOST_CHECK_THROW(forge::chain::api::require_request_within_limits(account_changes, limits),
                     forge::chain::api::exceptions::invalid_request);

   table.lower_bound = forge::chain::protocol::bytes(16U, 0U);
   table.index.kind = static_cast<forge::chain::protocol::table_index_kind>(0xffU);
   BOOST_CHECK_THROW(forge::chain::api::require_request_within_limits(table, limits),
                     forge::chain::api::exceptions::invalid_request);

   auto table_changes = request;
   table_changes.limit = 1U;
   auto table_changes_response = forge::chain::protocol::table_changes_response{
       .blocks =
           {
               {.mutations = {{.table = request.tables.front()}}},
               {.mutations = {{.table = request.tables.front(), .primary = 1U}}},
           },
   };
   BOOST_CHECK_THROW(forge::chain::api::require_response_within_limits(table_changes_response, table_changes, limits),
                     forge::chain::api::exceptions::resource_exhausted);

   request.to_block = 9U;
   BOOST_CHECK_THROW(forge::chain::api::require_request_within_limits(request, limits),
                     forge::chain::api::exceptions::invalid_request);

   request.to_block = 11U;
   request.tables.push_back(request.tables.front());
   BOOST_CHECK_THROW(forge::chain::api::require_request_within_limits(request, limits),
                     forge::chain::api::exceptions::invalid_request);
   request.tables.resize(1U);
   request.cursor = forge::chain::protocol::bytes{};
   BOOST_CHECK_THROW(forge::chain::api::require_request_within_limits(request, limits),
                     forge::chain::api::exceptions::invalid_request);

   auto waiting = forge::chain::protocol::transaction_await_request{.timeout_ms = limits.max_await_ms + 1U};
   BOOST_CHECK_THROW(forge::chain::api::require_request_within_limits(waiting, limits),
                     forge::chain::api::exceptions::resource_exhausted);

   auto submission = forge::chain::protocol::transaction_submit_request{.timeout_ms = 0U};
   BOOST_CHECK_THROW(forge::chain::api::require_request_within_limits(submission, limits),
                     forge::chain::api::exceptions::invalid_request);
   submission.timeout_ms = limits.max_await_ms + 1U;
   BOOST_CHECK_THROW(forge::chain::api::require_request_within_limits(submission, limits),
                     forge::chain::api::exceptions::resource_exhausted);

   auto batch = forge::chain::protocol::transaction_submit_batch_request{
       .transactions = {forge::chain::protocol::transaction_submit_request{.timeout_ms = 2'000U}},
       .timeout_ms = 1'000U,
   };
   BOOST_CHECK_NO_THROW(forge::chain::api::require_request_within_limits(batch, limits));
   batch.timeout_ms = limits.max_await_ms + 1U;
   BOOST_CHECK_THROW(forge::chain::api::require_request_within_limits(batch, limits),
                     forge::chain::api::exceptions::resource_exhausted);

   response.audit = forge::chain::protocol::audit_bundle{
       .finality = forge::chain::protocol::proof_blob{.scheme = "test.finality", .payload = {1U, 2U, 3U}},
   };
   limits.max_response_bytes = std::numeric_limits<std::uint32_t>::max();
   limits.max_proof_bytes = static_cast<std::uint32_t>(forge::raw::pack_size(*response.audit) - 1U);
   BOOST_CHECK_THROW(forge::chain::api::require_response_within_limits(response, limits),
                     forge::chain::api::exceptions::resource_exhausted);
}

BOOST_AUTO_TEST_CASE(chain_api_limited_descriptor_enforces_owner_request_and_response_limits) {
   auto limits = forge::chain::protocol::service_limits{};
   limits.max_page_size = 4U;
   auto service = std::make_shared<state_service>(forge::chain::protocol::table_changes_response{
       .blocks =
           {
               {.mutations = {{.table = {.code = forge::chain::protocol::account_name{"tester"}}, .primary = 1U}}},
               {.mutations = {{.table = {.code = forge::chain::protocol::account_name{"tester"}}, .primary = 2U}}},
           },
   });
   const auto descriptor = forge::chain::api::limited_descriptor<forge::chain::api::state>(limits);
   const auto* method = forge::api::core::find_method(descriptor, "get_table_changes");
   BOOST_REQUIRE(method != nullptr);

   const auto request = forge::chain::protocol::table_changes_request{
       .from_block = 10U,
       .to_block = 11U,
       .tables = {{.code = forge::chain::protocol::account_name{"tester"}}},
       .limit = 1U,
   };
   const auto request_bytes = forge::raw::pack(request);
   method->request_validator(request_bytes);
   const auto response_bytes = run(method->raw_invoker(service, request_bytes));
   BOOST_CHECK_THROW(method->response_validator(request_bytes, response_bytes),
                     forge::chain::api::exceptions::resource_exhausted);

   auto oversized = request;
   oversized.limit = limits.max_page_size + 1U;
   BOOST_CHECK_THROW(method->request_validator(forge::raw::pack(oversized)),
                     forge::chain::api::exceptions::resource_exhausted);
}

BOOST_AUTO_TEST_CASE(chain_api_limited_descriptor_rejects_malformed_and_unbounded_submission_deadlines) {
   auto limits = forge::chain::protocol::service_limits{};
   limits.max_await_ms = 2'000U;
   const auto descriptor = forge::chain::api::limited_descriptor<forge::chain::api::submission>(limits);
   const auto* submit = forge::api::core::find_method(descriptor, "submit");
   const auto* submit_batch = forge::api::core::find_method(descriptor, "submit_batch");
   BOOST_REQUIRE(submit != nullptr);
   BOOST_REQUIRE(submit_batch != nullptr);

   auto valid = forge::raw::pack(forge::chain::protocol::transaction_submit_request{.timeout_ms = 2'000U});
   BOOST_CHECK_NO_THROW(submit->request_validator(valid));
   valid.resize(valid.size() - sizeof(std::uint64_t));
   BOOST_CHECK_THROW(submit->request_validator(valid), forge::chain::api::exceptions::invalid_request);

   const auto over_limit = forge::raw::pack(forge::chain::protocol::transaction_submit_request{.timeout_ms = 2'001U});
   BOOST_CHECK_THROW(submit->request_validator(over_limit), forge::chain::api::exceptions::resource_exhausted);

   const auto bounded_batch = forge::raw::pack(forge::chain::protocol::transaction_submit_batch_request{
       .transactions = {forge::chain::protocol::transaction_submit_request{.timeout_ms = 1'500U}},
       .timeout_ms = 1'000U,
   });
   BOOST_CHECK_NO_THROW(submit_batch->request_validator(bounded_batch));
}

BOOST_AUTO_TEST_CASE(chain_api_producer_pagination_uses_bounded_nonempty_opaque_cursors) {
   auto limits = forge::chain::protocol::service_limits{};
   auto request = forge::chain::protocol::producers_request{
       .lower_bound = forge::chain::protocol::account_name{"alice"},
       .limit = 0U,
       .cursor = forge::chain::protocol::bytes{0x00U, 0xffU},
   };
   auto response = forge::chain::protocol::producers_response{
       .next = forge::chain::protocol::bytes{0x01U, 0x02U},
   };

   BOOST_CHECK_NO_THROW(forge::chain::api::require_request_within_limits(request, limits));
   BOOST_CHECK_NO_THROW(forge::chain::api::require_response_within_limits(response, request, limits));

   auto empty_cursor = request;
   empty_cursor.cursor = forge::chain::protocol::bytes{};
   BOOST_CHECK_THROW(forge::chain::api::require_request_within_limits(empty_cursor, limits),
                     forge::chain::api::exceptions::invalid_request);

   auto empty_next = response;
   empty_next.next = forge::chain::protocol::bytes{};
   BOOST_CHECK_THROW(forge::chain::api::require_response_within_limits(empty_next, request, limits),
                     forge::chain::api::exceptions::unavailable);

   auto invalid = response;
   invalid.rows.emplace_back();
   BOOST_CHECK_THROW(forge::chain::api::require_response_within_limits(invalid, request, limits),
                     forge::chain::api::exceptions::resource_exhausted);

   const auto descriptor = forge::chain::api::limited_descriptor<forge::chain::api::block>(limits);
   const auto* method = forge::api::core::find_method(descriptor, "get_producers");
   BOOST_REQUIRE(method != nullptr);
   BOOST_CHECK_NO_THROW(method->request_validator(forge::raw::pack(request)));
   BOOST_CHECK_NO_THROW(method->response_validator(forge::raw::pack(request), forge::raw::pack(response)));
   BOOST_CHECK_THROW(method->request_validator(forge::raw::pack(empty_cursor)),
                     forge::chain::api::exceptions::invalid_request);
   BOOST_CHECK_THROW(method->response_validator(forge::raw::pack(request), forge::raw::pack(empty_next)),
                     forge::chain::api::exceptions::unavailable);

   limits.max_request_bytes = static_cast<std::uint32_t>(forge::raw::pack_size(request));
   BOOST_CHECK_NO_THROW(forge::chain::api::require_request_within_limits(request, limits));
   --limits.max_request_bytes;
   BOOST_CHECK_THROW(forge::chain::api::require_request_within_limits(request, limits),
                     forge::chain::api::exceptions::resource_exhausted);

   limits = forge::chain::protocol::service_limits{};
   limits.max_response_bytes = static_cast<std::uint32_t>(forge::raw::pack_size(response));
   BOOST_CHECK_NO_THROW(forge::chain::api::require_response_within_limits(response, request, limits));
   --limits.max_response_bytes;
   BOOST_CHECK_THROW(forge::chain::api::require_response_within_limits(response, request, limits),
                     forge::chain::api::exceptions::resource_exhausted);
}

BOOST_AUTO_TEST_CASE(chain_api_limited_descriptor_only_decodes_audited_response_types) {
   auto limits = forge::chain::protocol::service_limits{};
   limits.max_container_elements = 2U;
   const auto descriptor = forge::chain::api::limited_descriptor<forge::chain::api::transaction>(limits);
   const auto* required_keys = forge::api::core::find_method(descriptor, "get_required_keys");
   const auto* status = forge::api::core::find_method(descriptor, "get_status");
   BOOST_REQUIRE(required_keys != nullptr);
   BOOST_REQUIRE(status != nullptr);
   BOOST_TEST(!required_keys->has_response_trait<forge::chain::protocol::audited_response>());
   BOOST_TEST(status->has_response_trait<forge::chain::protocol::audited_response>());

   const auto plain_response = forge::raw::pack(std::vector<forge::chain::protocol::public_key>{});
   BOOST_CHECK_NO_THROW(required_keys->response_validator(
       forge::raw::pack(forge::chain::protocol::transaction_required_keys_request{}), plain_response));
   BOOST_CHECK_THROW(status->response_validator(forge::raw::pack(forge::chain::protocol::transaction_status_request{}),
                                                plain_response),
                     forge::chain::api::exceptions::unavailable);

   const auto oversized_keys = forge::raw::pack(std::vector<forge::chain::protocol::public_key>(3U));
   BOOST_CHECK_THROW(required_keys->response_validator(
                         forge::raw::pack(forge::chain::protocol::transaction_required_keys_request{}), oversized_keys),
                     forge::chain::api::exceptions::resource_exhausted);

   auto status_with_oversized_tail = forge::chain::protocol::transaction_status_response{};
   status_with_oversized_tail.trace = forge::chain::protocol::transaction_trace{};
   status_with_oversized_tail.trace->actions.resize(3U);
   BOOST_CHECK_THROW(status->response_validator(forge::raw::pack(forge::chain::protocol::transaction_status_request{}),
                                                forge::raw::pack(status_with_oversized_tail)),
                     forge::chain::api::exceptions::resource_exhausted);
}

BOOST_AUTO_TEST_CASE(chain_api_limited_descriptor_rejects_declared_collections_before_allocation) {
   auto limits = forge::chain::protocol::service_limits{};
   limits.max_page_size = 1'024U;
   limits.max_transaction_batch_size = 2U;
   limits.max_container_elements = 4'096U;
   const auto descriptor = forge::chain::api::limited_descriptor<forge::chain::api::submission>(limits);
   const auto* method = forge::api::core::find_method(descriptor, "submit_batch");
   BOOST_REQUIRE(method != nullptr);

   const auto over_transaction_limit = forge::api::core::bytes{0x03U};
   BOOST_CHECK_THROW(method->request_validator(over_transaction_limit),
                     forge::chain::api::exceptions::resource_exhausted);

   const auto declared_million_items = forge::api::core::bytes{0x80U, 0x80U, 0x40U};
   BOOST_CHECK_THROW(method->request_validator(declared_million_items),
                     forge::chain::api::exceptions::resource_exhausted);
}

BOOST_AUTO_TEST_CASE(chain_api_limited_descriptor_bounds_admin_pages_and_response_cardinality) {
   auto limits = forge::chain::protocol::service_limits{};
   limits.max_page_size = 2U;
   const auto descriptor = forge::chain::api::limited_descriptor<forge::chain::api::admin>(limits);
   const auto* ram = forge::api::core::find_method(descriptor, "account_ram_corrections");
   const auto* unapplied = forge::api::core::find_method(descriptor, "unapplied_transactions");
   BOOST_REQUIRE(ram != nullptr);
   BOOST_REQUIRE(unapplied != nullptr);

   const auto oversized_ram = forge::raw::pack(forge::chain::protocol::ram_corrections_request{.limit = 3U});
   BOOST_CHECK_THROW(ram->request_validator(oversized_ram), forge::chain::api::exceptions::resource_exhausted);
   const auto ram_request = forge::raw::pack(forge::chain::protocol::ram_corrections_request{.limit = 1U});
   const auto ram_response = forge::raw::pack(forge::chain::protocol::ram_corrections_response{
       .rows = {forge::chain::protocol::account_ram_correction{}, forge::chain::protocol::account_ram_correction{}},
   });
   BOOST_CHECK_THROW(ram->response_validator(ram_request, ram_response),
                     forge::chain::api::exceptions::resource_exhausted);

   const auto oversized_unapplied =
       forge::raw::pack(forge::chain::protocol::unapplied_transactions_request{.limit = 3U});
   BOOST_CHECK_THROW(unapplied->request_validator(oversized_unapplied),
                     forge::chain::api::exceptions::resource_exhausted);
}

BOOST_AUTO_TEST_CASE(chain_api_limited_descriptor_rejects_authorizer_second_container_at_generic_decode_boundary) {
   const auto default_limits = forge::chain::protocol::service_limits{};
   BOOST_TEST(default_limits.max_state_batch_size == 128U);
   const auto default_descriptor = forge::chain::api::limited_descriptor<forge::chain::api::state>(default_limits);
   const auto* default_method = forge::api::core::find_method(default_descriptor, "get_accounts_by_authorizers");
   BOOST_REQUIRE(default_method != nullptr);

   auto cursor_request = forge::chain::protocol::authorizers_request{
       .accounts = {forge::chain::protocol::permission_level{}},
       .limit = 1U,
       .cursor = forge::chain::protocol::bytes(128U, 0x7fU),
   };
   BOOST_CHECK_NO_THROW(default_method->request_validator(forge::raw::pack(cursor_request)));

   cursor_request.keys.resize(default_limits.max_state_batch_size);
   BOOST_CHECK_THROW(default_method->request_validator(forge::raw::pack(cursor_request)),
                     forge::chain::api::exceptions::resource_exhausted);

   auto limits = forge::chain::protocol::service_limits{};
   limits.max_state_batch_size = 2U;
   limits.max_container_elements = 4'096U;
   const auto descriptor = forge::chain::api::limited_descriptor<forge::chain::api::state>(limits);
   const auto* method = forge::api::core::find_method(descriptor, "get_accounts_by_authorizers");
   BOOST_REQUIRE(method != nullptr);

   const auto request = forge::chain::protocol::authorizers_request{
       .accounts = {forge::chain::protocol::permission_level{}, forge::chain::protocol::permission_level{}},
       .keys = {forge::chain::protocol::public_key{}},
       .limit = 1U,
   };
   BOOST_CHECK_THROW(method->request_validator(forge::raw::pack(request)),
                     forge::chain::api::exceptions::resource_exhausted);

   // The payload ends at the second vector count: its elements must never be allocated or decoded.
   const auto truncated_after_second_count = forge::raw::pack(
       std::vector<forge::chain::protocol::permission_level>{forge::chain::protocol::permission_level{}},
       forge::unsigned_int{2U});
   BOOST_CHECK_THROW(method->request_validator(truncated_after_second_count),
                     forge::chain::api::exceptions::resource_exhausted);

   limits.max_state_batch_size = 0U;
   const auto zero_limit_descriptor = forge::chain::api::limited_descriptor<forge::chain::api::state>(limits);
   const auto* zero_limit_method = forge::api::core::find_method(zero_limit_descriptor, "get_accounts_by_authorizers");
   BOOST_REQUIRE(zero_limit_method != nullptr);
   const auto nonempty_request = forge::chain::protocol::authorizers_request{
       .accounts = {forge::chain::protocol::permission_level{}},
       .limit = 1U,
   };
   BOOST_CHECK_THROW(zero_limit_method->request_validator(forge::raw::pack(nonempty_request)),
                     forge::chain::api::exceptions::resource_exhausted);
}

BOOST_AUTO_TEST_CASE(chain_api_transaction_batch_response_requires_exact_cardinality) {
   const auto limits = forge::chain::protocol::service_limits{};
   BOOST_CHECK_THROW(forge::chain::api::require_transaction_batch_response_within_limits({}, 1U, limits),
                     forge::chain::api::exceptions::unavailable);
}

BOOST_AUTO_TEST_CASE(chain_openapi_covers_every_owner_contract_route_and_schema) {
   const auto owners = std::array{
       owner_openapi_contract{"info", &owner_routes<forge::chain::api::info>, &owner_openapi<forge::chain::api::info>,
                              &owner_descriptor<forge::chain::api::info>},
       owner_openapi_contract{"block", &owner_routes<forge::chain::api::block>,
                              &owner_openapi<forge::chain::api::block>, &owner_descriptor<forge::chain::api::block>},
       owner_openapi_contract{"state", &owner_routes<forge::chain::api::state>,
                              &owner_openapi<forge::chain::api::state>, &owner_descriptor<forge::chain::api::state>},
       owner_openapi_contract{"transaction", &owner_routes<forge::chain::api::transaction>,
                              &owner_openapi<forge::chain::api::transaction>,
                              &owner_descriptor<forge::chain::api::transaction>},
       owner_openapi_contract{"admin", &owner_routes<forge::chain::api::admin>,
                              &owner_openapi<forge::chain::api::admin>, &owner_descriptor<forge::chain::api::admin>},
   };
   auto operation_ids = std::set<std::string>{};

   for (const auto& owner : owners) {
      const auto routes = owner.routes();
      const auto document = owner.document();
      const auto descriptor = owner.describe();
      BOOST_TEST(document["openapi"].as_string() == "3.1.0");

      const auto& paths = document["paths"].get_object();
      auto expected_paths = std::set<std::string>{};
      auto expected_operations = std::set<std::pair<std::string, std::string>>{};
      for (const auto& mapping : routes) {
         const auto* method_descriptor = forge::api::core::find_method(descriptor, mapping.method_name);
         BOOST_REQUIRE_MESSAGE(method_descriptor != nullptr,
                               owner.name << "." << mapping.method_name << " has a method descriptor");
         BOOST_REQUIRE_MESSAGE(!method_descriptor->errors.empty(),
                               owner.name << "." << mapping.method_name << " declares typed errors");
         const auto resource_identity =
             forge::api::core::exception_identity<forge::chain::api::exceptions::resource_exhausted>();
         BOOST_REQUIRE_MESSAGE(std::ranges::find(method_descriptor->errors, resource_identity,
                                                 &forge::api::core::error_descriptor::identity) !=
                                   method_descriptor->errors.end(),
                               owner.name << "." << mapping.method_name << " declares resource exhaustion");
         const auto path = openapi_path(mapping);
         const auto verb = openapi_verb(mapping.verb);
         BOOST_REQUIRE_MESSAGE(!verb.empty(), owner.name << "." << mapping.method_name << " has an HTTP verb");
         expected_paths.insert(path);
         BOOST_REQUIRE_MESSAGE(expected_operations.emplace(path, verb).second,
                               owner.name << "." << mapping.method_name << " has a unique path and verb");
      }

      BOOST_TEST(paths.size() == expected_paths.size());
      auto documented_operations = std::size_t{};
      for (const auto& path : paths) {
         documented_operations += path.value().get_object().size();
      }
      BOOST_TEST(documented_operations == expected_operations.size());

      for (const auto& mapping : routes) {
         const auto* method_descriptor = forge::api::core::find_method(descriptor, mapping.method_name);
         BOOST_REQUIRE_MESSAGE(method_descriptor != nullptr,
                               owner.name << "." << mapping.method_name << " has a method descriptor");
         const auto path = openapi_path(mapping);
         const auto verb = openapi_verb(mapping.verb);
         const auto path_entry = paths.find(path);
         BOOST_REQUIRE_MESSAGE(path_entry != paths.end(), owner.name << "." << mapping.method_name << " path exists");
         const auto& methods = path_entry->value().get_object();
         const auto method_entry = methods.find(verb);
         BOOST_REQUIRE_MESSAGE(method_entry != methods.end(),
                               owner.name << "." << mapping.method_name << " verb exists");
         const auto& operation = method_entry->value().get_object();

         const auto operation_id = operation["operationId"].as_string();
         BOOST_REQUIRE_MESSAGE(!operation_id.empty(), owner.name << "." << mapping.method_name << " has operationId");
         BOOST_CHECK_MESSAGE(operation_ids.insert(operation_id).second,
                             owner.name << "." << mapping.method_name << " operationId is unique");

         const auto& responses = operation["responses"].get_object();
         const auto success_status = std::to_string(static_cast<unsigned>(mapping.success_status));
         const auto success_entry = responses.find(success_status);
         BOOST_REQUIRE_MESSAGE(success_entry != responses.end(),
                               owner.name << "." << mapping.method_name << " has its success response");
         const auto& success = success_entry->value().get_object();
         BOOST_REQUIRE_MESSAGE(success.contains("content"),
                               owner.name << "." << mapping.method_name << " has success content");
         const auto& success_schema = success["content"]["application/json"]["schema"].get_object();
         BOOST_CHECK_MESSAGE(success_schema.size() != 0U,
                             owner.name << "." << mapping.method_name << " has a nonempty success schema");

         const auto error_entry = responses.find("default");
         BOOST_REQUIRE_MESSAGE(error_entry != responses.end(),
                               owner.name << "." << mapping.method_name << " has an error response");
         const auto& error = error_entry->value().get_object();
         BOOST_REQUIRE_MESSAGE(error.contains("content"),
                               owner.name << "." << mapping.method_name << " has error content");
         const auto& error_schema = error["content"]["application/json"]["schema"].get_object();
         BOOST_CHECK_MESSAGE(error_schema.size() != 0U,
                             owner.name << "." << mapping.method_name << " has a nonempty error schema");
         BOOST_TEST(error_schema["type"].as_string() == "object");
         const auto& error_properties = error_schema["properties"].get_object();
         for (const auto field :
              {"error", "message", "retryable", "status_code", "identity", "details_codec", "details"}) {
            BOOST_CHECK_MESSAGE(error_properties.contains(field),
                                owner.name << "." << mapping.method_name << " error envelope has " << field);
         }

         const auto& documented_errors = error_schema["x-forge-declared-errors"].get_array();
         BOOST_TEST(documented_errors.size() == method_descriptor->errors.size());
         for (const auto& declared : method_descriptor->errors) {
            const auto documented = std::ranges::find_if(documented_errors, [&](const forge::variant& candidate) {
               return candidate["name"].as_string() == declared.name;
            });
            BOOST_REQUIRE_MESSAGE(documented != documented_errors.end(),
                                  owner.name << "." << mapping.method_name << " documents " << declared.name);
            BOOST_TEST((*documented)["status_code"].as_uint64() == static_cast<std::uint64_t>(declared.status_code));
            BOOST_TEST((*documented)["retryable"].as_bool() == declared.retryable);
            BOOST_TEST((*documented)["identity"]["category"].as_string() == declared.identity.category);
            BOOST_TEST((*documented)["identity"]["code"].as_uint64() == declared.identity.code);
         }

         if (operation.contains("requestBody")) {
            const auto& request_schema = operation["requestBody"]["content"]["application/json"]["schema"].get_object();
            BOOST_CHECK_MESSAGE(request_schema.size() != 0U,
                                owner.name << "." << mapping.method_name << " has a nonempty request schema");
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(chain_http_transaction_wait_uses_request_deadline) {
   const auto routes = forge::api::http::traits<forge::chain::api::transaction>::routes();
   const auto found = std::ranges::find(routes, std::string_view{"await_transaction"}, &route::method_name);
   BOOST_REQUIRE(found != routes.end());
   BOOST_REQUIRE(found->timeout_field.has_value());
   BOOST_TEST(*found->timeout_field == "timeout_ms");

   const auto options = forge::api::http::detail::request_options_for(
       *found, forge::chain::protocol::transaction_await_request{.timeout_ms = 300'000U});
   BOOST_TEST(options.timeout == std::chrono::milliseconds{305'000});
   BOOST_TEST(options.retry_idempotent);
   BOOST_TEST(options.max_retries == 1U);
}

BOOST_AUTO_TEST_CASE(chain_http_retry_policy_matches_idempotent_verbs) {
   for (const auto verb : {method::get, method::head, method::put, method::delete_, method::options}) {
      const auto options = forge::api::http::detail::request_options_for(route{.verb = verb}, 0);
      BOOST_TEST(options.retry_idempotent);
      BOOST_TEST(options.max_retries == 1U);
   }

   for (const auto verb : {method::post, method::patch}) {
      const auto options = forge::api::http::detail::request_options_for(route{.verb = verb}, 0);
      BOOST_TEST(!options.retry_idempotent);
      BOOST_TEST(options.max_retries == 0U);
   }
}

BOOST_AUTO_TEST_CASE(chain_http_submission_uses_request_and_batch_deadlines) {
   const auto routes = forge::api::http::traits<forge::chain::api::submission>::routes();
   const auto submit = std::ranges::find(routes, std::string_view{"submit"}, &route::method_name);
   const auto submit_batch = std::ranges::find(routes, std::string_view{"submit_batch"}, &route::method_name);
   BOOST_REQUIRE(submit != routes.end());
   BOOST_REQUIRE(submit_batch != routes.end());
   BOOST_REQUIRE(submit->timeout_field.has_value());
   BOOST_REQUIRE(submit_batch->timeout_field.has_value());
   BOOST_TEST(*submit->timeout_field == "timeout_ms");
   BOOST_TEST(*submit_batch->timeout_field == "timeout_ms");

   const auto submit_options = forge::api::http::detail::request_options_for(
       *submit, forge::chain::protocol::transaction_submit_request{.timeout_ms = 12'000U});
   BOOST_TEST(submit_options.timeout == std::chrono::milliseconds{17'000});
   BOOST_TEST(!submit_options.retry_idempotent);
   BOOST_TEST(submit_options.max_retries == 0U);

   const auto batch_options = forge::api::http::detail::request_options_for(
       *submit_batch, forge::chain::protocol::transaction_submit_batch_request{
                          .transactions = {forge::chain::protocol::transaction_submit_request{.timeout_ms = 1'000U}},
                          .timeout_ms = 20'000U,
                      });
   BOOST_TEST(batch_options.timeout == std::chrono::milliseconds{25'000});
   BOOST_TEST(!batch_options.retry_idempotent);
   BOOST_TEST(batch_options.max_retries == 0U);
}

BOOST_AUTO_TEST_CASE(chain_transaction_remote_deadline_restores_the_declared_exception) {
   const auto descriptor = forge::chain::api::transaction::describe();
   const auto* method = forge::api::core::find_method(descriptor, "await_transaction");
   BOOST_REQUIRE(method != nullptr);
   const auto identity = forge::api::core::exception_identity<forge::chain::api::exceptions::deadline_exceeded>();
   const auto declared = std::ranges::find(method->errors, identity, &forge::api::core::error_descriptor::identity);
   BOOST_REQUIRE(declared != method->errors.end());
   BOOST_CHECK(declared->status_code == forge::api::core::status::deadline_exceeded);
   BOOST_TEST(declared->retryable);
   const auto submission_descriptor = forge::chain::api::submission::describe();
   const auto* submit = forge::api::core::find_method(submission_descriptor, "submit");
   const auto* submit_batch = forge::api::core::find_method(submission_descriptor, "submit_batch");
   BOOST_REQUIRE(submit != nullptr);
   BOOST_REQUIRE(submit_batch != nullptr);
   BOOST_CHECK(std::ranges::find(submit->errors, identity, &forge::api::core::error_descriptor::identity) !=
               submit->errors.end());
   BOOST_CHECK(std::ranges::find(submit_batch->errors, identity, &forge::api::core::error_descriptor::identity) !=
               submit_batch->errors.end());
   const auto mutation_identities =
       std::array{forge::api::core::exception_identity<forge::chain::api::exceptions::conflict>(),
                  forge::api::core::exception_identity<forge::chain::api::exceptions::admission_rejected>()};
   for (const auto& mutation_identity : mutation_identities) {
      BOOST_CHECK(std::ranges::find(submit->errors, mutation_identity, &forge::api::core::error_descriptor::identity) !=
                  submit->errors.end());
   }

   auto invoker = std::make_shared<deadline_remote_invoker>();
   auto remote = forge::api::core::proxy<forge::chain::api::transaction>{invoker};
   BOOST_CHECK_THROW(run(remote.await_transaction({.timeout_ms = 1U})),
                     forge::chain::api::exceptions::deadline_exceeded);
   BOOST_TEST(invoker->calls == 1U);
}

BOOST_AUTO_TEST_CASE(chain_audited_query_remote_restores_trust_required) {
   const auto descriptor = forge::chain::api::info::describe();
   const auto* method = forge::api::core::find_method(descriptor, "get");
   BOOST_REQUIRE(method != nullptr);
   const auto identity = forge::api::core::exception_identity<forge::chain::api::exceptions::trust_required>();
   const auto declared = std::ranges::find(method->errors, identity, &forge::api::core::error_descriptor::identity);
   BOOST_REQUIRE(declared != method->errors.end());
   BOOST_CHECK(declared->status_code == forge::api::core::status::failed_precondition);
   BOOST_TEST(!declared->retryable);

   auto invoker = std::make_shared<trust_required_remote_invoker>();
   auto remote = forge::api::core::proxy<forge::chain::api::info>{invoker};
   BOOST_CHECK_THROW(run(remote.get({.audit = forge::chain::protocol::audit_mode::required})),
                     forge::chain::api::exceptions::trust_required);
   BOOST_TEST(invoker->calls == 1U);
}

BOOST_AUTO_TEST_CASE(chain_admin_declares_mutation_errors_only_for_mutating_methods) {
   const auto descriptor = forge::chain::api::admin::describe();
   const auto identity = forge::api::core::exception_identity<forge::chain::api::exceptions::conflict>();
   const auto* push = forge::api::core::find_method(descriptor, "push_block");
   const auto* status = forge::api::core::find_method(descriptor, "producer_status");
   const auto* finalizer = forge::api::core::find_method(descriptor, "get_finalizer_status");
   const auto* operator_identity = forge::api::core::find_method(descriptor, "get_operator_identity");
   const auto* node_status = forge::api::core::find_method(descriptor, "get_node_status");
   const auto* snapshot_status = forge::api::core::find_method(descriptor, "snapshot_status");
   const auto* snapshot_request = forge::api::core::find_method(descriptor, "request_snapshot");
   const auto not_found = forge::api::core::exception_identity<forge::chain::api::exceptions::not_found>();
   const auto snapshot_lost = forge::api::core::exception_identity<forge::chain::api::exceptions::snapshot_lost>();
   BOOST_REQUIRE(push != nullptr);
   BOOST_REQUIRE(status != nullptr);
   BOOST_REQUIRE(finalizer != nullptr);
   BOOST_REQUIRE(operator_identity != nullptr);
   BOOST_REQUIRE(node_status != nullptr);
   BOOST_REQUIRE(snapshot_status != nullptr);
   BOOST_REQUIRE(snapshot_request != nullptr);
   BOOST_CHECK(std::ranges::find(push->errors, identity, &forge::api::core::error_descriptor::identity) !=
               push->errors.end());
   BOOST_CHECK(std::ranges::find(status->errors, identity, &forge::api::core::error_descriptor::identity) ==
               status->errors.end());
   BOOST_CHECK(std::ranges::find(finalizer->errors, identity, &forge::api::core::error_descriptor::identity) ==
               finalizer->errors.end());
   BOOST_CHECK(std::ranges::find(operator_identity->errors, identity, &forge::api::core::error_descriptor::identity) ==
               operator_identity->errors.end());
   BOOST_CHECK(std::ranges::find(node_status->errors, identity, &forge::api::core::error_descriptor::identity) ==
               node_status->errors.end());
   BOOST_CHECK(std::ranges::find(snapshot_status->errors, identity, &forge::api::core::error_descriptor::identity) ==
               snapshot_status->errors.end());
   BOOST_CHECK(std::ranges::find(snapshot_status->errors, not_found, &forge::api::core::error_descriptor::identity) !=
               snapshot_status->errors.end());
   BOOST_CHECK(std::ranges::find(snapshot_request->errors, snapshot_lost,
                                 &forge::api::core::error_descriptor::identity) != snapshot_request->errors.end());
   for (const auto& method : descriptor.methods) {
      if (method.name != "request_snapshot") {
         BOOST_CHECK(std::ranges::find(method.errors, snapshot_lost, &forge::api::core::error_descriptor::identity) ==
                     method.errors.end());
      }
   }
   BOOST_TEST(operator_identity->since_revision == 1U);
   BOOST_TEST(node_status->since_revision == 1U);
   BOOST_TEST(finalizer->since_revision == 4U);
}

BOOST_AUTO_TEST_CASE(chain_http_omits_an_unspecified_anchor) {
   const auto routes = forge::api::http::traits<forge::chain::api::block>::routes();
   const auto& route = find_route(routes, "get_consensus_parameters");
   const auto target = forge::api::http::detail::render_route_target(
       route, forge::chain::protocol::anchored_request{.anchor = std::nullopt,
                                                       .audit = forge::chain::protocol::audit_mode::required});

   BOOST_TEST(target == "/v1/chain/blocks/consensus-parameters?audit=required");
}

BOOST_AUTO_TEST_CASE(chain_http_carries_the_verified_finality_checkpoint) {
   const auto routes = forge::api::http::traits<forge::chain::api::info>::routes();
   const auto& route = find_route(routes, "get");
   auto checkpoint = forge::chain::protocol::block_id{};
   checkpoint._hash[0] = 0x42U;
   const auto target =
       forge::api::http::detail::render_route_target(route, forge::chain::protocol::anchored_request{
                                                                .finality_from = checkpoint,
                                                                .audit = forge::chain::protocol::audit_mode::required,
                                                            });

   BOOST_TEST(target.find("finality_from=") != std::string::npos);
   BOOST_TEST(target.find("audit=required") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(chain_table_scope_http_carries_the_opaque_cursor) {
   const auto routes = forge::api::http::traits<forge::chain::api::state>::routes();
   const auto& route = find_route(routes, "get_table_scope");
   const auto target = forge::api::http::detail::render_route_target(
       route, forge::chain::protocol::table_scope_request{
                  .code = forge::chain::protocol::account_name{"eosio.token"},
                  .table = forge::chain::protocol::name{"accounts"},
                  .cursor = forge::chain::protocol::bytes{0x00U, 0x2fU, 0xffU},
              });

   BOOST_TEST(route.target.find("&cursor={cursor}&") != std::string::npos);
   BOOST_TEST(target.find("cursor=%5B0%2C47%2C255%5D") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(chain_producers_http_carries_typed_lower_bound_and_opaque_cursor) {
   const auto routes = forge::api::http::traits<forge::chain::api::block>::routes();
   const auto& route = find_route(routes, "get_producers");
   const auto target = forge::api::http::detail::render_route_target(
       route, forge::chain::protocol::producers_request{
                  .lower_bound = forge::chain::protocol::account_name{"alice"},
                  .cursor = forge::chain::protocol::bytes{0x00U, 0x2fU, 0xffU},
              });

   BOOST_CHECK(route.verb == method::get);
   BOOST_TEST(route.target.find("json={json}") == std::string::npos);
   BOOST_TEST(route.target.find("&cursor={cursor}&") != std::string::npos);
   BOOST_TEST(target.find("lower_bound=alice") != std::string::npos);
   BOOST_TEST(target.find("cursor=%5B0%2C47%2C255%5D") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(chain_table_rows_http_carries_the_secondary_index) {
   const auto routes = forge::api::http::traits<forge::chain::api::state>::routes();
   const auto& route = find_route(routes, "get_table_rows");
   const auto index = forge::chain::protocol::table_index{
       .kind = forge::chain::protocol::table_index_kind::secondary_u128,
       .position = 2U,
   };
   const auto target = forge::api::http::detail::render_route_target(
       route, forge::chain::protocol::table_rows_request{
                  .code = forge::chain::protocol::account_name{"storlane"},
                  .scope = forge::chain::protocol::name{"storlane"},
                  .table = forge::chain::protocol::name{"revgeometry"},
                  .index = index,
              });

   BOOST_TEST(target.find("index=secondary-u128%3A2") != std::string::npos);
   BOOST_CHECK(forge::schema::parse_scalar_text<forge::chain::protocol::table_index>("secondary-u128:2") == index);
   BOOST_CHECK_THROW(
       static_cast<void>(forge::schema::parse_scalar_text<forge::chain::protocol::table_index>("secondary-u128")),
       forge::schema::exceptions::invalid_value);
}

BOOST_AUTO_TEST_CASE(chain_currency_stats_http_formats_the_symbol_code_path) {
   const auto routes = forge::api::http::traits<forge::chain::api::state>::routes();
   const auto& route = find_route(routes, "get_currency_stats");
   const auto target = forge::api::http::detail::render_route_target(
       route, forge::chain::protocol::currency_stats_request{
                  .code = forge::chain::protocol::account_name{"eosio.token"},
                  .symbol = forge::chain::protocol::symbol_code{"SYS"},
              });

   BOOST_TEST(target.starts_with("/v1/chain/state/currencies/eosio.token/stats/SYS?"));
   BOOST_CHECK(forge::schema::parse_scalar_text<forge::chain::protocol::symbol_code>("SYS") ==
               forge::chain::protocol::symbol_code{"SYS"});
}

BOOST_AUTO_TEST_CASE(chain_openapi_uses_canonical_public_key_json_shape) {
   const auto document = forge::api::http::openapi<forge::chain::api::transaction>();
   const auto& schema = document["paths"]["/v1/chain/transactions/required-keys"]["post"]["responses"]["200"]["content"]
                                ["application/json"]["schema"]["items"];

   BOOST_TEST(schema["type"].as_string() == "string");
   BOOST_TEST(schema["format"].as_string() == "forge-public-key");
}

BOOST_AUTO_TEST_CASE(chain_admin_operator_identity_preserves_non_k1_key_across_raw_variant_and_http_json) {
   auto r1 = forge::crypto::asymmetric::r1_public_key{};
   r1.data[0] = static_cast<char>(0x02);
   auto finalizer_bytes = forge::crypto::bls::public_key::data_type{};
   const auto finalizer_wire = forge::codec::hex::decode(
       "f363f7a0cd6ed0812feb8bbd8b8bd2cef835f900e5e056f69f9d0ca7c4a4ec5af54f3d0c272a732f7f6749de553c580"
       "50bd5aaae3a2945b066d4f7f44643f4d7c7e8d64dab5da258ed6b7377d44a944f0fa10e978439b83f266522ea5083f80e");
   BOOST_REQUIRE(finalizer_wire.size() == finalizer_bytes.size());
   std::ranges::copy(finalizer_wire, finalizer_bytes.begin());
   const auto identity = forge::chain::protocol::operator_identity{
       .producer = forge::chain::protocol::account_name{"operator"},
       .block_public_key = forge::chain::protocol::public_key{r1},
       .finalizer_public_key = forge::crypto::bls::public_key{finalizer_bytes},
       .enabled_roles = {forge::chain::protocol::operator_role::producer},
   };

   const auto raw = forge::raw::unpack<forge::chain::protocol::operator_identity>(forge::raw::pack(identity));
   BOOST_CHECK(raw == identity);
   BOOST_CHECK(std::holds_alternative<forge::crypto::asymmetric::r1_public_key>(raw.block_public_key));

   auto variant = forge::variant{};
   forge::to_variant(identity, variant);
   auto variant_round_trip = forge::chain::protocol::operator_identity{};
   forge::from_variant(variant, variant_round_trip);
   BOOST_CHECK(variant_round_trip == identity);
   BOOST_CHECK(std::holds_alternative<forge::crypto::asymmetric::r1_public_key>(variant_round_trip.block_public_key));

   const auto http_json = forge::codec::json::write(identity);
   BOOST_REQUIRE(http_json.ok());
   const auto http_round_trip = forge::codec::json::read<forge::chain::protocol::operator_identity>(
       http_json.text,
       {.source_name = "http.operator_identity", .unknown_fields = forge::codec::json::unknown_field_policy::error});
   BOOST_REQUIRE(http_round_trip.ok());
   BOOST_CHECK(http_round_trip.value == identity);
   BOOST_CHECK(
       std::holds_alternative<forge::crypto::asymmetric::r1_public_key>(http_round_trip.value.block_public_key));
}

BOOST_AUTO_TEST_CASE(http_response_decode_reports_safe_codec_location) {
   auto response = forge::net::http::response{forge::net::http::status::ok, 11};
   response.body() = R"({"enabled_roles":["not-an-operator-role"],"secret-bearing-field\n":true})";

   try {
      static_cast<void>(forge::api::http::detail::decode_response_body<forge::chain::protocol::operator_identity>(
          response, forge::api::http::body_codec::json));
      BOOST_FAIL("invalid typed HTTP response was accepted");
   } catch (const std::exception& error) {
      const auto chain = forge::exceptions::format_exception_chain(error);
      BOOST_TEST(chain.find("diagnostic_code=json.type") != std::string::npos);
      BOOST_TEST(chain.find("diagnostic_path_size=") != std::string::npos);
      BOOST_TEST(chain.find("secret-bearing-field") == std::string::npos);
      BOOST_TEST(chain.find(response.body()) == std::string::npos);
   }
}

BOOST_AUTO_TEST_CASE(chain_state_selector_openapi_requires_exactly_one_id_or_key) {
   const auto document = forge::api::http::openapi<forge::chain::api::state>();
   const auto check = [&](const char* path, std::initializer_list<std::string_view> other_fields) {
      const auto& operation = document["paths"][path]["get"];
      BOOST_TEST(!operation.get_object().contains("x-forge-query-schema"));
      const auto& parameters = operation["parameters"].get_array();
      const auto selector = std::ranges::find_if(parameters, [](const forge::variant& value) {
         return value["name"].as_string() == "selector" && value["in"].as_string() == "query";
      });
      BOOST_REQUIRE(selector != parameters.end());
      BOOST_TEST((*selector)["required"].as_bool());
      BOOST_TEST((*selector)["style"].as_string() == "form");
      BOOST_TEST((*selector)["explode"].as_bool());
      const auto& schema = (*selector)["schema"];
      const auto& one_of = schema["oneOf"].get_array();
      BOOST_REQUIRE(one_of.size() == 2U);
      BOOST_TEST(one_of[std::size_t{0}]["required"][std::size_t{0}].as_string() == "id");
      BOOST_TEST(one_of[std::size_t{0}]["not"]["required"][std::size_t{0}].as_string() == "key");
      BOOST_TEST(one_of[std::size_t{1}]["required"][std::size_t{0}].as_string() == "key");
      BOOST_TEST(one_of[std::size_t{1}]["not"]["required"][std::size_t{0}].as_string() == "id");
      BOOST_TEST(schema["additionalProperties"].as_bool() == false);
      BOOST_TEST(schema["properties"].get_object().size() == 2U);
      for (const auto field : {"id", "key"}) {
         BOOST_TEST(std::ranges::none_of(parameters, [&](const forge::variant& value) {
            return value["name"].as_string() == field && value["in"].as_string() == "query";
         }));
      }
      for (const auto field : other_fields) {
         BOOST_TEST(std::ranges::count_if(parameters, [&](const forge::variant& value) {
                       return value["name"].as_string() == field && value["in"].as_string() == "query";
                    }) == 1U);
      }
   };

   check("/v1/chain/state/accounts", {"anchor", "finality_from", "audit"});
   check("/v1/chain/state/codes",
         {"include_wasm", "include_abi", "known_abi_hash", "anchor", "finality_from", "audit"});
   check("/v1/chain/state/permission-links",
         {"code", "message_type", "limit", "cursor", "anchor", "finality_from", "audit"});
}

BOOST_AUTO_TEST_CASE(chain_block_info_admin_openapi_exposes_canonical_typed_records) {
   const auto block = forge::api::http::openapi<forge::chain::api::block>();
   const auto& producers = block["paths"]["/v1/chain/blocks/producers"]["get"];
   const auto& parameters = producers["parameters"].get_array();
   BOOST_TEST(std::ranges::none_of(parameters,
                                   [](const forge::variant& value) { return value["name"].as_string() == "json"; }));
   for (const auto name : {"lower_bound", "limit", "cursor", "anchor", "finality_from", "audit"}) {
      BOOST_TEST(std::ranges::count_if(
                     parameters, [&](const forge::variant& value) { return value["name"].as_string() == name; }) == 1U);
   }
   const auto cursor = std::ranges::find_if(
       parameters, [](const forge::variant& value) { return value["name"].as_string() == "cursor"; });
   BOOST_REQUIRE(cursor != parameters.end());
   BOOST_TEST(!cursor->get_object().contains("schema"));
   BOOST_TEST((*cursor)["content"]["application/json"]["schema"]["anyOf"][std::size_t{0}]["type"].as_string() ==
              "array");

   const auto& producer_properties =
       producers["responses"]["200"]["content"]["application/json"]["schema"]["properties"];
   BOOST_TEST(producer_properties["rows"]["items"]["properties"]["total_votes"]["type"].as_string() == "number");
   BOOST_TEST(producer_properties["total_vote_weight"]["type"].as_string() == "number");
   BOOST_TEST(producer_properties["total_vote_weight"]["format"].as_string() == "double");
   BOOST_TEST(producer_properties["next"]["anyOf"][std::size_t{0}]["type"].as_string() == "array");

   const auto& feature_properties =
       block["paths"]["/v1/chain/blocks/activated-protocol-features"]["get"]["responses"]["200"]["content"]
            ["application/json"]["schema"]["properties"]["features"]["items"]["properties"];
   auto feature_fields = std::vector<std::string>{};
   feature_fields.reserve(feature_properties.get_object().size());
   for (const auto& entry : feature_properties.get_object()) {
      feature_fields.emplace_back(entry.key());
   }
   const auto expected_feature_fields =
       std::vector<std::string>{"feature_digest", "activation_ordinal",    "activation_block_num", "description_digest",
                                "dependencies",   "protocol_feature_type", "specification"};
   BOOST_TEST(feature_fields == expected_feature_fields, boost::test_tools::per_element());
   BOOST_TEST(!feature_properties.get_object().contains("subjective_restrictions"));

   const auto& consensus_properties = block["paths"]["/v1/chain/blocks/consensus-parameters"]["get"]["responses"]["200"]
                                           ["content"]["application/json"]["schema"]["properties"];
   BOOST_TEST(consensus_properties["parameters"]["properties"].get_object().contains("max_action_return_value_size"));
   BOOST_TEST(consensus_properties["wasm"]["anyOf"][std::size_t{0}]["properties"].get_object().contains("max_pages"));

   const auto& vote_properties = block["paths"]["/v1/chain/blocks/finalizers"]["get"]["responses"]["200"]["content"]
                                      ["application/json"]["schema"]["properties"]["last_votes"]["items"]["properties"];
   BOOST_TEST(vote_properties["public_key"]["type"].as_string() == "string");
   BOOST_TEST(vote_properties["public_key"]["format"].as_string() == "forge-bls-public-key");

   const auto information = forge::api::http::openapi<forge::chain::api::info>();
   const auto& info_properties = information["paths"]["/v1/chain/info"]["get"]["responses"]["200"]["content"]
                                            ["application/json"]["schema"]["properties"];
   BOOST_TEST(info_properties.get_object().contains("resource_config"));
   BOOST_TEST(info_properties.get_object().contains("resource_state"));
   BOOST_TEST(!info_properties.get_object().contains("virtual_block_cpu_limit"));
   BOOST_TEST(!info_properties.get_object().contains("total_cpu_weight"));

   const auto admin = forge::api::http::openapi<forge::chain::api::admin>();
   const auto& operator_identity = admin["paths"]["/v1/chain/admin/operator-identity"]["get"]["responses"]["200"]
                                        ["content"]["application/json"]["schema"]["properties"];
   BOOST_TEST(operator_identity.get_object().contains("producer"));
   BOOST_TEST(operator_identity.get_object().contains("block_public_key"));
   BOOST_TEST(operator_identity.get_object().contains("finalizer_public_key"));
   BOOST_TEST(operator_identity.get_object().contains("enabled_roles"));
   const auto& node_status = admin["paths"]["/v1/chain/admin/node-status"]["get"]["responses"]["200"]["content"]
                                  ["application/json"]["schema"]["properties"];
   BOOST_TEST(node_status.get_object().contains("process_start"));
   BOOST_TEST(node_status.get_object().contains("uptime_ms"));
   BOOST_TEST(node_status.get_object().contains("lifecycle"));
   const auto& supported_feature_properties =
       admin["paths"]["/v1/chain/admin/protocol-features/supported"]["get"]["responses"]["200"]["content"]
            ["application/json"]["schema"]["properties"]["features"]["items"]["properties"];
   auto supported_feature_fields = std::vector<std::string>{};
   supported_feature_fields.reserve(supported_feature_properties.get_object().size());
   for (const auto& entry : supported_feature_properties.get_object()) {
      supported_feature_fields.emplace_back(entry.key());
   }
   const auto expected_supported_feature_fields =
       std::vector<std::string>{"feature_digest", "subjective_restrictions", "description_digest",
                                "dependencies",   "protocol_feature_type",   "specification"};
   BOOST_TEST(supported_feature_fields == expected_supported_feature_fields, boost::test_tools::per_element());
   BOOST_TEST(!supported_feature_properties.get_object().contains("activation_ordinal"));
   BOOST_TEST(!supported_feature_properties.get_object().contains("activation_block_num"));
   const auto& correction_properties =
       admin["paths"]["/v1/chain/admin/accounts/ram-corrections"]["get"]["responses"]["200"]["content"]
            ["application/json"]["schema"]["properties"]["rows"]["items"]["properties"];
   BOOST_TEST(correction_properties.get_object().contains("id"));
   BOOST_TEST(correction_properties.get_object().contains("name"));
   BOOST_TEST(correction_properties.get_object().contains("ram_correction"));
}

BOOST_AUTO_TEST_CASE(chain_authorizer_pagination_uses_opaque_bytes) {
   const auto limits = forge::chain::protocol::service_limits{};
   auto request = forge::chain::protocol::authorizers_request{
       .accounts = {forge::chain::protocol::permission_level{}},
       .limit = 1U,
       .cursor = forge::chain::protocol::bytes{0x01U},
   };
   BOOST_CHECK_NO_THROW(forge::chain::api::require_request_within_limits(request, limits));
   request.cursor = forge::chain::protocol::bytes{};
   BOOST_CHECK_THROW(forge::chain::api::require_request_within_limits(request, limits),
                     forge::chain::api::exceptions::invalid_request);

   const auto document = forge::api::http::openapi<forge::chain::api::state>();
   const auto& operation = document["paths"]["/v1/chain/state/accounts-by-authorizers"]["post"];
   const auto& request_properties = operation["requestBody"]["content"]["application/json"]["schema"]["properties"];
   BOOST_TEST(request_properties["cursor"]["anyOf"][std::size_t{0}]["type"].as_string() == "array");
   const auto& response_properties =
       operation["responses"]["200"]["content"]["application/json"]["schema"]["properties"];
   BOOST_TEST(response_properties["next"]["anyOf"][std::size_t{0}]["type"].as_string() == "array");
}

BOOST_AUTO_TEST_CASE(chain_state_paginated_responses_require_nonempty_next) {
   const auto limits = forge::chain::protocol::service_limits{};
   const auto descriptor = forge::chain::api::limited_descriptor<forge::chain::api::state>(limits);

   const auto check = [&]<typename Request, typename Response>(std::string_view method_name, const Request& request,
                                                               Response response) {
      const auto* method = forge::api::core::find_method(descriptor, method_name);
      BOOST_REQUIRE(method != nullptr);
      const auto request_bytes = forge::raw::pack(request);

      response.next = forge::chain::protocol::bytes{0x00U, 0xffU};
      BOOST_CHECK_NO_THROW(forge::chain::api::require_response_within_limits(response, request, limits));
      BOOST_CHECK_NO_THROW(method->response_validator(request_bytes, forge::raw::pack(response)));

      response.next = forge::chain::protocol::bytes{};
      BOOST_CHECK_THROW(forge::chain::api::require_response_within_limits(response, request, limits),
                        forge::chain::api::exceptions::unavailable);
      BOOST_CHECK_THROW(method->response_validator(request_bytes, forge::raw::pack(response)),
                        forge::chain::api::exceptions::unavailable);
   };

   auto permission_links = forge::chain::protocol::permission_links_request{.limit = 1U};
   permission_links.key = forge::chain::protocol::account_name{"alice"};
   check("get_permission_links", permission_links, forge::chain::protocol::permission_links_response{});
   check("get_scheduled_transactions", forge::chain::protocol::scheduled_request{.limit = 1U},
         forge::chain::protocol::scheduled_response{});
   check("get_accounts_by_authorizers", forge::chain::protocol::authorizers_request{.limit = 1U},
         forge::chain::protocol::authorizers_response{});
   check("get_table_changes",
         forge::chain::protocol::table_changes_request{
             .from_block = 1U,
             .to_block = 2U,
             .tables = {{.code = forge::chain::protocol::account_name{"alice"}}},
             .limit = 1U,
         },
         forge::chain::protocol::table_changes_response{});
   check("get_account_changes",
         forge::chain::protocol::account_changes_request{
             .from_block = 1U,
             .to_block = 2U,
             .accounts = {forge::chain::protocol::account_name{"alice"}},
             .limit = 1U,
         },
         forge::chain::protocol::account_changes_response{});
}

BOOST_AUTO_TEST_CASE(chain_openapi_omits_body_for_query_only_admin_action) {
   const auto document = forge::api::http::openapi<forge::chain::api::admin>();
   const auto& operation = document["paths"]["/v1/chain/admin/snapshots"]["post"];

   BOOST_TEST(!operation.get_object().contains("requestBody"));
   const auto& parameters = operation["parameters"].get_array();
   const auto name = std::ranges::find_if(
       parameters, [](const forge::variant& value) { return value["name"].as_string() == "name"; });
   BOOST_REQUIRE(name != parameters.end());
   BOOST_TEST((*name)["in"].as_string() == "query");
}

BOOST_AUTO_TEST_CASE(chain_table_scope_openapi_exposes_json_bytes_cursor_and_next) {
   const auto document = forge::api::http::openapi<forge::chain::api::state>();
   const auto& operation = document["paths"]["/v1/chain/state/tables/{code}/scopes"]["get"];
   const auto& parameters = operation["parameters"].get_array();
   const auto cursor = std::ranges::find_if(
       parameters, [](const forge::variant& value) { return value["name"].as_string() == "cursor"; });
   BOOST_REQUIRE(cursor != parameters.end());
   BOOST_TEST((*cursor)["required"].as_bool() == false);
   BOOST_TEST(!cursor->get_object().contains("schema"));
   const auto& cursor_schema = (*cursor)["content"]["application/json"]["schema"];
   BOOST_TEST(cursor_schema["anyOf"][std::size_t{0}]["type"].as_string() == "array");
   BOOST_TEST(cursor_schema["anyOf"][std::size_t{0}]["items"]["type"].as_string() == "integer");

   const auto& properties =
       operation["responses"]["200"]["content"]["application/json"]["schema"]["properties"].get_object();
   BOOST_TEST(properties.contains("next"));
   BOOST_TEST(!properties.contains("more"));
   BOOST_TEST(!properties.contains("next_key"));
   BOOST_TEST(properties["next"]["anyOf"][std::size_t{0}]["type"].as_string() == "array");
}

BOOST_AUTO_TEST_CASE(chain_typed_changes_openapi_uses_post_bodies_and_opaque_bytes_cursors) {
   const auto document = forge::api::http::openapi<forge::chain::api::state>();
   for (const auto path : {"/v1/chain/state/table-changes", "/v1/chain/state/account-changes"}) {
      const auto& operation = document["paths"][path]["post"];
      const auto& request = operation["requestBody"]["content"]["application/json"]["schema"]["properties"];
      BOOST_TEST(request["cursor"]["anyOf"][std::size_t{0}]["type"].as_string() == "array");
      BOOST_TEST(request["from_block"]["type"].as_string() == "integer");
      BOOST_TEST(request["to_block"]["type"].as_string() == "integer");
      const auto& response = operation["responses"]["200"]["content"]["application/json"]["schema"]["properties"];
      BOOST_TEST(response["next"]["anyOf"][std::size_t{0}]["type"].as_string() == "array");
      BOOST_TEST(response["blocks"]["type"].as_string() == "array");
      BOOST_TEST(!response.get_object().contains("changes"));
      const auto& batch = response["blocks"]["items"]["properties"];
      BOOST_TEST(batch.get_object().contains("anchor"));
      BOOST_TEST(batch["mutations"]["type"].as_string() == "array");
      const auto& mutation = batch["mutations"]["items"]["properties"].get_object();
      BOOST_TEST(mutation.contains(path == std::string_view{"/v1/chain/state/table-changes"} ? "table" : "account"));
   }
}

BOOST_AUTO_TEST_CASE(verified_block_response_is_bound_to_the_requested_identity) {
   auto response = forge::chain::protocol::block_response{};
   response.id = response.block.calculate_id();
   response.num = response.block.calculate_block_num();
   response.canonical = true;
   response.context = forge::chain::protocol::response_context{
       .chain = {},
       .head = response.id,
       .finalized = response.id,
       .anchor =
           forge::chain::protocol::state_anchor{
               .chain = {},
               .block = response.id,
               .block_num = response.num,
           },
   };
   response.audit = forge::chain::protocol::audit_bundle{
       .finality =
           forge::chain::protocol::proof_blob{
               .scheme = "test.finality",
               .version = 1,
           },
   };

   auto services = forge::api::core::registry{};
   services.install<forge::chain::api::block>(std::make_shared<block_service>(response));
   auto client = forge::chain::api::verified_client{
       forge::chain::api::raw_client{forge::chain::api::service_handles{
           .blocks = services.get<forge::chain::api::block>(forge::chain::api::block::ref()),
       }},
       std::make_shared<accepting_audit_verifier>(),
   };

   auto other = response.id;
   ++other._hash[1];
   BOOST_CHECK_THROW(run(client.get_block({.id = other})), forge::chain::api::exceptions::invalid_finality);
   BOOST_CHECK_THROW(run(client.get_block({.num = response.num + 1U})),
                     forge::chain::api::exceptions::invalid_finality);
   const auto verified = run(client.get_block({.id = response.id, .num = response.num}));
   BOOST_TEST(verified.id == response.id);
}

BOOST_AUTO_TEST_CASE(verified_block_rejects_transaction_receipts_not_committed_by_its_header) {
   auto response = forge::chain::protocol::block_response{};
   auto receipt = forge::chain::protocol::transaction_receipt{};
   receipt.status = forge::chain::protocol::transaction_receipt::status::executed;
   receipt.cpu_usage_us = 7U;
   auto receipt_id = forge::chain::protocol::transaction_id{};
   receipt_id._hash[0] = 17U;
   receipt.trx = receipt_id;
   response.block.transactions.push_back(receipt);
   response.block.transaction_mroot = forge::chain::protocol::calculate_transaction_mroot(response.block.transactions);
   response.id = response.block.calculate_id();
   response.num = response.block.calculate_block_num();
   response.canonical = true;
   response.context = forge::chain::protocol::response_context{
       .head = response.id,
       .finalized = response.id,
       .anchor =
           forge::chain::protocol::state_anchor{
               .block = response.id,
               .block_num = response.num,
               .transaction_root = response.block.transaction_mroot,
           },
   };
   response.audit = forge::chain::protocol::audit_bundle{
       .finality = forge::chain::protocol::proof_blob{.scheme = "test.finality"},
   };

   auto mutated = response;
   ++mutated.block.transactions.front().cpu_usage_us;
   auto services = forge::api::core::registry{};
   services.install<forge::chain::api::block>(std::make_shared<block_service>(std::move(mutated)));
   auto client = forge::chain::api::verified_client{
       forge::chain::api::raw_client{forge::chain::api::service_handles{
           .blocks = services.get<forge::chain::api::block>(forge::chain::api::block::ref()),
       }},
       std::make_shared<accepting_audit_verifier>(),
   };

   BOOST_CHECK_THROW(run(client.get_block({.id = response.id, .num = response.num})),
                     forge::chain::api::exceptions::invalid_finality);
}

BOOST_AUTO_TEST_CASE(verified_header_is_bound_to_its_request_and_finalized_anchor) {
   auto response = forge::chain::protocol::block_header_response{};
   response.header.transaction_mroot._hash[0] = 13U;
   response.id = response.header.calculate_id();
   response.num = response.header.calculate_block_num();
   response.canonical = true;
   response.context.anchor = forge::chain::protocol::state_anchor{
       .block = response.id,
       .block_num = response.num,
       .transaction_root = response.header.transaction_mroot,
   };
   response.audit = forge::chain::protocol::audit_bundle{
       .finality = forge::chain::protocol::proof_blob{.scheme = "test.finality"},
   };

   const auto verify = [&](forge::chain::protocol::block_header_response candidate) {
      auto services = forge::api::core::registry{};
      services.install<forge::chain::api::block>(std::make_shared<block_service>(std::move(candidate)));
      auto client = forge::chain::api::verified_client{
          forge::chain::api::raw_client{forge::chain::api::service_handles{
              .blocks = services.get<forge::chain::api::block>(forge::chain::api::block::ref()),
          }},
          std::make_shared<accepting_audit_verifier>(),
      };
      return run(client.get_header({.id = response.id, .num = response.num}));
   };

   BOOST_TEST(verify(response).id == response.id);

   auto mutated = response;
   ++mutated.header.transaction_mroot._hash[0];
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(mutated))), forge::chain::api::exceptions::invalid_finality);

   auto non_canonical = response;
   non_canonical.canonical = false;
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(non_canonical))),
                     forge::chain::api::exceptions::invalid_finality);
}

BOOST_AUTO_TEST_CASE(verified_typed_state_query_delegates_authenticated_projection) {
   auto anchor = forge::chain::protocol::state_anchor{};
   anchor.block._hash[0] = 21U;
   anchor.block_num = 21U;
   auto response = forge::chain::protocol::account_response{};
   response.account.name = forge::chain::protocol::account_name{"alice"};
   response.context.anchor = anchor;
   response.audit = forge::chain::protocol::audit_bundle{
       .finality = forge::chain::protocol::proof_blob{.scheme = "test.finality"},
       .state = {forge::chain::protocol::proof_blob{.scheme = "test.state"}},
   };

   auto services = forge::api::core::registry{};
   services.install<forge::chain::api::state>(std::make_shared<state_service>(std::move(response)));
   auto verifier = std::make_shared<accepting_audit_verifier>();
   verifier->point_value = forge::chain::protocol::bytes{1U, 2U};
   auto projections = std::make_shared<account_projection_verifier>();
   auto client = forge::chain::api::verified_client{
       forge::chain::api::raw_client{forge::chain::api::service_handles{
           .state_queries = services.get<forge::chain::api::state>(forge::chain::api::state::ref()),
       }},
       verifier,
       projections,
   };

   const auto result =
       run(client.get_account(account_by_name(forge::chain::protocol::account_name{"alice"}, anchor.block)));
   BOOST_TEST(result.account.name.value == forge::chain::protocol::account_name{"alice"}.value);
   BOOST_TEST(verifier->state_point_verifications == 1U);
   BOOST_TEST(projections->verifications == 1U);
}

BOOST_AUTO_TEST_CASE(verified_client_uses_preferred_finality_anchor_without_overwriting_an_explicit_anchor) {
   auto anchor = forge::chain::protocol::state_anchor{};
   anchor.block._hash[0] = 21U;
   anchor.block_num = 21U;
   auto response = forge::chain::protocol::account_response{};
   response.account.name = forge::chain::protocol::account_name{"alice"};
   response.context.anchor = anchor;
   response.audit = forge::chain::protocol::audit_bundle{
       .finality = forge::chain::protocol::proof_blob{.scheme = "test.finality"},
       .state = {forge::chain::protocol::proof_blob{.scheme = "test.state"}},
   };

   auto services = forge::api::core::registry{};
   auto service = std::make_shared<state_service>(response);
   services.install<forge::chain::api::state>(service);
   auto verifier = std::make_shared<accepting_audit_verifier>();
   verifier->point_value = forge::chain::protocol::bytes{1U, 2U};
   auto preferred = forge::chain::protocol::block_id{};
   preferred._hash[0] = 8U;
   verifier->preferred_anchor = preferred;
   auto client = forge::chain::api::verified_client{
       forge::chain::api::raw_client{forge::chain::api::service_handles{
           .state_queries = services.get<forge::chain::api::state>(forge::chain::api::state::ref()),
       }},
       verifier,
       std::make_shared<account_projection_verifier>(),
   };

   static_cast<void>(
       run(client.get_account(account_by_name(forge::chain::protocol::account_name{"alice"}, anchor.block))));
   BOOST_REQUIRE(service->last_account_request.has_value());
   BOOST_REQUIRE(service->last_account_request->finality_from.has_value());
   BOOST_TEST(*service->last_account_request->finality_from == preferred);

   auto explicit_anchor = forge::chain::protocol::block_id{};
   explicit_anchor._hash[0] = 13U;
   auto explicit_request = account_by_name(forge::chain::protocol::account_name{"alice"}, anchor.block);
   explicit_request.finality_from = explicit_anchor;
   static_cast<void>(run(client.get_account(explicit_request)));
   BOOST_REQUIRE(service->last_account_request.has_value());
   BOOST_REQUIRE(service->last_account_request->finality_from.has_value());
   BOOST_TEST(*service->last_account_request->finality_from == explicit_anchor);
}

BOOST_AUTO_TEST_CASE(verified_client_translates_extension_failures_to_typed_errors) {
   auto anchor = forge::chain::protocol::state_anchor{};
   anchor.block._hash[0] = 21U;
   anchor.block_num = 21U;
   auto response = forge::chain::protocol::account_response{};
   response.account.name = forge::chain::protocol::account_name{"alice"};
   response.context.anchor = anchor;
   response.audit = forge::chain::protocol::audit_bundle{
       .finality = forge::chain::protocol::proof_blob{.scheme = "test.finality"},
       .state = {forge::chain::protocol::proof_blob{.scheme = "test.state"}},
   };

   const auto make_client = [&](const std::shared_ptr<accepting_audit_verifier>& verifier) {
      verifier->point_value = forge::chain::protocol::bytes{1U, 2U};
      auto services = std::make_shared<forge::api::core::registry>();
      services->install<forge::chain::api::state>(std::make_shared<state_service>(response));
      return std::pair{
          forge::chain::api::verified_client{
              forge::chain::api::raw_client{forge::chain::api::service_handles{
                  .state_queries = services->get<forge::chain::api::state>(forge::chain::api::state::ref()),
              }},
              verifier,
              std::make_shared<account_projection_verifier>(),
          },
          std::move(services),
      };
   };

   auto context_verifier = std::make_shared<accepting_audit_verifier>();
   context_verifier->throw_standard_context = true;
   auto context_client = make_client(context_verifier);
   BOOST_CHECK_THROW(static_cast<void>(run(context_client.first.get_account(
                         account_by_name(forge::chain::protocol::account_name{"alice"}, anchor.block)))),
                     forge::chain::api::exceptions::invalid_state_proof);

   auto point_verifier = std::make_shared<accepting_audit_verifier>();
   point_verifier->throw_nonstandard_state_point = true;
   auto point_client = make_client(point_verifier);
   BOOST_CHECK_THROW(static_cast<void>(run(point_client.first.get_account(
                         account_by_name(forge::chain::protocol::account_name{"alice"}, anchor.block)))),
                     forge::chain::api::exceptions::invalid_state_proof);

   auto anchor_verifier = std::make_shared<accepting_audit_verifier>();
   anchor_verifier->throw_standard_preferred_anchor = true;
   auto anchor_client = make_client(anchor_verifier);
   BOOST_CHECK_THROW(static_cast<void>(run(anchor_client.first.get_account(
                         account_by_name(forge::chain::protocol::account_name{"alice"}, anchor.block)))),
                     forge::chain::api::exceptions::anchor_unavailable);
}

BOOST_AUTO_TEST_CASE(verified_client_translates_service_failures_and_cancellation) {
   auto services = forge::api::core::registry{};
   auto service = std::make_shared<state_service>(forge::chain::protocol::account_response{});
   services.install<forge::chain::api::state>(service);
   auto client = forge::chain::api::verified_client{
       forge::chain::api::raw_client{forge::chain::api::service_handles{
           .state_queries = services.get<forge::chain::api::state>(forge::chain::api::state::ref()),
       }},
       std::make_shared<accepting_audit_verifier>(),
       std::make_shared<account_projection_verifier>(),
   };

   const auto request = account_by_name(forge::chain::protocol::account_name{"alice"});
   service->account_failure = state_service::failure::standard;
   BOOST_CHECK_THROW(static_cast<void>(run(client.get_account(request))), forge::chain::api::exceptions::unavailable);
   service->account_failure = state_service::failure::nonstandard;
   BOOST_CHECK_THROW(static_cast<void>(run(client.get_account(request))), forge::chain::api::exceptions::unavailable);
   service->account_failure = state_service::failure::canceled;
   BOOST_CHECK_THROW(static_cast<void>(run(client.get_account(request))), forge::asio::exceptions::canceled);
   service->account_failure = state_service::failure::timed_out;
   BOOST_CHECK_THROW(static_cast<void>(run(client.get_account(request))),
                     forge::chain::api::exceptions::deadline_exceeded);
   service->account_failure = state_service::failure::api_canceled;
   BOOST_CHECK_THROW(static_cast<void>(run(client.get_account(request))), forge::asio::exceptions::canceled);
   service->account_failure = state_service::failure::api_deadline;
   BOOST_CHECK_THROW(static_cast<void>(run(client.get_account(request))),
                     forge::chain::api::exceptions::deadline_exceeded);
   service->account_failure = state_service::failure::foreign_forge;
   BOOST_CHECK_THROW(static_cast<void>(run(client.get_account(request))), forge::chain::api::exceptions::unavailable);
   service->account_failure = state_service::failure::chain_api;
   BOOST_CHECK_THROW(static_cast<void>(run(client.get_account(request))),
                     forge::chain::api::exceptions::invalid_request);
}

BOOST_AUTO_TEST_CASE(verified_info_rejects_payload_identity_inconsistent_with_audited_context) {
   auto chain = forge::chain::protocol::chain_id{};
   chain._hash[0] = 1U;
   const auto finalized_header = forge::chain::protocol::signed_block_header{};
   const auto finalized = finalized_header.calculate_id();
   auto head_header = forge::chain::protocol::signed_block_header{};
   head_header.previous = finalized;
   const auto head = head_header.calculate_id();

   auto response = forge::chain::protocol::info_response{};
   response.chain = chain;
   response.head = head;
   response.head_num = head_header.calculate_block_num();
   response.finalized = finalized;
   response.finalized_num = finalized_header.calculate_block_num();
   response.context = forge::chain::protocol::response_context{
       .chain = chain,
       .head = head,
       .finalized = finalized,
       .anchor =
           forge::chain::protocol::state_anchor{
               .chain = chain,
               .block = finalized,
               .block_num = response.finalized_num,
           },
   };
   response.audit = forge::chain::protocol::audit_bundle{
       .finality = forge::chain::protocol::proof_blob{.scheme = "test.finality"},
   };

   const auto verify = [](forge::chain::protocol::info_response candidate,
                          forge::chain::protocol::anchored_request request = {}) {
      auto services = forge::api::core::registry{};
      services.install<forge::chain::api::info>(std::make_shared<info_service>(std::move(candidate)));
      auto client = forge::chain::api::verified_client{
          forge::chain::api::raw_client{forge::chain::api::service_handles{
              .information = services.get<forge::chain::api::info>(forge::chain::api::info::ref()),
          }},
          std::make_shared<accepting_audit_verifier>(),
      };
      return run(client.get_info(std::move(request)));
   };

   BOOST_TEST(verify(response).chain == chain);

   auto wrong_anchor = finalized;
   ++wrong_anchor._hash[0];
   BOOST_CHECK_THROW(static_cast<void>(verify(response, {.anchor = wrong_anchor})),
                     forge::chain::api::exceptions::invalid_finality);

   auto wrong_chain = response;
   ++wrong_chain.chain._hash[0];
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(wrong_chain))), forge::chain::api::exceptions::wrong_chain);

   auto wrong_head = response;
   ++wrong_head.head._hash[0];
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(wrong_head))), forge::chain::api::exceptions::invalid_finality);

   auto wrong_head_num = response;
   ++wrong_head_num.head_num;
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(wrong_head_num))),
                     forge::chain::api::exceptions::invalid_finality);

   auto wrong_finalized = response;
   ++wrong_finalized.finalized._hash[0];
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(wrong_finalized))),
                     forge::chain::api::exceptions::invalid_finality);

   auto wrong_finalized_num = response;
   ++wrong_finalized_num.finalized_num;
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(wrong_finalized_num))),
                     forge::chain::api::exceptions::invalid_finality);
}

BOOST_AUTO_TEST_CASE(verified_await_transaction_enforces_requested_finality) {
   auto id = forge::chain::protocol::transaction_id{};
   id._hash[0] = 29U;
   auto anchor = forge::chain::protocol::state_anchor{};
   anchor.block._hash[0] = 42U;
   anchor.block_num = 42U;

   auto response = forge::chain::protocol::transaction_status_response{};
   response.id = id;
   response.state = forge::chain::protocol::transaction_lifecycle::included;
   response.context.anchor = anchor;
   response.audit = forge::chain::protocol::audit_bundle{
       .finality = forge::chain::protocol::proof_blob{.scheme = "test.finality"},
       .transaction = forge::chain::protocol::transaction_inclusion_proof{},
   };

   const auto await = [&](forge::chain::protocol::transaction_status_response candidate,
                          forge::chain::protocol::transaction_lifecycle desired) {
      auto services = forge::api::core::registry{};
      services.install<forge::chain::api::transaction>(std::make_shared<transaction_service>(std::move(candidate)));
      auto client = forge::chain::api::verified_client{
          forge::chain::api::raw_client{forge::chain::api::service_handles{
              .transactions = services.get<forge::chain::api::transaction>(forge::chain::api::transaction::ref()),
          }},
          std::make_shared<accepting_audit_verifier>(),
      };
      return run(client.await_transaction({.id = id, .desired = desired, .finality_from = anchor.block}));
   };

   BOOST_CHECK_THROW(static_cast<void>(await(response, forge::chain::protocol::transaction_lifecycle::finalized)),
                     forge::chain::api::exceptions::invalid_transaction_proof);

   auto wrong_head = response;
   ++wrong_head.head._hash[0];
   BOOST_CHECK_THROW(
       static_cast<void>(await(std::move(wrong_head), forge::chain::protocol::transaction_lifecycle::included)),
       forge::chain::api::exceptions::invalid_finality);

   response.state = forge::chain::protocol::transaction_lifecycle::finalized;
   BOOST_TEST(static_cast<unsigned>(await(response, forge::chain::protocol::transaction_lifecycle::finalized).state) ==
              static_cast<unsigned>(forge::chain::protocol::transaction_lifecycle::finalized));
   BOOST_TEST(static_cast<unsigned>(await(response, forge::chain::protocol::transaction_lifecycle::included).state) ==
              static_cast<unsigned>(forge::chain::protocol::transaction_lifecycle::finalized));
}

BOOST_AUTO_TEST_CASE(verified_transaction_status_delegates_the_inclusion_proof) {
   auto id = forge::chain::protocol::transaction_id{};
   id._hash[0] = 31U;
   auto response = forge::chain::protocol::transaction_status_response{};
   response.id = id;
   response.state = forge::chain::protocol::transaction_lifecycle::finalized;
   response.context.anchor = forge::chain::protocol::state_anchor{.block_num = 7U};
   response.audit = forge::chain::protocol::audit_bundle{
       .finality = forge::chain::protocol::proof_blob{.scheme = "test.finality"},
       .transaction = forge::chain::protocol::transaction_inclusion_proof{},
   };

   auto services = forge::api::core::registry{};
   services.install<forge::chain::api::transaction>(std::make_shared<transaction_service>(std::move(response)));
   auto verifier = std::make_shared<accepting_audit_verifier>();
   auto client = forge::chain::api::verified_client{
       forge::chain::api::raw_client{forge::chain::api::service_handles{
           .transactions = services.get<forge::chain::api::transaction>(forge::chain::api::transaction::ref()),
       }},
       verifier,
   };

   static_cast<void>(
       run(client.get_transaction_status({.id = id, .finality_from = forge::chain::protocol::block_id{}})));
   BOOST_TEST(verifier->transaction_verifications == 1U);
}

BOOST_AUTO_TEST_CASE(verified_transaction_uses_an_unaudited_height_hint_then_a_retained_trust_anchor) {
   auto id = forge::chain::protocol::transaction_id{};
   id._hash[0] = 41U;
   auto response = forge::chain::protocol::transaction_status_response{};
   response.id = id;
   response.state = forge::chain::protocol::transaction_lifecycle::finalized;
   response.block_num = 7U;
   response.context.anchor = forge::chain::protocol::state_anchor{.block_num = 7U};
   response.audit = forge::chain::protocol::audit_bundle{
       .finality = forge::chain::protocol::proof_blob{.scheme = "test.finality"},
       .transaction = forge::chain::protocol::transaction_inclusion_proof{},
   };

   auto confirmation = response;
   confirmation.block_num = 8U;
   confirmation.context.anchor = forge::chain::protocol::state_anchor{.block_num = 8U};

   auto services = forge::api::core::registry{};
   auto service = std::make_shared<transaction_service>(
       std::vector<forge::chain::protocol::transaction_status_response>{response, confirmation},
       std::vector<forge::chain::protocol::transaction_status_response>{response});
   services.install<forge::chain::api::transaction>(service);
   auto verifier = std::make_shared<accepting_audit_verifier>();
   auto retained = forge::chain::protocol::block_id{};
   retained._hash[0] = 3U;
   verifier->retained_anchor = retained;
   auto client = forge::chain::api::verified_client{
       forge::chain::api::raw_client{forge::chain::api::service_handles{
           .transactions = services.get<forge::chain::api::transaction>(forge::chain::api::transaction::ref()),
       }},
       verifier,
   };

   const auto status = run(client.get_transaction_status({.id = id}));
   BOOST_REQUIRE(status.block_num);
   BOOST_CHECK_EQUAL(*status.block_num, 8U);
   BOOST_REQUIRE_EQUAL(service->status_requests.size(), 2U);
   BOOST_CHECK(service->status_requests[0].audit == forge::chain::protocol::audit_mode::none);
   BOOST_CHECK(!service->status_requests[0].finality_from.has_value());
   BOOST_CHECK(service->status_requests[1].audit == forge::chain::protocol::audit_mode::required);
   BOOST_REQUIRE(service->status_requests[1].finality_from.has_value());
   BOOST_CHECK(*service->status_requests[1].finality_from == retained);
   BOOST_REQUIRE_EQUAL(verifier->finality_anchor_targets.size(), 1U);
   BOOST_CHECK_EQUAL(verifier->finality_anchor_targets[0], 7U);

   service->status_requests.clear();
   const auto awaited = run(client.await_transaction({.id = id}));
   BOOST_REQUIRE(awaited.block_num);
   BOOST_CHECK_EQUAL(*awaited.block_num, 8U);
   BOOST_REQUIRE_EQUAL(service->await_requests.size(), 1U);
   BOOST_CHECK(service->await_requests[0].audit == forge::chain::protocol::audit_mode::none);
   BOOST_CHECK(!service->await_requests[0].finality_from.has_value());
   BOOST_REQUIRE_EQUAL(service->status_requests.size(), 1U);
   BOOST_CHECK(service->status_requests[0].audit == forge::chain::protocol::audit_mode::required);
   BOOST_REQUIRE(service->status_requests[0].finality_from.has_value());
   BOOST_CHECK(*service->status_requests[0].finality_from == retained);
   BOOST_REQUIRE_EQUAL(verifier->finality_anchor_targets.size(), 2U);
   BOOST_CHECK_EQUAL(verifier->finality_anchor_targets[1], 7U);

   service->status_requests.clear();
   auto explicit_anchor = forge::chain::protocol::block_id{};
   explicit_anchor._hash[0] = 2U;
   static_cast<void>(run(client.get_transaction_status({.id = id, .finality_from = explicit_anchor})));
   BOOST_REQUIRE_EQUAL(service->status_requests.size(), 1U);
   BOOST_CHECK(service->status_requests[0].audit == forge::chain::protocol::audit_mode::required);
   BOOST_REQUIRE(service->status_requests[0].finality_from.has_value());
   BOOST_CHECK(*service->status_requests[0].finality_from == explicit_anchor);
   BOOST_CHECK_EQUAL(verifier->finality_anchor_targets.size(), 2U);
}

BOOST_AUTO_TEST_CASE(verified_transaction_hint_requires_a_bound_height_and_retained_anchor) {
   auto id = forge::chain::protocol::transaction_id{};
   id._hash[0] = 43U;
   auto response = forge::chain::protocol::transaction_status_response{};
   response.id = id;
   response.state = forge::chain::protocol::transaction_lifecycle::finalized;
   response.block_num = 7U;
   response.context.anchor = forge::chain::protocol::state_anchor{.block_num = 7U};
   response.audit = forge::chain::protocol::audit_bundle{
       .finality = forge::chain::protocol::proof_blob{.scheme = "test.finality"},
       .transaction = forge::chain::protocol::transaction_inclusion_proof{},
   };

   const auto make_client = [](const std::shared_ptr<transaction_service>& service,
                               const std::shared_ptr<accepting_audit_verifier>& verifier) {
      auto services = forge::api::core::registry{};
      services.install<forge::chain::api::transaction>(service);
      return forge::chain::api::verified_client{
          forge::chain::api::raw_client{forge::chain::api::service_handles{
              .transactions = services.get<forge::chain::api::transaction>(forge::chain::api::transaction::ref()),
          }},
          verifier,
      };
   };

   {
      auto missing_height = response;
      missing_height.block_num.reset();
      const auto service = std::make_shared<transaction_service>(std::move(missing_height));
      const auto verifier = std::make_shared<accepting_audit_verifier>();
      auto client = make_client(service, verifier);
      BOOST_CHECK_THROW(static_cast<void>(run(client.get_transaction_status({.id = id}))),
                        forge::chain::api::exceptions::invalid_transaction_proof);
      BOOST_REQUIRE_EQUAL(service->status_requests.size(), 1U);
      BOOST_CHECK(service->status_requests.front().audit == forge::chain::protocol::audit_mode::none);
      BOOST_CHECK(verifier->finality_anchor_targets.empty());
   }
   {
      auto wrong_id = response;
      ++wrong_id.id._hash[0];
      const auto service = std::make_shared<transaction_service>(std::move(wrong_id));
      const auto verifier = std::make_shared<accepting_audit_verifier>();
      auto client = make_client(service, verifier);
      BOOST_CHECK_THROW(static_cast<void>(run(client.get_transaction_status({.id = id}))),
                        forge::chain::api::exceptions::invalid_transaction_proof);
      BOOST_REQUIRE_EQUAL(service->status_requests.size(), 1U);
      BOOST_CHECK(verifier->finality_anchor_targets.empty());
   }
   {
      auto inconsistent_block = response;
      inconsistent_block.block = forge::chain::protocol::block_id{};
      const auto service = std::make_shared<transaction_service>(std::move(inconsistent_block));
      const auto verifier = std::make_shared<accepting_audit_verifier>();
      auto client = make_client(service, verifier);
      BOOST_CHECK_THROW(static_cast<void>(run(client.get_transaction_status({.id = id}))),
                        forge::chain::api::exceptions::invalid_transaction_proof);
      BOOST_REQUIRE_EQUAL(service->status_requests.size(), 1U);
      BOOST_CHECK(verifier->finality_anchor_targets.empty());
   }
   {
      const auto service = std::make_shared<transaction_service>(response);
      const auto verifier = std::make_shared<accepting_audit_verifier>();
      verifier->preferred_anchor.reset();
      auto client = make_client(service, verifier);
      BOOST_CHECK_THROW(static_cast<void>(run(client.get_transaction_status({.id = id}))),
                        forge::chain::api::exceptions::anchor_unavailable);
      BOOST_REQUIRE_EQUAL(service->status_requests.size(), 1U);
      BOOST_CHECK(service->status_requests.front().audit == forge::chain::protocol::audit_mode::none);
      BOOST_REQUIRE_EQUAL(verifier->finality_anchor_targets.size(), 1U);
      BOOST_CHECK_EQUAL(verifier->finality_anchor_targets.front(), 7U);
   }
   {
      const auto service = std::make_shared<transaction_service>(response);
      const auto verifier = std::make_shared<accepting_audit_verifier>();
      verifier->preferred_anchor.reset();
      auto client = make_client(service, verifier);
      BOOST_CHECK_THROW(static_cast<void>(run(client.await_transaction({.id = id}))),
                        forge::chain::api::exceptions::anchor_unavailable);
      BOOST_REQUIRE_EQUAL(service->await_requests.size(), 1U);
      BOOST_CHECK(service->await_requests.front().audit == forge::chain::protocol::audit_mode::none);
      BOOST_CHECK(service->status_requests.empty());
      BOOST_REQUIRE_EQUAL(verifier->finality_anchor_targets.size(), 1U);
      BOOST_CHECK_EQUAL(verifier->finality_anchor_targets.front(), 7U);
   }
}

BOOST_AUTO_TEST_CASE(submission_client_binds_acknowledgements_to_local_transaction_ids) {
   auto first = forge::chain::protocol::transaction_submit_request{};
   auto first_transaction = forge::chain::protocol::signed_transaction{};
   first_transaction.expiration = forge::chain::protocol::time_point_sec{1U};
   first.transaction = forge::chain::protocol::packed_transaction{std::move(first_transaction)};
   auto second = forge::chain::protocol::transaction_submit_request{};
   auto second_transaction = forge::chain::protocol::signed_transaction{};
   second_transaction.expiration = forge::chain::protocol::time_point_sec{2U};
   second.transaction = forge::chain::protocol::packed_transaction{std::move(second_transaction)};
   const auto first_id = first.transaction.id();
   const auto second_id = second.transaction.id();
   const auto batch = [&] {
      return forge::chain::protocol::transaction_submit_batch_request{.transactions = {first, second}};
   };

   const auto make_client = [&](std::vector<forge::chain::protocol::transaction_submit_response> responses) {
      auto services = forge::api::core::registry{};
      services.install<forge::chain::api::submission>(std::make_shared<submission_service>(std::move(responses)));
      return forge::chain::api::submission_client{
          services.get<forge::chain::api::submission>(forge::chain::api::submission::ref()),
      };
   };

   {
      auto client = make_client({forge::chain::protocol::transaction_submit_response{.id = first_id}});
      BOOST_TEST(run(client.submit(first)).id == first_id);
   }
   {
      auto client = make_client({forge::chain::protocol::transaction_submit_response{.id = second_id}});
      BOOST_CHECK_THROW(static_cast<void>(run(client.submit(first))),
                        forge::chain::api::exceptions::invalid_transaction_proof);
   }
   {
      auto response = forge::chain::protocol::transaction_submit_response{.id = first_id};
      response.trace = forge::chain::protocol::transaction_trace{.id = second_id};
      auto client = make_client({std::move(response)});
      BOOST_CHECK_THROW(static_cast<void>(run(client.submit(first))),
                        forge::chain::api::exceptions::invalid_transaction_proof);
   }
   {
      auto client = make_client({forge::chain::protocol::transaction_submit_response{.id = first_id},
                                 forge::chain::protocol::transaction_submit_response{.id = second_id}});
      const auto responses = run(client.submit_batch(batch()));
      BOOST_TEST(responses.size() == 2U);
      BOOST_TEST(responses[0].id == first_id);
      BOOST_TEST(responses[1].id == second_id);
   }
   {
      auto client = make_client({forge::chain::protocol::transaction_submit_response{.id = second_id},
                                 forge::chain::protocol::transaction_submit_response{.id = first_id}});
      BOOST_CHECK_THROW(static_cast<void>(run(client.submit_batch(batch()))),
                        forge::chain::api::exceptions::invalid_transaction_proof);
   }
   {
      auto first_response = forge::chain::protocol::transaction_submit_response{.id = first_id};
      auto second_response = forge::chain::protocol::transaction_submit_response{.id = second_id};
      second_response.trace = forge::chain::protocol::transaction_trace{.id = first_id};
      auto client = make_client({std::move(first_response), std::move(second_response)});
      BOOST_CHECK_THROW(static_cast<void>(run(client.submit_batch(batch()))),
                        forge::chain::api::exceptions::invalid_transaction_proof);
   }
   {
      auto client = make_client({forge::chain::protocol::transaction_submit_response{.id = first_id}});
      BOOST_CHECK_THROW(static_cast<void>(run(client.submit_batch(batch()))),
                        forge::chain::api::exceptions::unavailable);
   }
}

BOOST_AUTO_TEST_CASE(submission_client_fails_typed_without_a_transport) {
   auto client = forge::chain::api::submission_client{{}};
   BOOST_CHECK_THROW(static_cast<void>(run(client.submit({}))), forge::chain::api::exceptions::unavailable);
}

BOOST_AUTO_TEST_CASE(submission_client_enforces_local_limits_and_translates_service_failures) {
   auto services = forge::api::core::registry{};
   auto service =
       std::make_shared<submission_service>(std::vector<forge::chain::protocol::transaction_submit_response>{});
   services.install<forge::chain::api::submission>(service);
   auto limits = forge::chain::protocol::service_limits{};
   limits.max_transaction_batch_size = 1U;
   auto client = forge::chain::api::submission_client{
       services.get<forge::chain::api::submission>(forge::chain::api::submission::ref()), limits};

   BOOST_CHECK_THROW(static_cast<void>(run(client.submit_batch({}))), forge::chain::api::exceptions::invalid_request);
   BOOST_CHECK_THROW(static_cast<void>(run(client.submit_batch({.transactions = {{}, {}}}))),
                     forge::chain::api::exceptions::resource_exhausted);
   service->throw_standard = true;
   auto request = forge::chain::protocol::transaction_submit_request{};
   request.transaction = forge::chain::protocol::packed_transaction{forge::chain::protocol::signed_transaction{}};
   BOOST_CHECK_THROW(static_cast<void>(run(client.submit(std::move(request)))),
                     forge::chain::api::exceptions::unavailable);
   service->throw_standard = false;
   service->throw_forge = true;
   request = forge::chain::protocol::transaction_submit_request{};
   request.transaction = forge::chain::protocol::packed_transaction{forge::chain::protocol::signed_transaction{}};
   BOOST_CHECK_THROW(static_cast<void>(run(client.submit(std::move(request)))),
                     forge::chain::api::exceptions::unavailable);
   service->throw_forge = false;
   service->throw_timed_out = true;
   request = forge::chain::protocol::transaction_submit_request{};
   request.transaction = forge::chain::protocol::packed_transaction{forge::chain::protocol::signed_transaction{}};
   BOOST_CHECK_THROW(static_cast<void>(run(client.submit(std::move(request)))),
                     forge::chain::api::exceptions::deadline_exceeded);
   service->throw_timed_out = false;
   service->throw_api_canceled = true;
   request = forge::chain::protocol::transaction_submit_request{};
   request.transaction = forge::chain::protocol::packed_transaction{forge::chain::protocol::signed_transaction{}};
   BOOST_CHECK_THROW(static_cast<void>(run(client.submit(std::move(request)))), forge::asio::exceptions::canceled);
   auto batch_request = forge::chain::protocol::transaction_submit_request{};
   batch_request.transaction = forge::chain::protocol::packed_transaction{forge::chain::protocol::signed_transaction{}};
   BOOST_CHECK_THROW(static_cast<void>(run(client.submit_batch({.transactions = {std::move(batch_request)}}))),
                     forge::asio::exceptions::canceled);
   service->throw_api_canceled = false;
   service->throw_api_deadline = true;
   request = forge::chain::protocol::transaction_submit_request{};
   request.transaction = forge::chain::protocol::packed_transaction{forge::chain::protocol::signed_transaction{}};
   BOOST_CHECK_THROW(static_cast<void>(run(client.submit(std::move(request)))),
                     forge::chain::api::exceptions::deadline_exceeded);
   batch_request = forge::chain::protocol::transaction_submit_request{};
   batch_request.transaction = forge::chain::protocol::packed_transaction{forge::chain::protocol::signed_transaction{}};
   BOOST_CHECK_THROW(static_cast<void>(run(client.submit_batch({.transactions = {std::move(batch_request)}}))),
                     forge::chain::api::exceptions::deadline_exceeded);
}

BOOST_AUTO_TEST_CASE(verified_transaction_status_rejects_an_unauthenticated_execution_trace) {
   auto id = forge::chain::protocol::transaction_id{};
   id._hash[0] = 32U;
   auto response = forge::chain::protocol::transaction_status_response{};
   response.id = id;
   response.state = forge::chain::protocol::transaction_lifecycle::finalized;
   response.trace = forge::chain::protocol::transaction_trace{.id = id};
   response.context.anchor = forge::chain::protocol::state_anchor{.block_num = 7U};
   response.audit = forge::chain::protocol::audit_bundle{
       .finality = forge::chain::protocol::proof_blob{.scheme = "test.finality"},
       .transaction = forge::chain::protocol::transaction_inclusion_proof{},
   };

   auto services = forge::api::core::registry{};
   services.install<forge::chain::api::transaction>(std::make_shared<transaction_service>(std::move(response)));
   auto verifier = std::make_shared<accepting_audit_verifier>();
   auto client = forge::chain::api::verified_client{
       forge::chain::api::raw_client{forge::chain::api::service_handles{
           .transactions = services.get<forge::chain::api::transaction>(forge::chain::api::transaction::ref()),
       }},
       verifier,
   };

   BOOST_CHECK_THROW(static_cast<void>(run(client.get_transaction_status(
                         {.id = id, .finality_from = forge::chain::protocol::block_id{}}))),
                     forge::chain::api::exceptions::invalid_transaction_proof);
   BOOST_TEST(verifier->transaction_verifications == 0U);
}

BOOST_AUTO_TEST_CASE(verified_transaction_translates_verifier_failures_to_typed_errors) {
   auto id = forge::chain::protocol::transaction_id{};
   id._hash[0] = 33U;
   auto response = forge::chain::protocol::transaction_status_response{};
   response.id = id;
   response.state = forge::chain::protocol::transaction_lifecycle::included;
   response.context.anchor = forge::chain::protocol::state_anchor{};
   response.audit = forge::chain::protocol::audit_bundle{
       .finality = forge::chain::protocol::proof_blob{.scheme = "test.finality"},
       .transaction = forge::chain::protocol::transaction_inclusion_proof{},
   };

   auto services = forge::api::core::registry{};
   services.install<forge::chain::api::transaction>(std::make_shared<transaction_service>(std::move(response)));
   auto verifier = std::make_shared<accepting_audit_verifier>();
   verifier->throw_standard_transaction = true;
   auto client = forge::chain::api::verified_client{
       forge::chain::api::raw_client{forge::chain::api::service_handles{
           .transactions = services.get<forge::chain::api::transaction>(forge::chain::api::transaction::ref()),
       }},
       verifier,
   };

   BOOST_CHECK_THROW(static_cast<void>(run(client.get_transaction_status(
                         {.id = id, .finality_from = forge::chain::protocol::block_id{}}))),
                     forge::chain::api::exceptions::invalid_transaction_proof);
}

BOOST_AUTO_TEST_CASE(verified_composite_response_delegates_product_projection_and_authenticated_sources) {
   auto anchor = make_finality_anchor();
   auto response = forge::chain::protocol::account_response{};
   response.account.name = forge::chain::protocol::account_name{"alice"};
   response.context = forge::chain::protocol::response_context{
       .chain = anchor.chain,
       .head = anchor.block,
       .finalized = anchor.block,
       .anchor = anchor,
   };
   response.audit = forge::chain::protocol::audit_bundle{
       .finality = forge::chain::protocol::proof_blob{.scheme = "test.finality"},
       .state = {forge::chain::protocol::proof_blob{.scheme = "test.state"}},
   };

   auto services = forge::api::core::registry{};
   services.install<forge::chain::api::state>(std::make_shared<state_service>(std::move(response)));
   auto audit = std::make_shared<accepting_audit_verifier>();
   audit->point_value = forge::chain::protocol::bytes{1U, 2U};
   auto projections = std::make_shared<account_projection_verifier>();
   auto client = forge::chain::api::verified_client{
       forge::chain::api::raw_client{forge::chain::api::service_handles{
           .state_queries = services.get<forge::chain::api::state>(forge::chain::api::state::ref()),
       }},
       audit,
       projections,
   };

   const auto result =
       run(client.get_account(account_by_name(forge::chain::protocol::account_name{"alice"}, anchor.block)));

   BOOST_CHECK(result.account.name == forge::chain::protocol::account_name{"alice"});
   BOOST_TEST(projections->verifications == 1U);
   BOOST_TEST(audit->state_point_verifications == 1U);
}

BOOST_AUTO_TEST_CASE(verified_composite_response_translates_projection_failures_to_typed_errors) {
   auto anchor = make_finality_anchor();
   auto response = forge::chain::protocol::account_response{};
   response.context.anchor = anchor;
   response.audit = forge::chain::protocol::audit_bundle{
       .finality = forge::chain::protocol::proof_blob{.scheme = "test.finality"},
       .state = {forge::chain::protocol::proof_blob{.scheme = "test.state"}},
   };

   const auto verify = [&](bool nonstandard) {
      auto services = forge::api::core::registry{};
      services.install<forge::chain::api::state>(std::make_shared<state_service>(response));
      auto projections = std::make_shared<throwing_account_projection_verifier>();
      projections->nonstandard = nonstandard;
      auto client = forge::chain::api::verified_client{
          forge::chain::api::raw_client{forge::chain::api::service_handles{
              .state_queries = services.get<forge::chain::api::state>(forge::chain::api::state::ref()),
          }},
          std::make_shared<accepting_audit_verifier>(),
          std::move(projections),
      };
      static_cast<void>(
          run(client.get_account(account_by_name(forge::chain::protocol::account_name{"alice"}, anchor.block))));
   };

   BOOST_CHECK_THROW(verify(false), forge::chain::api::exceptions::invalid_state_proof);
   BOOST_CHECK_THROW(verify(true), forge::chain::api::exceptions::invalid_state_proof);
}

BOOST_AUTO_TEST_CASE(verified_client_fails_closed_for_methods_without_content_witnesses) {
   auto client = forge::chain::api::verified_client{
       forge::chain::api::raw_client{forge::chain::api::service_handles{}},
       std::make_shared<accepting_audit_verifier>(),
   };

   BOOST_CHECK_THROW(run(client.get_block_state(forge::chain::protocol::block_request{})),
                     forge::chain::api::exceptions::audit_not_supported);
   BOOST_CHECK_THROW(run(client.get_canonical_range(forge::chain::protocol::block_range_request{})),
                     forge::chain::api::exceptions::audit_not_supported);
   BOOST_CHECK_THROW(run(client.get_activated_protocol_features(forge::chain::protocol::protocol_features_request{})),
                     forge::chain::api::exceptions::audit_not_supported);
   BOOST_CHECK_THROW(run(client.get_consensus_parameters(forge::chain::protocol::anchored_request{})),
                     forge::chain::api::exceptions::audit_not_supported);
   BOOST_CHECK_THROW(run(client.get_producers(forge::chain::protocol::producers_request{})),
                     forge::chain::api::exceptions::audit_not_supported);
   BOOST_CHECK_THROW(run(client.get_producer_schedule(forge::chain::protocol::anchored_request{})),
                     forge::chain::api::exceptions::audit_not_supported);
   BOOST_CHECK_THROW(run(client.get_finalizer_info(forge::chain::protocol::anchored_request{})),
                     forge::chain::api::exceptions::audit_not_supported);

   BOOST_CHECK_THROW(run(client.get_account(forge::chain::protocol::account_request{})),
                     forge::chain::api::exceptions::audit_not_supported);
   BOOST_CHECK_THROW(run(client.get_account_changes(forge::chain::protocol::account_changes_request{})),
                     forge::chain::api::exceptions::audit_not_supported);
   BOOST_CHECK_THROW(run(client.get_code(forge::chain::protocol::code_request{})),
                     forge::chain::api::exceptions::audit_not_supported);
   BOOST_CHECK_THROW(run(client.get_permission_links(forge::chain::protocol::permission_links_request{})),
                     forge::chain::api::exceptions::audit_not_supported);
   BOOST_CHECK_THROW(run(client.get_table_rows(forge::chain::protocol::table_rows_request{})),
                     forge::chain::api::exceptions::audit_not_supported);
   BOOST_CHECK_THROW(run(client.get_table_changes(forge::chain::protocol::table_changes_request{})),
                     forge::chain::api::exceptions::audit_not_supported);
   BOOST_CHECK_THROW(run(client.get_table_scope(forge::chain::protocol::table_scope_request{})),
                     forge::chain::api::exceptions::audit_not_supported);
   BOOST_CHECK_THROW(run(client.get_currency_balance(forge::chain::protocol::currency_balance_request{})),
                     forge::chain::api::exceptions::audit_not_supported);
   BOOST_CHECK_THROW(run(client.get_currency_stats(forge::chain::protocol::currency_stats_request{})),
                     forge::chain::api::exceptions::audit_not_supported);
   BOOST_CHECK_THROW(run(client.get_scheduled_transactions(forge::chain::protocol::scheduled_request{})),
                     forge::chain::api::exceptions::audit_not_supported);
   BOOST_CHECK_THROW(run(client.get_accounts_by_authorizers(forge::chain::protocol::authorizers_request{})),
                     forge::chain::api::exceptions::audit_not_supported);

   BOOST_CHECK_THROW(run(client.get_required_keys(forge::chain::protocol::transaction_required_keys_request{})),
                     forge::chain::api::exceptions::audit_not_supported);
   BOOST_CHECK_THROW(run(client.compute_transaction(forge::chain::protocol::transaction_read_only_request{})),
                     forge::chain::api::exceptions::audit_not_supported);
   BOOST_CHECK_THROW(run(client.send_read_only_transaction(forge::chain::protocol::transaction_read_only_request{})),
                     forge::chain::api::exceptions::audit_not_supported);
}

BOOST_AUTO_TEST_CASE(verified_change_requests_select_the_first_possible_witness_anchor) {
   auto services = forge::api::core::registry{};
   services.install<forge::chain::api::state>(
       std::make_shared<state_service>(forge::chain::protocol::table_changes_response{}));
   auto verifier = std::make_shared<accepting_audit_verifier>();
   auto client = forge::chain::api::verified_client{
       forge::chain::api::raw_client{forge::chain::api::service_handles{
           .state_queries = services.get<forge::chain::api::state>(forge::chain::api::state::ref()),
       }},
       verifier,
       std::make_shared<typed_changes_projection_verifier>(),
   };

   const auto check_table = [&](std::uint32_t from_block, std::uint32_t to_block, std::uint32_t expected_target) {
      verifier->finality_anchor_targets.clear();
      BOOST_CHECK_THROW(static_cast<void>(run(client.get_table_changes({
                            .from_block = from_block,
                            .to_block = to_block,
                            .tables = {{.code = forge::chain::protocol::account_name{"table"}}},
                        }))),
                        forge::chain::api::exceptions::audit_not_supported);
      BOOST_REQUIRE_EQUAL(verifier->finality_anchor_targets.size(), 1U);
      BOOST_CHECK_EQUAL(verifier->finality_anchor_targets.front(), expected_target);
   };
   const auto check_account = [&](std::uint32_t from_block, std::uint32_t to_block, std::uint32_t expected_target) {
      verifier->finality_anchor_targets.clear();
      BOOST_CHECK_THROW(static_cast<void>(run(client.get_account_changes({
                            .from_block = from_block,
                            .to_block = to_block,
                            .accounts = {forge::chain::protocol::account_name{"account"}},
                        }))),
                        forge::chain::api::exceptions::audit_not_supported);
      BOOST_REQUIRE_EQUAL(verifier->finality_anchor_targets.size(), 1U);
      BOOST_CHECK_EQUAL(verifier->finality_anchor_targets.front(), expected_target);
   };

   check_table(10U, 12U, 11U);
   check_account(10U, 12U, 11U);
   check_table(12U, 12U, 12U);
   check_account(12U, 12U, 12U);
   check_table(std::numeric_limits<std::uint32_t>::max() - 1U, std::numeric_limits<std::uint32_t>::max(),
               std::numeric_limits<std::uint32_t>::max());
   check_account(std::numeric_limits<std::uint32_t>::max() - 1U, std::numeric_limits<std::uint32_t>::max(),
                 std::numeric_limits<std::uint32_t>::max());
   check_table(std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max(),
               std::numeric_limits<std::uint32_t>::max());
   check_account(std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max(),
                 std::numeric_limits<std::uint32_t>::max());
}

BOOST_AUTO_TEST_CASE(verified_table_changes_bind_opaque_cursor_and_enforce_lww_projection) {
   auto anchor = make_finality_anchor();
   anchor.block_num = 12U;
   auto intermediate = anchor;
   intermediate.block._hash[0] = 11U;
   intermediate.block_num = 11U;
   const auto tables = std::vector{
       forge::chain::protocol::table_change_selector{.code = forge::chain::protocol::account_name{"alpha"}},
       forge::chain::protocol::table_change_selector{.code = forge::chain::protocol::account_name{"beta"}},
   };
   const auto cursor = forge::chain::protocol::bytes{0x01U, 0x02U};
   const auto next = forge::chain::protocol::bytes{0x03U, 0x04U};
   const auto request = forge::chain::protocol::table_changes_request{
       .from_block = 10U,
       .to_block = 12U,
       .tables = tables,
       .limit = 8U,
       .cursor = cursor,
   };
   auto response = forge::chain::protocol::table_changes_response{
       .blocks =
           {
               {.anchor = intermediate,
                .mutations = {{.table = tables.front(),
                               .primary = 7U,
                               .row = forge::chain::protocol::table_row{.value = {0xaaU}}},
                              {.table = tables.front(),
                               .primary = 8U,
                               .row = forge::chain::protocol::table_row{.value = {0xbbU}}}}},
               {.anchor = anchor, .mutations = {{.table = tables.back(), .primary = 9U}}},
           },
       .next = next,
   };
   response.context = {.chain = anchor.chain, .head = anchor.block, .finalized = anchor.block, .anchor = anchor};
   response.audit = forge::chain::protocol::audit_bundle{
       .finality = forge::chain::protocol::proof_blob{.scheme = "test.finality"},
       .ancestry = forge::chain::protocol::proof_blob{.scheme = "test.ancestry"},
       .state = {forge::chain::protocol::proof_blob{.scheme = "test.changes.11.a"},
                 forge::chain::protocol::proof_blob{.scheme = "test.changes.11.b"},
                 forge::chain::protocol::proof_blob{.scheme = "test.changes.12"}},
   };

   const auto verify = [&](forge::chain::protocol::table_changes_response candidate,
                           std::vector<forge::chain::protocol::table_change_selector> expected_tables) {
      auto services = forge::api::core::registry{};
      services.install<forge::chain::api::state>(std::make_shared<state_service>(std::move(candidate)));
      auto audit = std::make_shared<accepting_audit_verifier>();
      audit->expected_state_change_proofs = std::vector<std::pair<std::uint32_t, std::string>>{
          {11U, "test.changes.11.a"}, {11U, "test.changes.11.b"}, {12U, "test.changes.12"}};
      auto projections = std::make_shared<typed_changes_projection_verifier>();
      projections->expected_chain = anchor.chain;
      projections->expected_anchor = anchor.block;
      projections->expected_tables = std::move(expected_tables);
      projections->expected_table_blocks = response.blocks;
      projections->expected_table_proofs_per_batch = std::vector<std::size_t>{2U, 1U};
      projections->expected_table_cursor = cursor;
      projections->expected_next = next;
      auto client = forge::chain::api::verified_client{
          forge::chain::api::raw_client{forge::chain::api::service_handles{
              .state_queries = services.get<forge::chain::api::state>(forge::chain::api::state::ref()),
          }},
          audit,
          projections,
      };
      return std::tuple{run(client.get_table_changes(request)), std::move(audit), std::move(projections)};
   };

   const auto [result, audit, projections] = verify(response, tables);
   BOOST_TEST(result.blocks.size() == 2U);
   BOOST_TEST(!result.blocks.back().mutations.back().row.has_value());
   BOOST_TEST(audit->state_change_verifications == 3U);
   BOOST_TEST(audit->ancestry_verifications == 1U);
   BOOST_REQUIRE(audit->ancestry_finalized);
   BOOST_CHECK(*audit->ancestry_finalized == anchor);
   BOOST_REQUIRE_EQUAL(audit->ancestry_intermediate.size(), 1U);
   BOOST_CHECK(audit->ancestry_intermediate.front() == intermediate);
   BOOST_REQUIRE_EQUAL(audit->state_change_anchors.size(), 3U);
   BOOST_CHECK(audit->state_change_anchors.front() == intermediate);
   BOOST_CHECK(audit->state_change_anchors[1] == intermediate);
   BOOST_CHECK(audit->state_change_anchors.back() == anchor);
   BOOST_TEST(projections->table_verifications == 1U);
   BOOST_REQUIRE(projections->last_table_request);
   BOOST_TEST(static_cast<std::uint8_t>(projections->last_table_request->audit) ==
              static_cast<std::uint8_t>(forge::chain::protocol::audit_mode::required));

   auto duplicate = response;
   duplicate.blocks.front().mutations.push_back(duplicate.blocks.front().mutations.front());
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(duplicate), tables)),
                     forge::chain::api::exceptions::invalid_state_proof);

   auto reordered_mutations = response;
   std::ranges::reverse(reordered_mutations.blocks.front().mutations);
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(reordered_mutations), tables)),
                     forge::chain::api::exceptions::invalid_state_proof);

   auto omitted_mutation = response;
   omitted_mutation.blocks.front().mutations.pop_back();
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(omitted_mutation), tables)),
                     forge::chain::api::exceptions::invalid_state_proof);

   auto reordered = response;
   std::ranges::reverse(reordered.blocks);
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(reordered), tables)),
                     forge::chain::api::exceptions::invalid_state_proof);

   auto omitted = response;
   omitted.blocks.pop_back();
   omitted.audit->state.pop_back();
   omitted.next.reset();
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(omitted), tables)),
                     forge::chain::api::exceptions::invalid_state_proof);

   auto mismatched_proofs = response;
   std::ranges::reverse(mismatched_proofs.audit->state);
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(mismatched_proofs), tables)),
                     forge::chain::api::exceptions::invalid_state_proof);

   auto omitted_proof = response;
   omitted_proof.audit->state.pop_back();
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(omitted_proof), tables)),
                     forge::chain::api::exceptions::invalid_state_proof);

   auto wrong_anchor = response;
   wrong_anchor.context.anchor->block_num = 13U;
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(wrong_anchor), tables)),
                     forge::chain::api::exceptions::invalid_state_proof);

   auto wrong_target_block = response;
   wrong_target_block.context.anchor->block._hash[0] ^= 0xffU;
   wrong_target_block.blocks.back().anchor = *wrong_target_block.context.anchor;
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(wrong_target_block), tables)),
                     forge::chain::api::exceptions::invalid_state_proof);

   auto wrong_chain = response;
   wrong_chain.context.chain._hash[0] ^= 0xffU;
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(wrong_chain), tables)),
                     forge::chain::api::exceptions::invalid_state_proof);

   auto wrong_position = response;
   wrong_position.next = forge::chain::protocol::bytes{0xffU};
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(wrong_position), tables)),
                     forge::chain::api::exceptions::invalid_state_proof);

   auto empty_next = response;
   empty_next.next = forge::chain::protocol::bytes{};
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(empty_next), tables)),
                     forge::chain::api::exceptions::unavailable);

   auto wrong_tables = tables;
   wrong_tables.back().code = forge::chain::protocol::account_name{"gamma"};
   BOOST_CHECK_THROW(static_cast<void>(verify(response, std::move(wrong_tables))),
                     forge::chain::api::exceptions::invalid_state_proof);
}

BOOST_AUTO_TEST_CASE(verified_account_changes_reuse_account_authority_and_reject_malicious_projections) {
   auto anchor = make_finality_anchor();
   anchor.block_num = 22U;
   auto intermediate = anchor;
   intermediate.block._hash[0] = 21U;
   intermediate.block_num = 21U;
   const auto accounts =
       std::vector{forge::chain::protocol::account_name{"alice"}, forge::chain::protocol::account_name{"bob"}};
   const auto cursor = forge::chain::protocol::bytes{0xa1U};
   const auto request = forge::chain::protocol::account_changes_request{
       .from_block = 20U,
       .to_block = 22U,
       .accounts = accounts,
       .cursor = cursor,
   };
   auto response = forge::chain::protocol::account_changes_response{
       .blocks =
           {{.anchor = intermediate,
             .mutations = {{.account = accounts.front(), .authority = forge::chain::protocol::account_authority{}},
                           {.account = accounts.back(), .authority = forge::chain::protocol::account_authority{}}}},
            {.anchor = anchor, .mutations = {{.account = accounts.back()}}}},
   };
   response.context = {.chain = anchor.chain, .head = anchor.block, .finalized = anchor.block, .anchor = anchor};
   response.audit = forge::chain::protocol::audit_bundle{
       .finality = forge::chain::protocol::proof_blob{.scheme = "test.finality"},
       .ancestry = forge::chain::protocol::proof_blob{.scheme = "test.ancestry"},
       .state = {forge::chain::protocol::proof_blob{.scheme = "test.changes.21"},
                 forge::chain::protocol::proof_blob{.scheme = "test.changes.22"}},
   };

   const auto verify = [&](forge::chain::protocol::account_changes_response candidate,
                           std::optional<forge::chain::protocol::bytes> expected_cursor = std::nullopt) {
      auto services = forge::api::core::registry{};
      services.install<forge::chain::api::state>(std::make_shared<state_service>(std::move(candidate)));
      auto audit = std::make_shared<accepting_audit_verifier>();
      audit->expected_state_change_proofs =
          std::vector<std::pair<std::uint32_t, std::string>>{{21U, "test.changes.21"}, {22U, "test.changes.22"}};
      auto projections = std::make_shared<typed_changes_projection_verifier>();
      projections->expected_chain = anchor.chain;
      projections->expected_anchor = anchor.block;
      projections->expected_accounts = accounts;
      projections->expected_account_blocks = response.blocks;
      projections->expected_account_proofs_per_batch = std::vector<std::size_t>{1U, 1U};
      projections->expected_account_cursor = expected_cursor.value_or(cursor);
      auto client = forge::chain::api::verified_client{
          forge::chain::api::raw_client{forge::chain::api::service_handles{
              .state_queries = services.get<forge::chain::api::state>(forge::chain::api::state::ref()),
          }},
          audit,
          projections,
      };
      return std::tuple{run(client.get_account_changes(request)), std::move(audit), std::move(projections)};
   };

   const auto [result, audit, projections] = verify(response);
   BOOST_TEST(result.blocks.size() == 2U);
   BOOST_REQUIRE(result.blocks.front().mutations.front().authority);
   BOOST_TEST(!result.blocks.back().mutations.back().authority.has_value());
   BOOST_TEST(audit->state_change_verifications == 2U);
   BOOST_REQUIRE_EQUAL(audit->state_change_anchors.size(), 2U);
   BOOST_CHECK(audit->state_change_anchors.front() == intermediate);
   BOOST_CHECK(audit->state_change_anchors.back() == anchor);
   BOOST_TEST(projections->account_verifications == 1U);

   BOOST_CHECK_THROW(static_cast<void>(verify(response, forge::chain::protocol::bytes{0x01U, 0x02U})),
                     forge::chain::api::exceptions::invalid_state_proof);

   auto empty_next = response;
   empty_next.next = forge::chain::protocol::bytes{};
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(empty_next))), forge::chain::api::exceptions::unavailable);

   auto duplicate = response;
   duplicate.blocks.front().mutations.push_back(duplicate.blocks.front().mutations.front());
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(duplicate))),
                     forge::chain::api::exceptions::invalid_state_proof);

   auto reordered_mutations = response;
   std::ranges::reverse(reordered_mutations.blocks.front().mutations);
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(reordered_mutations))),
                     forge::chain::api::exceptions::invalid_state_proof);

   auto omitted_mutation = response;
   omitted_mutation.blocks.front().mutations.pop_back();
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(omitted_mutation))),
                     forge::chain::api::exceptions::invalid_state_proof);

   auto reordered = response;
   std::ranges::reverse(reordered.blocks);
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(reordered))),
                     forge::chain::api::exceptions::invalid_state_proof);

   auto omitted = response;
   omitted.blocks.pop_back();
   omitted.audit->state.pop_back();
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(omitted))), forge::chain::api::exceptions::invalid_state_proof);

   auto mismatched_proofs = response;
   std::ranges::reverse(mismatched_proofs.audit->state);
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(mismatched_proofs))),
                     forge::chain::api::exceptions::invalid_state_proof);

   auto omitted_proof = response;
   omitted_proof.audit->state.pop_back();
   BOOST_CHECK_THROW(static_cast<void>(verify(std::move(omitted_proof))),
                     forge::chain::api::exceptions::invalid_state_proof);
}

BOOST_AUTO_TEST_CASE(authenticated_audit_verifier_accepts_real_point_membership_and_nonmembership) {
   const auto fixture = make_authenticated_point_fixture();
   const auto anchor = authenticated_anchor(fixture.root);
   auto finality = std::make_shared<recording_finality_verifier>();
   auto verifier = forge::chain::api::authenticated_audit_verifier{
       {
           .chain = authenticated_chain(),
           .state_domain = fixture.domain,
           .proof_limits = {},
       },
       finality,
   };

   const auto membership = authenticated_point_proof(fixture, "alpha");
   const auto membership_value = verifier.verify_state_point(
       anchor, protocol_bytes("alpha"), authenticated_proof_blob("forge.db.authenticated.point", membership));
   BOOST_REQUIRE(membership_value.has_value());
   BOOST_CHECK(*membership_value == protocol_bytes("one"));

   const auto nonmembership = authenticated_point_proof(fixture, "beta");
   const auto absent = verifier.verify_state_point(
       anchor, protocol_bytes("beta"), authenticated_proof_blob("forge.db.authenticated.point", nonmembership));
   BOOST_TEST(!absent.has_value());
}

BOOST_AUTO_TEST_CASE(authenticated_audit_verifier_rejects_membership_without_value_bytes) {
   const auto fixture = make_authenticated_point_fixture();
   const auto anchor = authenticated_anchor(fixture.root);
   auto verifier = forge::chain::api::authenticated_audit_verifier{
       {
           .chain = authenticated_chain(),
           .state_domain = fixture.domain,
           .proof_limits = {},
       },
       std::make_shared<recording_finality_verifier>(),
   };

   auto proof = authenticated_point_proof(fixture, "alpha");
   BOOST_REQUIRE(proof.terminal.has_value());
   proof.terminal->value.reset();

   BOOST_CHECK_THROW(
       static_cast<void>(verifier.verify_state_point(anchor, protocol_bytes("alpha"),
                                                     authenticated_proof_blob("forge.db.authenticated.point", proof))),
       forge::chain::api::exceptions::invalid_state_proof);
}

BOOST_AUTO_TEST_CASE(authenticated_audit_verifier_rejects_wrong_scheme_version_limits_chain_and_root) {
   const auto fixture = make_authenticated_point_fixture();
   const auto anchor = authenticated_anchor(fixture.root);
   const auto key = protocol_bytes("alpha");
   const auto proof = authenticated_point_proof(fixture, "alpha");
   const auto valid = authenticated_proof_blob("forge.db.authenticated.point", proof);
   auto finality = std::make_shared<recording_finality_verifier>();
   auto verifier = forge::chain::api::authenticated_audit_verifier{
       {
           .chain = authenticated_chain(),
           .state_domain = fixture.domain,
           .proof_limits = {},
       },
       finality,
   };

   auto wrong_scheme = valid;
   wrong_scheme.scheme = "forge.db.authenticated.range";
   BOOST_CHECK_THROW(static_cast<void>(verifier.verify_state_point(anchor, key, wrong_scheme)),
                     forge::chain::api::exceptions::invalid_state_proof);

   auto wrong_version = valid;
   ++wrong_version.version;
   BOOST_CHECK_THROW(static_cast<void>(verifier.verify_state_point(anchor, key, wrong_version)),
                     forge::chain::api::exceptions::invalid_state_proof);

   BOOST_REQUIRE(valid.payload.size() > 1U);
   auto tight_limits = authenticated::limits{};
   tight_limits.max_proof_bytes = valid.payload.size() - 1U;
   auto limited = forge::chain::api::authenticated_audit_verifier{
       {
           .chain = authenticated_chain(),
           .state_domain = fixture.domain,
           .proof_limits = tight_limits,
       },
       std::make_shared<recording_finality_verifier>(),
   };
   BOOST_CHECK_THROW(static_cast<void>(limited.verify_state_point(anchor, key, valid)),
                     forge::chain::api::exceptions::invalid_state_proof);

   auto wrong_root = anchor;
   ++wrong_root.state_root._hash[0];
   BOOST_CHECK_THROW(static_cast<void>(verifier.verify_state_point(wrong_root, key, valid)),
                     forge::chain::api::exceptions::invalid_state_proof);

   const auto context = forge::chain::protocol::response_context{.chain = authenticated_chain(), .anchor = anchor};
   BOOST_CHECK_NO_THROW(verifier.verify_context(context));
   auto wrong_context = context;
   ++wrong_context.chain._hash[0];
   BOOST_CHECK_THROW(verifier.verify_context(wrong_context), forge::chain::api::exceptions::wrong_chain);

   const auto finality_proof = forge::chain::protocol::proof_blob{.scheme = "test.finality"};
   BOOST_CHECK_NO_THROW(verifier.verify_finality(anchor, finality_proof));
   BOOST_TEST(finality->verify_calls == 1U);
   auto wrong_chain_anchor = anchor;
   ++wrong_chain_anchor.chain._hash[0];
   BOOST_CHECK_THROW(verifier.verify_finality(wrong_chain_anchor, finality_proof),
                     forge::chain::api::exceptions::wrong_chain);
   BOOST_TEST(finality->verify_calls == 1U);
}

BOOST_AUTO_TEST_CASE(authenticated_audit_verifier_verifies_ranked_range_and_change_tombstone_proofs) {
   const auto ranked = make_authenticated_ranked_fixture();
   auto ranked_verifier = forge::chain::api::authenticated_audit_verifier{
       {
           .chain = authenticated_chain(),
           .state_domain = ranked.domain,
           .proof_limits = {},
       },
       std::make_shared<recording_finality_verifier>(),
   };
   const auto request = forge::chain::api::authenticated_range_query{
       .range = {.lower = protocol_bytes("b")},
       .limit = 2U,
   };
   const auto proof = authenticated_ranked_proof(ranked, authenticated::range_request{
                                                             .lower = authenticated_bytes("b"),
                                                             .limit = 2U,
                                                             .include_values = true,
                                                         });
   const auto result = ranked_verifier.verify_state_range(
       authenticated_anchor(ranked.root), request, authenticated_proof_blob("forge.db.authenticated.range", proof));
   BOOST_REQUIRE_EQUAL(result.rows.size(), 2U);
   BOOST_CHECK(result.rows[0].key == protocol_bytes("b"));
   BOOST_CHECK(result.rows[0].value == protocol_bytes("two"));
   BOOST_CHECK(result.rows[1].key == protocol_bytes("c"));
   BOOST_CHECK(result.rows[1].value == protocol_bytes("three"));
   BOOST_REQUIRE(result.next_key.has_value());
   BOOST_CHECK(*result.next_key == protocol_bytes("d"));

   const auto reverse_request = forge::chain::api::authenticated_range_query{
       .range = {.lower = protocol_bytes("b")},
       .limit = 2U,
       .reverse = true,
   };
   const auto reverse_proof = authenticated_ranked_proof(ranked, authenticated::range_request{
                                                                     .lower = authenticated_bytes("b"),
                                                                     .limit = 2U,
                                                                     .include_values = true,
                                                                     .reverse = true,
                                                                 });
   const auto reverse_result =
       ranked_verifier.verify_state_range(authenticated_anchor(ranked.root), reverse_request,
                                          authenticated_proof_blob("forge.db.authenticated.range", reverse_proof));
   BOOST_REQUIRE_EQUAL(reverse_result.rows.size(), 2U);
   BOOST_CHECK(reverse_result.rows[0].key == protocol_bytes("d"));
   BOOST_CHECK(reverse_result.rows[0].value == protocol_bytes("four"));
   BOOST_CHECK(reverse_result.rows[1].key == protocol_bytes("c"));
   BOOST_CHECK(reverse_result.rows[1].value == protocol_bytes("three"));
   BOOST_REQUIRE(reverse_result.next_key.has_value());
   BOOST_CHECK(*reverse_result.next_key == protocol_bytes("c"));

   const auto changes = make_authenticated_changes_fixture();
   auto changes_verifier = forge::chain::api::authenticated_audit_verifier{
       {
           .chain = authenticated_chain(),
           .state_domain = changes.domain,
           .proof_limits = {},
       },
       std::make_shared<recording_finality_verifier>(),
   };
   const auto change_request = forge::chain::api::authenticated_range_query{.limit = 2U};
   const auto change_proof =
       authenticated_changes_proof(changes, authenticated::range_request{.limit = 2U, .include_values = true});
   const auto change_result =
       changes_verifier.verify_state_changes(authenticated_anchor(changes.root), change_request,
                                             authenticated_proof_blob("forge.db.authenticated.changes", change_proof));
   BOOST_REQUIRE_EQUAL(change_result.changes.size(), 2U);
   BOOST_CHECK(change_result.changes[0].key == protocol_bytes("alpha"));
   BOOST_TEST(!change_result.changes[0].value.has_value());
   BOOST_CHECK(change_result.changes[1].key == protocol_bytes("beta"));
   BOOST_REQUIRE(change_result.changes[1].value.has_value());
   BOOST_CHECK(*change_result.changes[1].value == protocol_bytes("updated"));
   BOOST_TEST(!change_result.next_key.has_value());
}

BOOST_AUTO_TEST_CASE(authenticated_audit_verifier_rejects_reordered_and_forged_ranked_inputs) {
   const auto fixture = make_authenticated_ranked_fixture();
   const auto request = forge::chain::api::authenticated_range_query{
       .range = {.lower = protocol_bytes("b"), .upper = protocol_bytes("d")},
       .limit = 2U,
   };
   const auto valid = authenticated_ranked_proof(fixture, authenticated::range_request{
                                                              .lower = authenticated_bytes("b"),
                                                              .upper = authenticated_bytes("d"),
                                                              .limit = 2U,
                                                              .include_values = true,
                                                          });
   auto verifier = forge::chain::api::authenticated_audit_verifier{
       {
           .chain = authenticated_chain(),
           .state_domain = fixture.domain,
           .proof_limits = {},
       },
       std::make_shared<recording_finality_verifier>(),
   };
   const auto reject = [&](const authenticated::range_proof& proof) {
      BOOST_CHECK_THROW(static_cast<void>(verifier.verify_state_range(
                            authenticated_anchor(fixture.root), request,
                            authenticated_proof_blob("forge.db.authenticated.range", proof))),
                        forge::chain::api::exceptions::invalid_state_proof);
   };

   auto reordered = valid;
   std::swap(reordered.nodes[2], reordered.nodes[3]);
   reject(reordered);

   auto forged_rank = valid;
   ++std::get<authenticated::range_inner>(forged_rank.nodes.front()).size;
   reject(forged_rank);

   auto forged_value = valid;
   std::get<authenticated::proof_leaf>(forged_value.nodes[2]).value = authenticated_bytes("forged");
   reject(forged_value);
}

BOOST_AUTO_TEST_CASE(authenticated_audit_verifier_verifies_transaction_merkle_proof_and_rejects_forgery) {
   auto first_id = forge::chain::protocol::transaction_id{};
   first_id._hash[0] = 0x51U;
   auto second_id = forge::chain::protocol::transaction_id{};
   second_id._hash[0] = 0x52U;

   auto first_receipt = forge::chain::protocol::transaction_receipt{};
   first_receipt.status = forge::chain::protocol::transaction_receipt::status::executed;
   first_receipt.cpu_usage_us = 7U;
   first_receipt.trx = first_id;
   auto second_receipt = forge::chain::protocol::transaction_receipt{};
   second_receipt.status = forge::chain::protocol::transaction_receipt::status::executed;
   second_receipt.cpu_usage_us = 8U;
   second_receipt.trx = second_id;

   const auto leaves = std::array{first_receipt.digest(), second_receipt.digest()};
   auto anchor = authenticated_anchor(make_authenticated_point_fixture().root);
   anchor.transaction_root = forge::chain::core::calculate_merkle_root(leaves);
   auto response = forge::chain::protocol::transaction_status_response{};
   response.id = first_id;
   response.state = forge::chain::protocol::transaction_lifecycle::finalized;
   response.block = anchor.block;
   response.block_num = anchor.block_num;
   response.receipt = first_receipt;
   const auto proof = forge::chain::protocol::transaction_inclusion_proof{
       .leaf = leaves[0],
       .index = 0U,
       .leaf_count = leaves.size(),
       .path = forge::chain::core::calculate_merkle_path(leaves, 0U),
   };
   auto verifier = forge::chain::api::authenticated_audit_verifier{
       {
           .chain = authenticated_chain(),
           .state_domain = "forge.test.chain-api.transaction-proof.v3",
           .proof_limits = {},
       },
       std::make_shared<recording_finality_verifier>(),
   };

   BOOST_CHECK_NO_THROW(verifier.verify_transaction(anchor, first_id, response, proof));

   auto traced = response;
   traced.trace = forge::chain::protocol::transaction_trace{.id = first_id};
   BOOST_CHECK_THROW(verifier.verify_transaction(anchor, first_id, traced, proof),
                     forge::chain::api::exceptions::invalid_transaction_proof);

   auto reordered = proof;
   reordered.index = 1U;
   BOOST_CHECK_THROW(verifier.verify_transaction(anchor, first_id, response, reordered),
                     forge::chain::api::exceptions::invalid_transaction_proof);

   auto forged_path = proof;
   BOOST_REQUIRE_EQUAL(forged_path.path.size(), 1U);
   ++forged_path.path.front().sibling._hash[0];
   BOOST_CHECK_THROW(verifier.verify_transaction(anchor, first_id, response, forged_path),
                     forge::chain::api::exceptions::invalid_transaction_proof);

   auto forged_receipt = response;
   ++forged_receipt.receipt->cpu_usage_us;
   BOOST_CHECK_THROW(verifier.verify_transaction(anchor, first_id, forged_receipt, proof),
                     forge::chain::api::exceptions::invalid_transaction_proof);
}

BOOST_AUTO_TEST_CASE(content_witness_roundtrips_and_returns_the_authenticated_value) {
   const auto value = forge::chain::protocol::bytes{0x10U, 0x20U, 0x30U};
   const auto other = forge::chain::protocol::bytes{0x40U};
   const auto expected = forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{value});
   const auto other_hash = forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{other});
   const auto audit = forge::chain::protocol::audit_bundle{
       .content =
           {
               {.hash = other_hash, .value = other},
               {.hash = expected, .value = value},
           },
   };

   const auto wire = forge::raw::pack(audit);
   const auto decoded =
       forge::raw::unpack_exact<forge::chain::protocol::audit_bundle>(std::span<const std::uint8_t>{wire});
   BOOST_CHECK(decoded == audit);

   const auto& authenticated_without_size = forge::chain::api::require_content_witness(decoded, expected);
   const auto& authenticated = forge::chain::api::require_content_witness(decoded, expected, value.size());
   BOOST_CHECK(authenticated == value);
   BOOST_CHECK(&authenticated_without_size == &decoded.content[1].value);
   BOOST_CHECK(&authenticated == &decoded.content[1].value);
}

BOOST_AUTO_TEST_CASE(content_witness_rejects_missing_duplicate_digest_and_size_mismatch) {
   const auto value = forge::chain::protocol::bytes{0x10U, 0x20U, 0x30U};
   const auto expected = forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{value});
   const auto valid = forge::chain::protocol::content_witness{.hash = expected, .value = value};

   BOOST_CHECK_THROW(
       static_cast<void>(forge::chain::api::require_content_witness(forge::chain::protocol::audit_bundle{}, expected)),
       forge::chain::api::exceptions::invalid_state_proof);

   const auto duplicate = forge::chain::protocol::audit_bundle{.content = {valid, valid}};
   BOOST_CHECK_THROW(static_cast<void>(forge::chain::api::require_content_witness(duplicate, expected)),
                     forge::chain::api::exceptions::invalid_state_proof);

   const auto malformed = forge::chain::protocol::audit_bundle{
       .content = {{.hash = expected, .value = {0xffU}}},
   };
   BOOST_CHECK_THROW(static_cast<void>(forge::chain::api::require_content_witness(malformed, expected)),
                     forge::chain::api::exceptions::invalid_state_proof);

   const auto wrong_size = forge::chain::protocol::audit_bundle{.content = {valid}};
   BOOST_CHECK_THROW(
       static_cast<void>(forge::chain::api::require_content_witness(wrong_size, expected, value.size() + 1U)),
       forge::chain::api::exceptions::invalid_state_proof);
}

BOOST_AUTO_TEST_CASE(cached_finality_verifier_reuses_an_exact_anchor) {
   const auto anchor = make_finality_anchor();
   const auto proof = forge::chain::protocol::proof_blob{.scheme = "test.finality"};
   auto delegate = std::make_shared<recording_finality_verifier>();
   auto verifier = forge::chain::api::cached_finality_verifier{delegate, 4U};

   verifier.verify(anchor, proof);
   verifier.verify(anchor, proof);

   BOOST_TEST(delegate->verify_calls == 1U);
}

BOOST_AUTO_TEST_CASE(cached_finality_verifier_single_flights_a_concurrent_exact_anchor) {
   const auto anchor = make_finality_anchor();
   const auto proof = forge::chain::protocol::proof_blob{.scheme = "test.finality"};
   auto delegate = std::make_shared<blocking_finality_verifier>();
   auto verifier = forge::chain::api::cached_finality_verifier{delegate, 4U};

   auto first = std::async(std::launch::async, [&] { verifier.verify(anchor, proof); });
   delegate->wait_until_entered();

   auto second_started = std::promise<void>{};
   auto second_started_future = second_started.get_future();
   auto second = std::async(std::launch::async, [&] {
      second_started.set_value();
      verifier.verify(anchor, proof);
   });
   second_started_future.wait();
   const auto second_status = second.wait_for(std::chrono::milliseconds{100});

   delegate->release();
   first.get();
   second.get();

   BOOST_CHECK(second_status == std::future_status::timeout);
   BOOST_TEST(delegate->verify_calls.load() == 1U);
}

BOOST_AUTO_TEST_CASE(cached_finality_verifier_rejects_a_conflicting_anchor_identity) {
   const auto anchor = make_finality_anchor();
   const auto proof = forge::chain::protocol::proof_blob{.scheme = "test.finality"};
   auto delegate = std::make_shared<recording_finality_verifier>();
   auto verifier = forge::chain::api::cached_finality_verifier{delegate, 4U};
   verifier.verify(anchor, proof);

   auto conflicting = anchor;
   ++conflicting.change_count;
   BOOST_CHECK_THROW(verifier.verify(conflicting, proof), forge::chain::api::exceptions::invalid_finality);
   BOOST_TEST(delegate->verify_calls == 1U);
}

BOOST_AUTO_TEST_CASE(cached_finality_verifier_does_not_cache_a_failed_verification) {
   const auto anchor = make_finality_anchor();
   const auto proof = forge::chain::protocol::proof_blob{.scheme = "test.finality"};
   auto delegate = std::make_shared<recording_finality_verifier>();
   delegate->failures_remaining = 1U;
   auto verifier = forge::chain::api::cached_finality_verifier{delegate, 4U};

   BOOST_CHECK_THROW(verifier.verify(anchor, proof), forge::chain::api::exceptions::invalid_finality);
   BOOST_CHECK_NO_THROW(verifier.verify(anchor, proof));
   BOOST_CHECK_NO_THROW(verifier.verify(anchor, proof));
   BOOST_TEST(delegate->verify_calls == 2U);
}

BOOST_AUTO_TEST_CASE(chain_audit_translates_standard_finality_delegate_failures) {
   const auto anchor = make_finality_anchor();
   const auto proof = forge::chain::protocol::proof_blob{.scheme = "test.finality"};
   auto delegate = std::make_shared<standard_throwing_finality_verifier>();
   auto cached = forge::chain::api::cached_finality_verifier{delegate, 4U};

   BOOST_CHECK_THROW(cached.verify(anchor, proof), forge::chain::api::exceptions::invalid_finality);
   BOOST_CHECK_THROW(cached.verify_ancestry(anchor, {}, proof), forge::chain::api::exceptions::invalid_finality);

   auto authenticated = forge::chain::api::authenticated_audit_verifier{
       {
           .chain = anchor.chain,
           .state_domain = "test.state",
       },
       std::move(delegate),
   };
   BOOST_CHECK_THROW(authenticated.verify_finality(anchor, proof), forge::chain::api::exceptions::invalid_finality);
   BOOST_CHECK_THROW(authenticated.verify_ancestry(anchor, {}, proof), forge::chain::api::exceptions::invalid_finality);
}

BOOST_AUTO_TEST_CASE(chain_audit_translates_nonstandard_finality_delegate_failures) {
   const auto anchor = make_finality_anchor();
   const auto proof = forge::chain::protocol::proof_blob{.scheme = "test.finality"};
   auto delegate = std::make_shared<recording_finality_verifier>();
   delegate->throw_nonstandard = true;
   auto cached = forge::chain::api::cached_finality_verifier{delegate, 4U};

   BOOST_CHECK_THROW(cached.verify(anchor, proof), forge::chain::api::exceptions::invalid_finality);
   BOOST_CHECK_THROW(cached.verify_ancestry(anchor, {}, proof), forge::chain::api::exceptions::invalid_finality);
}

BOOST_AUTO_TEST_CASE(cached_finality_verifier_delegates_ancestry_and_caches_the_finalized_anchor) {
   const auto finalized = make_finality_anchor();
   auto earlier = finalized;
   earlier.block._hash[0] = 1U;
   earlier.block_num = finalized.block_num - 1U;
   const auto intermediate = std::vector{earlier};
   const auto proof = forge::chain::protocol::proof_blob{.scheme = "test.ancestry", .version = 1U};
   auto delegate = std::make_shared<recording_finality_verifier>();
   auto verifier = forge::chain::api::cached_finality_verifier{delegate, 4U};

   verifier.verify_ancestry(finalized, std::span<const forge::chain::protocol::state_anchor>{intermediate}, proof);
   verifier.verify(finalized, proof);
   verifier.verify_ancestry(finalized, std::span<const forge::chain::protocol::state_anchor>{intermediate}, proof);

   BOOST_TEST(delegate->verify_calls == 0U);
   BOOST_TEST(delegate->ancestry_calls == 2U);
   BOOST_REQUIRE(delegate->ancestry_finalized);
   BOOST_TEST(delegate->ancestry_finalized->block == finalized.block);
   BOOST_REQUIRE_EQUAL(delegate->ancestry_intermediate.size(), 1U);
   BOOST_TEST(delegate->ancestry_intermediate.front().block == earlier.block);
   BOOST_REQUIRE(delegate->ancestry_proof);
   BOOST_TEST(delegate->ancestry_proof->scheme == proof.scheme);
}
