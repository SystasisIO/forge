#pragma once

namespace forge::plugins::net::p2p::resolver {

class plugin::managed_api_impl final : public managed_api {
 public:
   explicit managed_api_impl(std::shared_ptr<plugin::impl> impl);

 private:
   boost::asio::awaitable<std::shared_ptr<forge::api::core::remote_invoker>>
   open_managed_remote(std::vector<forge::net::p2p::peer_id> ordered_peers, forge::api::core::api_ref requested,
                       forge::api::core::descriptor descriptor, managed_remote_options options) override;

   std::shared_ptr<plugin::impl> impl_;
};

} // namespace forge::plugins::net::p2p::resolver
