#pragma once

namespace forge::plugins::net::p2p::node {

class plugin::diagnostics_api_impl final : public diagnostics_api {
 public:
   explicit diagnostics_api_impl(std::shared_ptr<plugin::impl> impl);

   forge::net::p2p::diagnostics::snapshot snapshot() const override;
   forge::net::p2p::diagnostics::snapshot snapshot(forge::net::p2p::diagnostics::options options) const override;
   forge::net::p2p::diagnostics::network_state network() const override;
   forge::net::p2p::resource_manager::snapshot resources() const override;
   forge::net::p2p::pubsub::snapshot pubsub() const override;
   forge::net::p2p::dht::routing_status routing_status(const forge::net::p2p::protocol_id& profile) const override;
   forge::net::p2p::pubsub::score_snapshot pubsub_scores() const override;
   std::vector<forge::net::p2p::diagnostics::peer> peers(peer_filter value) const override;
   forge::net::p2p::diagnostics::peer peer(forge::net::p2p::peer_id value) const override;

 private:
   [[nodiscard]] std::shared_ptr<forge::net::p2p::node> require_node() const;
   [[nodiscard]] forge::net::p2p::diagnostics::snapshot
   take_snapshot(std::optional<forge::net::p2p::diagnostics::options> options = {}) const;
   std::shared_ptr<plugin::impl> impl_;
};

} // namespace forge::plugins::net::p2p::node
