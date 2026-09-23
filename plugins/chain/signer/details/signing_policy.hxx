#pragma once

namespace forge::plugins::chain::signer {

class signing_policy final {
 public:
   struct transaction_selection {
      std::string profile;
      std::shared_ptr<forge::crypto::signer::provider> provider;
      std::shared_ptr<semantic_authorization_provider> semantic_authorization;
      forge::chain::transaction::signing_key key;
      std::uint64_t max_packed_bytes = 0;
   };

   struct finality_selection {
      std::shared_ptr<forge::crypto::bls::signer::provider> provider;
      forge::crypto::bls::public_key expected_key;
   };

   struct block_key_selection {
      std::shared_ptr<forge::crypto::signer::provider> provider;
      forge::chain::transaction::signing_key key;
   };

   struct block_selection {
      std::string profile;
      forge::chain::protocol::account_name producer;
      std::vector<block_key_selection> keys;
      std::uint64_t max_header_bytes = 0;
   };

   signing_policy(const plugin_options& options, const config& settings);
   ~signing_policy();

   signing_policy(const signing_policy&) = delete;
   signing_policy& operator=(const signing_policy&) = delete;

   [[nodiscard]] transaction_selection
   select_transaction(const forge::chain::transaction::unsigned_transaction& transaction,
                      const forge::api::auth::authenticated_caller& caller,
                      forge::chain::protocol::time_point_sec now) const;
   [[nodiscard]] block_selection select_block(const forge::chain::protocol::block_sign_request& request,
                                              const forge::api::auth::authenticated_caller& caller) const;
   [[nodiscard]] finality_selection select_finality() const;

 private:
   struct compiled_caller;
   struct compiled_action;
   struct compiled_context_free_action;
   struct compiled_profile;
   struct compiled_block_profile;

   [[nodiscard]] bool matches(const compiled_profile& profile,
                              const forge::chain::transaction::unsigned_transaction& transaction,
                              const forge::api::auth::authenticated_caller& caller,
                              forge::chain::protocol::time_point_sec now) const;

   std::vector<compiled_profile> profiles_;
   std::vector<compiled_block_profile> block_profiles_;
   std::shared_ptr<forge::crypto::bls::signer::provider> finality_provider_;
   forge::crypto::bls::public_key finality_key_;
};

} // namespace forge::plugins::chain::signer
