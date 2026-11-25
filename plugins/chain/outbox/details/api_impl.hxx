#pragma once

namespace forge::plugins::chain::outbox {

class plugin::api_impl final : public forge::api::chain::runtime {
 public:
   explicit api_impl(std::shared_ptr<impl> state);

   boost::asio::awaitable<forge::chain::transaction::builder> begin(forge::api::chain::destination target) override;
   boost::asio::awaitable<forge::chain::protocol::transaction_id>
   submit(forge::api::chain::destination target, forge::api::core::api_ref contract, std::string method,
          forge::chain::transaction::unsigned_transaction transaction) override;
   boost::asio::awaitable<forge::chain::protocol::transaction_status_response>
   status(forge::api::chain::destination target, forge::chain::protocol::transaction_id id) override;

 private:
   std::shared_ptr<impl> _state;
};

} // namespace forge::plugins::chain::outbox
