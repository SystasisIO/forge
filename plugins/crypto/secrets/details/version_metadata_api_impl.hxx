#pragma once

namespace forge::plugins::crypto::secrets {

class plugin::version_metadata_api_impl final : public version_metadata_api {
 public:
   explicit version_metadata_api_impl(std::shared_ptr<impl> state);
   boost::asio::awaitable<version_metadata> describe_version(version_query value) override;
   boost::asio::awaitable<version_page> list_versions(version_list_request value) override;
   boost::asio::awaitable<version_snapshot> status(query value) override;

 private:
   std::shared_ptr<impl> state_;
};

} // namespace forge::plugins::crypto::secrets
