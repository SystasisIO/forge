#pragma once

namespace forge::plugins::net::p2p::resolver {

class plugin::api_impl final : public api {
 public:
   explicit api_impl(std::shared_ptr<plugin::impl> impl);

   [[nodiscard]] forge::api::p2p::publication
   publish_api(forge::api::core::binding_plan plan, forge::net::p2p::protocol_id protocol,
               publish_options options) override;
   [[nodiscard]] std::vector<entry> local_apis() const override;
   boost::asio::awaitable<std::vector<entry>> peer_apis(forge::net::p2p::peer_id peer,
                                                        resolve_options options) override;
   boost::asio::awaitable<resolution> resolve(forge::net::p2p::peer_id peer, forge::api::core::api_ref api,
                                              resolve_options options) override;

 private:
   boost::asio::awaitable<resolved_connection>
   open_resolved_connection(forge::net::p2p::peer_id peer, forge::api::core::api_ref api,
                            forge::api::core::descriptor descriptor, resolve_options options) override;

   std::shared_ptr<plugin::impl> impl_;
};

} // namespace forge::plugins::net::p2p::resolver
