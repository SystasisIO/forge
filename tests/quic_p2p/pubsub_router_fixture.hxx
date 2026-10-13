#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// C linkage keeps this test-only bridge independent of the caller's named module.
extern "C" void forge_test_pubsub_fail_next_stream_reserve_prepare() noexcept;

namespace forge::tests::p2p {

struct pubsub_router_receipt {
   forge::net::p2p::pubsub::trace_kind kind;
   forge::net::p2p::peer_id peer;
   forge::net::p2p::protocol_id protocol;
   std::uint64_t session = 0;
   std::int64_t stream = -1;
   std::uint64_t generation = 0;
   std::chrono::steady_clock::time_point observed{};
   std::optional<forge::net::p2p::pubsub::validation_result> result;
   std::vector<std::uint8_t> id;
   std::vector<std::uint8_t> data;
   std::vector<std::uint8_t> frame;
};

class pubsub_router_fixture {
 public:
   pubsub_router_fixture();
   ~pubsub_router_fixture();
   forge::net::p2p::node& add(std::string name, forge::net::p2p::pubsub::options options = {},
                             forge::net::p2p::resource_manager::limits resources = {},
                             std::string_view listen_address = "/ip4/127.0.0.1/tcp/0",
                             forge::net::p2p::node::stream_security security =
                                 forge::net::p2p::node::stream_security::tls_and_noise);
   void subscribe(forge::net::p2p::node& owner, forge::net::p2p::pubsub::handler handler = {});
   void connect(forge::net::p2p::node& source, forge::net::p2p::node& target);
   [[nodiscard]] forge::net::p2p::stream open(forge::net::p2p::node& source, forge::net::p2p::node& target);
   void send(forge::net::p2p::stream& stream, const forge::net::p2p::pubsub::rpc& rpc);
   [[nodiscard]] forge::net::p2p::pubsub::message publish(forge::net::p2p::node& source, std::string data);
   [[nodiscard]] std::vector<pubsub_router_receipt> receipts(const forge::net::p2p::node& owner) const;
   [[nodiscard]] bool wait(const std::function<bool()>& ready, std::chrono::milliseconds timeout = std::chrono::seconds{5});
   void stop();
   forge::asio::runtime runtime;
   const forge::net::p2p::pubsub::topic topic{"forge.pubsub.router"};

 private:
   std::vector<std::unique_ptr<forge::net::p2p::node>> _nodes;
   mutable std::mutex _mutex;
   std::condition_variable _changed;
   std::map<forge::net::p2p::peer_id, std::vector<pubsub_router_receipt>> _receipts;
};

} // namespace forge::tests::p2p
