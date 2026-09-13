#pragma once

#include "node_impl.hxx"

namespace forge::net::p2p {

struct node::impl::autonat_operation {
   autonat_operation();
   ~autonat_operation();

   void cancel() noexcept;
   void check() const;

   std::shared_ptr<impl> owner;
   std::shared_ptr<session_state> caller;
   autonat_protocol protocol = autonat_protocol::v1;
   forge::net::p2p::stream request;
   forge::net::p2p::stream dial_back;
   std::shared_ptr<detail::resource_stream> dial_back_resource;
   detail::direct_attempt attempt;
   std::shared_ptr<cancellation_latch> cancellation;
   std::shared_ptr<detail::worker_stop_bridge> stop;
   std::unique_ptr<boost::asio::steady_timer> delay;
   std::optional<resource_manager::memory_reservation> memory;
   std::chrono::steady_clock::time_point deadline;
   std::vector<endpoint> local_endpoints;
   std::optional<endpoint> remote_endpoint;
   std::optional<endpoint> inbound_endpoint;
   peer_id peer;
   std::exception_ptr failure;
   bool admitted = false;
   bool service = false;
   bool quota_rejected = false;
   bool completed = false;
};

} // namespace forge::net::p2p
