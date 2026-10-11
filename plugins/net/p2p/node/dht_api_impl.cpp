module;

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.api.transport.options;
import forge.asio.runtime;
import forge.asio.task;
import forge.chrono.timestamp;
import forge.net.p2p.dht;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.p2p.ipns;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.provider_registration;
import forge.net.p2p.pubsub;
import forge.net.p2p.scoring;
import forge.plugins.crypto.secrets.api;
import forge.plugins.db.store.api;
import forge.plugins.net.p2p.node.dht_api;
import forge.plugins.net.p2p.node.types;

#include "details/plugin_impl.hxx"
#include "details/dht_api_impl.hxx"

namespace forge::plugins::net::p2p::node {

plugin::dht_api_impl::dht_api_impl(std::shared_ptr<plugin::impl> impl) : impl_{std::move(impl)} {}

boost::asio::awaitable<forge::net::p2p::dht::query_result>
plugin::dht_api_impl::find_peer(forge::net::p2p::protocol_id profile, forge::net::p2p::peer_id peer,
                                forge::net::p2p::dht::query_options options) {
   return find_peer_owned(impl_, std::move(profile), std::move(peer), options);
}

boost::asio::awaitable<forge::net::p2p::provider_registration>
plugin::dht_api_impl::provide(forge::net::p2p::protocol_id profile, forge::net::p2p::dht::key key,
                              forge::net::p2p::dht::query_options options) {
   return provide_owned(impl_, std::move(profile), std::move(key), options);
}

boost::asio::awaitable<std::vector<forge::net::p2p::dht::peer>>
plugin::dht_api_impl::find_providers(forge::net::p2p::protocol_id profile, forge::net::p2p::dht::key key,
                                     forge::net::p2p::dht::query_options options) {
   return find_providers_owned(impl_, std::move(profile), std::move(key), options);
}

boost::asio::awaitable<forge::net::p2p::dht::value_put_result>
plugin::dht_api_impl::put_value(forge::net::p2p::protocol_id profile, forge::net::p2p::dht::record value,
                                forge::net::p2p::dht::query_options options) {
   return put_value_owned(impl_, std::move(profile), std::move(value), options);
}

boost::asio::awaitable<forge::net::p2p::dht::value_get_result>
plugin::dht_api_impl::get_value(forge::net::p2p::protocol_id profile, forge::net::p2p::dht::key key,
                                forge::net::p2p::dht::query_options options) {
   return get_value_owned(impl_, std::move(profile), std::move(key), options);
}

boost::asio::awaitable<forge::net::p2p::dht::query_result>
plugin::dht_api_impl::find_peer_owned(std::shared_ptr<plugin::impl> state, forge::net::p2p::protocol_id profile,
                                      forge::net::p2p::peer_id peer, forge::net::p2p::dht::query_options options) {
   const auto current = state->require_running_node();
   co_return co_await current->async_find_peer(std::move(profile), std::move(peer), options);
}

boost::asio::awaitable<forge::net::p2p::provider_registration>
plugin::dht_api_impl::provide_owned(std::shared_ptr<plugin::impl> state, forge::net::p2p::protocol_id profile,
                                    forge::net::p2p::dht::key key, forge::net::p2p::dht::query_options options) {
   const auto current = state->require_running_node();
   co_return co_await current->async_provide(std::move(profile), std::move(key), options);
}

boost::asio::awaitable<std::vector<forge::net::p2p::dht::peer>>
plugin::dht_api_impl::find_providers_owned(std::shared_ptr<plugin::impl> state, forge::net::p2p::protocol_id profile,
                                           forge::net::p2p::dht::key key, forge::net::p2p::dht::query_options options) {
   const auto current = state->require_running_node();
   co_return co_await current->async_find_providers(std::move(profile), std::move(key), options);
}

boost::asio::awaitable<forge::net::p2p::dht::value_put_result>
plugin::dht_api_impl::put_value_owned(std::shared_ptr<plugin::impl> state, forge::net::p2p::protocol_id profile,
                                      forge::net::p2p::dht::record value, forge::net::p2p::dht::query_options options) {
   const auto current = state->require_running_node();
   co_return co_await current->async_put_value(std::move(profile), std::move(value), options);
}

boost::asio::awaitable<forge::net::p2p::dht::value_get_result>
plugin::dht_api_impl::get_value_owned(std::shared_ptr<plugin::impl> state, forge::net::p2p::protocol_id profile,
                                      forge::net::p2p::dht::key key, forge::net::p2p::dht::query_options options) {
   const auto current = state->require_running_node();
   co_return co_await current->async_get_value(std::move(profile), std::move(key), options);
}

forge::net::p2p::ipns::record
plugin::dht_api_impl::create_ipns_record(std::span<const std::uint8_t> value, std::uint64_t sequence,
                                         forge::chrono::timestamp eol, std::chrono::nanoseconds ttl,
                                         forge::net::p2p::ipns::create_options options) const {
   const auto current = impl_->require_running_node();
   return current->create_ipns_record(value, sequence, eol, ttl, std::move(options));
}

} // namespace forge::plugins::net::p2p::node
