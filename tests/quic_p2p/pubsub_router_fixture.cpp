module;

#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include "libp2p_identity_fixture.hxx"

module forge.net.p2p.node;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;

#include "pubsub_router_fixture.hxx"

namespace forge::tests::p2p {

namespace p2p = forge::net::p2p;
using namespace std::chrono_literals;

pubsub_router_fixture::pubsub_router_fixture() : runtime(forge::asio::runtime_options{.worker_threads = 4}) {}

pubsub_router_fixture::~pubsub_router_fixture() {
   for (const auto& owner : _nodes) {
      try { forge::asio::blocking::run(runtime, owner->async_stop()); } catch (...) {}
   }
}

p2p::node& pubsub_router_fixture::add(std::string name, p2p::pubsub::options pubsub,
                                     p2p::resource_manager::limits resources, std::string_view listen_address,
                                     p2p::node::stream_security security) {
   const auto identity = make_identity_fixture(name);
   auto options = p2p::node::options{.certificate_pem = identity.certificate_pem,
      .private_key_pem = identity.private_key_pem, .capabilities = {.bits = p2p::capabilities::pubsub}};
   options.stream_security = security;
   if (security == p2p::node::stream_security::tls) {
      options.relay_policy.client_enabled = false;
   }
   options.peer_state.persistence = p2p::peer_store::make_memory_persistence();
   options.dht_profiles.clear();
   options.limits.resources = std::move(resources);
   auto native_owner = std::make_shared<p2p::node*>(nullptr);
   const auto tracer = pubsub.tracer;
   pubsub.tracer = [this, native_owner, tracer](const auto& event) {
      auto receipt = pubsub_router_receipt{.kind = event.kind, .peer = event.peer, .protocol = event.protocol,
         .session = event.session_id, .stream = event.stream_id, .generation = event.generation,
         .observed = std::chrono::steady_clock::now(),
         .result = event.result, .id = {event.message_id.begin(), event.message_id.end()},
         .data = {event.data.begin(), event.data.end()}, .frame = {event.framed_rpc.begin(), event.framed_rpc.end()}};
      {
         const auto lock = std::scoped_lock{_mutex};
         _receipts[(*native_owner)->local_peer()].push_back(std::move(receipt));
      }
      _changed.notify_all();
      if (tracer) { tracer(event); }
   };
   options.limits.pubsub = std::move(pubsub);
   auto owner = std::make_unique<p2p::node>(runtime, std::move(options));
   *native_owner = owner.get();
   auto& result = *owner;
   _nodes.push_back(std::move(owner));
   forge::asio::blocking::run(runtime, result.async_listen(p2p::parse_endpoint(listen_address)));
   return result;
}

void pubsub_router_fixture::subscribe(p2p::node& owner, p2p::pubsub::handler handler) {
   if (!handler) {
      handler = [](auto) -> boost::asio::awaitable<p2p::pubsub::validation_result> {
         co_return p2p::pubsub::validation_result::accept;
      };
   }
   static_cast<void>(forge::asio::blocking::run(runtime, owner.async_subscribe(topic, std::move(handler))));
}

void pubsub_router_fixture::connect(p2p::node& source, p2p::node& target) {
   static_cast<void>(forge::asio::blocking::run(runtime, source.async_connect(target.local_endpoints().front(),
      p2p::node::connect_options{.expected_peer = target.local_peer(), .allow_relay = false, .timeout = 3s,
                                .allow_hole_punch = false})));
}

p2p::stream pubsub_router_fixture::open(p2p::node& source, p2p::node& target) {
   return forge::asio::blocking::run(runtime, source.async_open_protocol_stream(target.local_peer(), p2p::builtins::meshsub_v11,
      p2p::node::open_options{.allow_relay = false, .timeout = 3s, .allow_hole_punch = false}));
}

void pubsub_router_fixture::send(p2p::stream& stream, const p2p::pubsub::rpc& rpc) {
   const auto frame = p2p::pubsub::codec::encode(rpc);
   forge::asio::blocking::run(runtime, stream.async_write(frame));
}

p2p::pubsub::message pubsub_router_fixture::publish(p2p::node& source, std::string data) {
   return forge::asio::blocking::run(runtime, source.async_publish(topic, {data.begin(), data.end()}));
}

std::vector<pubsub_router_receipt> pubsub_router_fixture::receipts(const p2p::node& owner) const {
   const auto lock = std::scoped_lock{_mutex};
   const auto it = _receipts.find(owner.local_peer());
   return it == _receipts.end() ? std::vector<pubsub_router_receipt>{} : it->second;
}

bool pubsub_router_fixture::wait(const std::function<bool()>& ready, std::chrono::milliseconds timeout) {
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   while (!ready()) {
      const auto observed = std::chrono::steady_clock::now();
      if (observed >= deadline) { return ready(); }
      // A bounded predicate wait also observes native score/mesh changes without a trace event.
      auto lock = std::unique_lock{_mutex};
      _changed.wait_until(lock, std::min(deadline, observed + 2ms));
   }
   return true;
}

void pubsub_router_fixture::stop() {
   auto first = std::exception_ptr{};
   for (const auto& owner : _nodes) {
      try { forge::asio::blocking::run(runtime, owner->async_stop()); }
      catch (...) { if (!first) { first = std::current_exception(); } }
   }
   if (first) { std::rethrow_exception(first); }
}

} // namespace forge::tests::p2p
