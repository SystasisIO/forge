#pragma once

namespace forge::plugins::crypto::secrets {

class plugin::version_write_api_impl final : public version_write_api {
 public:
   explicit version_write_api_impl(std::shared_ptr<impl> state);
   boost::asio::awaitable<version_mutation_result> create_version(create_version_request value) override;
   boost::asio::awaitable<version_mutation_result> revoke_version(revoke_version_request value) override;

 private:
   std::shared_ptr<impl> state_;
};

} // namespace forge::plugins::crypto::secrets
