#pragma once

namespace forge::plugins::net::p2p::diagnostics {

class plugin::api_impl final : public api {
 public:
   explicit api_impl(std::shared_ptr<plugin::impl> impl);

   forge::net::p2p::diagnostics::snapshot snapshot() const override;
   forge::net::p2p::diagnostics::snapshot snapshot(forge::net::p2p::diagnostics::options options) const override;
   forge::net::p2p::diagnostics::network_state network() const override;
   forge::net::p2p::resource_manager::snapshot resources() const override;
   forge::net::p2p::pubsub::snapshot pubsub() const override;
   std::vector<forge::net::p2p::diagnostics::peer> peers(filter value) const override;
   forge::net::p2p::diagnostics::peer peer(forge::net::p2p::peer_id value) const override;

 private:
   std::shared_ptr<plugin::impl> impl_;
};

} // namespace forge::plugins::net::p2p::diagnostics
