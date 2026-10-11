#pragma once

namespace forge::plugins::net::p2p::node {

class plugin::host_event_source_adapter final : public host_event_source {
 public:
   explicit host_event_source_adapter(std::shared_ptr<plugin::impl> impl);

   [[nodiscard]] forge::net::p2p::host_event reachability_status() const override;
   [[nodiscard]] forge::net::p2p::host_event_subscription host_events() const override;

 private:
   std::shared_ptr<plugin::impl> impl_;
};

} // namespace forge::plugins::net::p2p::node
