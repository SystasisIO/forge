module;

#include <memory>
#include <mutex>
#include <optional>
#include <utility>

module forge.plugins.net.p2p.diagnostics.plugin;

import forge.net.p2p.diagnostics;
import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.host_event_source;
import forge.plugins.net.p2p.diagnostics.events_api;
import forge.plugins.net.p2p.diagnostics.types;

#include "details/plugin_impl.hxx"
#include "details/events_api_impl.hxx"

namespace forge::plugins::net::p2p::diagnostics {

plugin::events_api_impl::events_api_impl(std::shared_ptr<plugin::impl> impl) : impl_{std::move(impl)} {}

forge::net::p2p::host_event plugin::events_api_impl::reachability_status() const {
   const auto current = impl_->require_events_source();
   return current->reachability_status();
}

forge::net::p2p::host_event_subscription plugin::events_api_impl::host_events() const {
   const auto current = impl_->require_events_source();
   return current->host_events();
}

} // namespace forge::plugins::net::p2p::diagnostics
