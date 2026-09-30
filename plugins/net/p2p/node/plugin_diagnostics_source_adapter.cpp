module;

#include <boost/asio/awaitable.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.api.transport.options;
import forge.asio.runtime;
import forge.asio.task;
import forge.net.p2p.diagnostics;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.scoring;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.types;
import forge.plugins.crypto.secrets.api;
import forge.plugins.db.store.api;

#include "details/plugin_impl.hxx"
#include "details/plugin_diagnostics_source_adapter.hxx"

namespace forge::plugins::net::p2p::node {

plugin::diagnostics_source_adapter::diagnostics_source_adapter(std::shared_ptr<plugin::impl> impl)
    : impl_{std::move(impl)} {}

forge::net::p2p::diagnostics::snapshot
plugin::diagnostics_source_adapter::snapshot(forge::net::p2p::diagnostics::options options) const {
   return impl_->require_node()->diagnostics(options);
}

} // namespace forge::plugins::net::p2p::node
