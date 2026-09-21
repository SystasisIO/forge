module;

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

export module forge.chain.api.verified_client;

export import forge.chain.api.limits;
export import forge.chain.api.raw_client;

export namespace forge::chain::api {

[[nodiscard]] const protocol::bytes& require_content_witness(const protocol::audit_bundle& audit,
                                                             protocol::digest expected,
                                                             std::optional<std::uint64_t> expected_size = std::nullopt);

struct authenticated_key_range {
   std::optional<protocol::bytes> lower;
   std::optional<protocol::bytes> upper;

   bool operator==(const authenticated_key_range&) const = default;
};

struct authenticated_range_query {
   authenticated_key_range range;
   std::uint32_t limit = 256;
   bool reverse = false;

   bool operator==(const authenticated_range_query&) const = default;
};

struct authenticated_state_item {
   protocol::bytes key;
   protocol::bytes value;

   bool operator==(const authenticated_state_item&) const = default;
};

struct authenticated_state_range {
   std::vector<authenticated_state_item> rows;
   std::optional<protocol::bytes> next_key;

   bool operator==(const authenticated_state_range&) const = default;
};

struct authenticated_state_change {
   protocol::bytes key;
   std::optional<protocol::bytes> value;

   bool operator==(const authenticated_state_change&) const = default;
};

struct authenticated_change_range {
   std::vector<authenticated_state_change> changes;
   std::optional<protocol::bytes> next_key;

   bool operator==(const authenticated_change_range&) const = default;
};

class audit_verifier {
 public:
   virtual ~audit_verifier() = default;

   [[nodiscard]] virtual std::optional<protocol::block_id> preferred_finality_anchor() const;
   [[nodiscard]] virtual std::optional<protocol::block_id>
   finality_anchor_at_or_before(protocol::block_num target) const;

   virtual void verify_context(const protocol::response_context& context) = 0;
   virtual void verify_finality(const protocol::state_anchor& anchor, const protocol::proof_blob& proof) = 0;
   virtual std::optional<protocol::bytes> verify_state_point(const protocol::state_anchor& anchor,
                                                             const protocol::bytes& key,
                                                             const protocol::proof_blob& proof) = 0;
   virtual authenticated_state_range verify_state_range(const protocol::state_anchor& anchor,
                                                        const authenticated_range_query& request,
                                                        const protocol::proof_blob& proof) = 0;
   virtual authenticated_change_range verify_state_changes(const protocol::state_anchor& anchor,
                                                           const authenticated_range_query& request,
                                                           const protocol::proof_blob& proof) = 0;
   virtual void verify_ancestry(const protocol::state_anchor& finalized,
                                std::span<const protocol::state_anchor> intermediate,
                                const protocol::proof_blob& proof) = 0;
   virtual void verify_transaction(const protocol::state_anchor& anchor,
                                   const forge::chain::protocol::transaction_id& expected,
                                   const protocol::transaction_status_response& response,
                                   const protocol::transaction_inclusion_proof& proof) = 0;
};

class projection_verifier {
 public:
   virtual ~projection_verifier();

   virtual void verify(const protocol::block_request& request, const protocol::block_state_response& response,
                       const protocol::audit_bundle& audit, audit_verifier& verifier);
   virtual void verify(const protocol::block_range_request& request, const protocol::block_range_response& response,
                       const protocol::audit_bundle& audit, audit_verifier& verifier);
   virtual void verify(const protocol::protocol_features_request& request,
                       const protocol::protocol_features_response& response, const protocol::audit_bundle& audit,
                       audit_verifier& verifier);
   virtual void verify(const protocol::anchored_request& request,
                       const protocol::consensus_parameters_response& response, const protocol::audit_bundle& audit,
                       audit_verifier& verifier);
   virtual void verify(const protocol::producers_request& request, const protocol::producers_response& response,
                       const protocol::audit_bundle& audit, audit_verifier& verifier);
   virtual void verify(const protocol::producer_rewards_request& request,
                       const protocol::producer_rewards_response& response, const protocol::audit_bundle& audit,
                       audit_verifier& verifier);
   virtual void verify(const protocol::anchored_request& request, const protocol::producer_schedule_response& response,
                       const protocol::audit_bundle& audit, audit_verifier& verifier);
   virtual void verify(const protocol::anchored_request& request, const protocol::finalizer_info_response& response,
                       const protocol::audit_bundle& audit, audit_verifier& verifier);
   virtual void verify(const protocol::account_request& request, const protocol::account_response& response,
                       const protocol::audit_bundle& audit, audit_verifier& verifier);
   virtual void verify(const protocol::account_changes_request& request,
                       const protocol::account_changes_response& response, const protocol::audit_bundle& audit,
                       audit_verifier& verifier);
   virtual void verify(const protocol::code_request& request, const protocol::code_response& response,
                       const protocol::audit_bundle& audit, audit_verifier& verifier);
   virtual void verify(const protocol::permission_links_request& request,
                       const protocol::permission_links_response& response, const protocol::audit_bundle& audit,
                       audit_verifier& verifier);
   virtual void verify(const protocol::table_rows_request& request, const protocol::table_rows_response& response,
                       const protocol::audit_bundle& audit, audit_verifier& verifier);
   virtual void verify(const protocol::table_changes_request& request, const protocol::table_changes_response& response,
                       const protocol::audit_bundle& audit, audit_verifier& verifier);
   virtual void verify(const protocol::table_scope_request& request, const protocol::table_scope_response& response,
                       const protocol::audit_bundle& audit, audit_verifier& verifier);
   virtual void verify(const protocol::currency_balance_request& request,
                       const protocol::currency_balance_response& response, const protocol::audit_bundle& audit,
                       audit_verifier& verifier);
   virtual void verify(const protocol::currency_stats_request& request,
                       const protocol::currency_stats_response& response, const protocol::audit_bundle& audit,
                       audit_verifier& verifier);
   virtual void verify(const protocol::scheduled_request& request, const protocol::scheduled_response& response,
                       const protocol::audit_bundle& audit, audit_verifier& verifier);
   virtual void verify(const protocol::authorizers_request& request, const protocol::authorizers_response& response,
                       const protocol::audit_bundle& audit, audit_verifier& verifier);
};

class verified_client {
 public:
   verified_client(raw_client client, std::shared_ptr<audit_verifier> verifier,
                   std::shared_ptr<projection_verifier> projections = {}, protocol::service_limits limits = {});

   boost::asio::awaitable<protocol::info_response> get_info();
   boost::asio::awaitable<protocol::info_response> get_info(protocol::anchored_request request);

   boost::asio::awaitable<protocol::block_response> get_block(protocol::block_request request);
   boost::asio::awaitable<protocol::block_header_response> get_header(protocol::block_request request);
   boost::asio::awaitable<protocol::block_state_response> get_block_state(protocol::block_request request);
   boost::asio::awaitable<protocol::block_range_response> get_canonical_range(protocol::block_range_request request);
   boost::asio::awaitable<protocol::protocol_features_response>
   get_activated_protocol_features(protocol::protocol_features_request request);
   boost::asio::awaitable<protocol::consensus_parameters_response>
   get_consensus_parameters(protocol::anchored_request request);
   boost::asio::awaitable<protocol::producers_response> get_producers(protocol::producers_request request);
   boost::asio::awaitable<protocol::producer_rewards_response>
   get_producer_rewards(protocol::producer_rewards_request request);
   boost::asio::awaitable<protocol::producer_schedule_response>
   get_producer_schedule(protocol::anchored_request request);
   boost::asio::awaitable<protocol::finalizer_info_response> get_finalizer_info(protocol::anchored_request request);

   boost::asio::awaitable<protocol::account_response> get_account(protocol::account_request request);
   boost::asio::awaitable<protocol::account_changes_response>
   get_account_changes(protocol::account_changes_request request);
   boost::asio::awaitable<protocol::code_response> get_code(protocol::code_request request);
   boost::asio::awaitable<protocol::permission_links_response>
   get_permission_links(protocol::permission_links_request request);
   boost::asio::awaitable<protocol::table_rows_response> get_table_rows(protocol::table_rows_request request);
   boost::asio::awaitable<protocol::table_changes_response> get_table_changes(protocol::table_changes_request request);
   boost::asio::awaitable<protocol::table_scope_response> get_table_scope(protocol::table_scope_request request);
   boost::asio::awaitable<protocol::currency_balance_response>
   get_currency_balance(protocol::currency_balance_request request);
   boost::asio::awaitable<protocol::currency_stats_response>
   get_currency_stats(protocol::currency_stats_request request);
   boost::asio::awaitable<protocol::scheduled_response> get_scheduled_transactions(protocol::scheduled_request request);
   boost::asio::awaitable<protocol::authorizers_response>
   get_accounts_by_authorizers(protocol::authorizers_request request);

   boost::asio::awaitable<protocol::transaction_status_response>
   get_transaction_status(protocol::transaction_status_request request);
   boost::asio::awaitable<protocol::transaction_status_response>
   await_transaction(protocol::transaction_await_request request);
   boost::asio::awaitable<std::vector<protocol::public_key>>
   get_required_keys(protocol::transaction_required_keys_request request);
   boost::asio::awaitable<protocol::transaction_read_only_response>
   compute_transaction(protocol::transaction_read_only_request request);
   boost::asio::awaitable<protocol::transaction_read_only_response>
   send_read_only_transaction(protocol::transaction_read_only_request request);

 private:
   const protocol::audit_bundle& verify_envelope(const protocol::audited_response& response);
   void verify_requested_anchor(const std::optional<protocol::block_id>& requested,
                                const protocol::audited_response& response);
   void verify_change_batches(std::uint32_t from_block, std::uint32_t to_block, bool has_request_cursor,
                              const protocol::audited_response& response,
                              std::span<const protocol::state_anchor> anchors, bool has_next,
                              const protocol::audit_bundle& audit);
   void verify_transaction_status(const forge::chain::protocol::transaction_id& expected,
                                  const protocol::transaction_status_response& response);

   raw_client client_;
   std::shared_ptr<audit_verifier> verifier_;
   std::shared_ptr<projection_verifier> projections_;
   protocol::service_limits limits_;
};

} // namespace forge::chain::api
