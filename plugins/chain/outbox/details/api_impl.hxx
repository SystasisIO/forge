#pragma once

namespace forge::plugins::chain::outbox {

class plugin::api_impl final : public forge::api::chain::runtime, public setup {
 public:
   explicit api_impl(std::shared_ptr<impl> state);

   void configure(std::string store, forge::chain::api::verified_client_options verification,
                  decltype(forge::api::chain::outbox::options::prepare_first_send) prepare_first_send,
                  forge::api::chain::outbox::admission admit) override;
   void configure_read(forge::api::core::api_ref contract, forge::api::chain::destination target,
                       forge::chain::api::verified_client_options verification, std::size_t max_calls,
                       std::chrono::milliseconds timeout) override;

   boost::asio::awaitable<forge::chain::protocol::block_id> finalized(forge::api::chain::destination target) override;

   forge::api::chain::destination read_target(const forge::api::core::api_ref& contract) const override;
   bool same_read_profile(const std::optional<forge::api::core::api_ref>& requested,
                          const forge::api::core::api_ref& contract) const override;
   boost::asio::awaitable<void> read(forge::api::chain::destination target,
                                    std::optional<forge::api::chain::block_reference> block, read_handler handler,
                                    std::optional<forge::api::core::api_ref> read_contract = {}) override;
   boost::asio::awaitable<forge::chain::protocol::block_response>
   get_block(forge::api::chain::destination target, forge::chain::protocol::block_id block) override;

   boost::asio::awaitable<forge::chain::transaction::builder> begin(forge::api::chain::destination target,
      std::optional<forge::api::chain::action_options> options) override;
   boost::asio::awaitable<forge::chain::protocol::transaction_id>
   submit(forge::api::chain::destination target, forge::api::core::api_ref contract, std::string method,
          forge::chain::transaction::unsigned_transaction transaction) override;
   boost::asio::awaitable<forge::chain::protocol::transaction_status_response>
   status(forge::api::chain::destination target, forge::chain::protocol::transaction_id id) override;

 private:
   const forge::chain::api::verified_client_options& verification_for(
       const forge::api::chain::destination& target, const impl::read_profile* profile) const;
   std::shared_ptr<impl> _state;
};

} // namespace forge::plugins::chain::outbox
