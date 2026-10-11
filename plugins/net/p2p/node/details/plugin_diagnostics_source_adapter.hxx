#pragma once

namespace forge::plugins::net::p2p::node {

class plugin::diagnostics_source_adapter final : public diagnostics_source {
 public:
   explicit diagnostics_source_adapter(std::shared_ptr<plugin::impl> impl);

   forge::net::p2p::diagnostics::snapshot snapshot(forge::net::p2p::diagnostics::options options) const override;
   forge::net::p2p::dht::routing_status routing_status(const forge::net::p2p::protocol_id& profile) const override;
   forge::net::p2p::pubsub::score_snapshot pubsub_scores() const override;

 private:
   std::shared_ptr<plugin::impl> impl_;
};

} // namespace forge::plugins::net::p2p::node
