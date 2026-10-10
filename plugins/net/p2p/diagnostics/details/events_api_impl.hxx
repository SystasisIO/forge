#pragma once

namespace forge::plugins::net::p2p::diagnostics {

class plugin::events_api_impl final : public events_api {
 public:
   explicit events_api_impl(std::shared_ptr<plugin::impl> impl);

   [[nodiscard]] forge::net::p2p::host_event reachability_status() const override;
   [[nodiscard]] forge::net::p2p::host_event_subscription host_events() const override;

 private:
   std::shared_ptr<plugin::impl> impl_;
};

} // namespace forge::plugins::net::p2p::diagnostics
