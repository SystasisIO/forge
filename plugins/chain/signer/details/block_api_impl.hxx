#pragma once

namespace forge::plugins::chain::signer {

class plugin::block_api_impl final : public forge::chain::api::block_signer {
 public:
   explicit block_api_impl(std::shared_ptr<impl> state);

   boost::asio::awaitable<std::vector<forge::chain::protocol::signature>>
   sign(forge::chain::protocol::block_sign_request request,
        forge::api::auth::authenticated_caller caller) override;

 private:
   std::shared_ptr<impl> state_;
};

} // namespace forge::plugins::chain::signer
