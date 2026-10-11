module;

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.asio.gate;
import forge.asio.notification;
import forge.asio.runtime;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.topology;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.pubsub_api;
import forge.plugins.net.p2p.node.types;

#include "../../../plugins/net/p2p/node/details/pubsub_service.hxx"
#include "../../../plugins/net/p2p/node/details/pubsub_policy.hxx"
#include "details/pubsub_service_fixture.hxx"

namespace forge::plugins::net::p2p::node::test {

pubsub_service_fixture::pubsub_service_fixture(forge::asio::runtime& runtime,
    detail::pubsub_operations operations, pubsub_config settings) {
   auto options = forge::net::p2p::node::options{};
   options.allow_insecure_test_mode = true;
   options.explicit_peer_id = forge::net::p2p::make_peer_id({
       .type = forge::net::p2p::public_key::type::ed25519, .data = std::vector<std::uint8_t>(32, 74)});
   options.limits.topology.operating_mode = forge::net::p2p::topology::mode::static_only;
   options.capabilities.bits = forge::net::p2p::capabilities::pubsub;
   options.limits.pubsub = detail::pubsub_options_for(settings);
   detail::validate_pubsub_config(settings);
   native_ = std::make_shared<forge::net::p2p::node>(runtime, std::move(options));
   owner_ = std::make_shared<detail::pubsub_service>(std::move(operations));
   owner_->initialize(std::move(settings), native_);
}

std::shared_ptr<pubsub_api> pubsub_service_fixture::interface() { return owner_->interface(); }
const std::shared_ptr<forge::net::p2p::node>& pubsub_service_fixture::native() const noexcept { return native_; }
void pubsub_service_fixture::request_stop() noexcept { owner_->request_stop(); }
boost::asio::awaitable<void> pubsub_service_fixture::shutdown() {
   return detail::pubsub_service::shutdown_owned(owner_);
}

}
