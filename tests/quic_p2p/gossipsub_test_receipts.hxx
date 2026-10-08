#pragma once

#include <cstdint>
#include <exception>
#include <mutex>
#include <vector>

// Include after forge.net.p2p.identity and forge.net.p2p.pubsub.
namespace forge::tests::p2p {

class gossipsub_test_receipts {
 public:
   void capture(const forge::net::p2p::pubsub::trace_event&);
   [[nodiscard]] bool subscribed(const forge::net::p2p::peer_id&, const forge::net::p2p::pubsub::topic&) const;
   [[nodiscard]] std::uint64_t written_generation(const forge::net::p2p::peer_id&,
                                                 const forge::net::p2p::pubsub::topic&) const;
   [[nodiscard]] bool reannounced_before_graft(const forge::net::p2p::peer_id&, const forge::net::p2p::pubsub::topic&,
                                              std::int64_t former_stream) const;
   [[nodiscard]] std::int64_t latest_subscription_stream(const forge::net::p2p::peer_id&,
                                                        const forge::net::p2p::pubsub::topic&) const;

 private:
   struct receipt {
      forge::net::p2p::pubsub::trace_kind kind;
      forge::net::p2p::peer_id peer;
      std::uint64_t session;
      std::int64_t stream;
      std::uint64_t generation;
      std::vector<forge::net::p2p::pubsub::subscription> subscriptions;
      std::vector<forge::net::p2p::pubsub::control::graft> grafts;
   };
   static bool has_subscription(const receipt&, const forge::net::p2p::pubsub::topic&);
   void check_failure() const;
   mutable std::mutex _mutex;
   std::vector<receipt> _events;
   std::exception_ptr _failure;
};

} // namespace forge::tests::p2p
