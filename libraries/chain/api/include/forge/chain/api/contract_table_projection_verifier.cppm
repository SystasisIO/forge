module;

#include <memory>

export module forge.chain.api.contract_table_projection_verifier;

export import forge.chain.api.verified_client;
export import forge.chain.protocol.contract_commitment;

export namespace forge::chain::api {

class contract_table_projection_verifier final : public projection_verifier {
 public:
   void verify(const protocol::table_rows_request& request, const protocol::table_rows_response& response,
               const protocol::audit_bundle& audit, audit_verifier& verifier) override;
   void verify(const protocol::producer_rewards_request& request, const protocol::producer_rewards_response& response,
               const protocol::audit_bundle& audit, audit_verifier& verifier) override;
   void verify(const protocol::table_changes_request& request, const protocol::table_changes_response& response,
               const protocol::audit_bundle& audit, audit_verifier& verifier) override;
};

[[nodiscard]] std::shared_ptr<projection_verifier> make_contract_table_projection_verifier();

} // namespace forge::chain::api
