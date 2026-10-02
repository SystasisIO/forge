module;

#include <span>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.net.p2p.peer_store;

import forge.asio.gate;

#include "details/peer_store_impl.hxx"

namespace forge::net::p2p {

peer_store::persistence::~persistence() = default;

peer_store::peer_store() : peer_store{options{.persistence = make_memory_persistence()}} {}

peer_store::peer_store(options options_value) : impl_(std::make_shared<impl>(std::move(options_value))) {}

peer_store::~peer_store() = default;
peer_store::peer_store(peer_store&&) noexcept = default;
peer_store& peer_store::operator=(peer_store&&) noexcept = default;

void peer_store::upsert(record value) {
   impl_->upsert(std::move(value));
}

peer_store::record peer_store::apply_identify(const peer_id& peer, identify_update update) {
   return impl_->apply_identify(peer, std::move(update));
}

std::optional<peer_store::record> peer_store::apply_discovery(const peer_id& peer, discovery_update update) {
   return impl_->apply_discovery(peer, std::move(update));
}

void peer_store::apply_peer_exchange(const peer_id& peer, capability_set capabilities) {
   impl_->apply_peer_exchange(peer, capabilities);
}

void peer_store::upsert_relay_reservation(relay_record value) {
   impl_->upsert_relay_reservation(std::move(value));
}

bool peer_store::mark_discovery_failure(const peer_id& peer, std::chrono::system_clock::time_point backoff_until) {
   return impl_->mark_discovery_failure(peer, backoff_until);
}

std::size_t peer_store::prune_expired_relay_reservations(const peer_id& peer,
                                                         std::chrono::system_clock::time_point now) {
   return impl_->prune_expired_relay_reservations(peer, now);
}

void peer_store::learn_address(peer_id peer, forge::multiformats::multiaddr address, capability_set capabilities) {
   impl_->learn_address(std::move(peer), std::move(address), capabilities);
}

void peer_store::learn_endpoint(peer_id peer, forge::net::p2p::endpoint endpoint, capability_set capabilities) {
   learn_address(std::move(peer), endpoint.to_multiaddr(), capabilities);
}

void peer_store::mark_reachability(peer_id peer, reachability::state state,
                                   std::optional<forge::net::p2p::endpoint> observed) {
   impl_->mark_reachability(std::move(peer), state, std::move(observed));
}

void peer_store::mark_success(const peer_id& peer, path::kind kind, std::chrono::milliseconds latency) {
   impl_->mark_success(peer, kind, latency);
}

void peer_store::mark_failure(const peer_id& peer) {
   impl_->mark_failure(peer);
}

void peer_store::mark_address_success(const peer_id& peer, const forge::multiformats::multiaddr& address,
                                      path::kind kind, std::chrono::milliseconds latency) {
   impl_->mark_address_success(peer, address, kind, latency);
}

void peer_store::mark_address_failure(const peer_id& peer, const forge::multiformats::multiaddr& address,
                                      path::kind kind, std::chrono::system_clock::time_point backoff_until) {
   impl_->mark_address_failure(peer, address, kind, backoff_until);
}

void peer_store::mark_endpoint_success(const peer_id& peer, const forge::net::p2p::endpoint& endpoint,
                                       path::kind kind, std::chrono::milliseconds latency) {
   mark_address_success(peer, endpoint.to_multiaddr(), kind, latency);
}

void peer_store::mark_endpoint_failure(const peer_id& peer, const forge::net::p2p::endpoint& endpoint,
                                       path::kind kind, std::chrono::system_clock::time_point backoff_until) {
   mark_address_failure(peer, endpoint.to_multiaddr(), kind, backoff_until);
}

void peer_store::upsert_routing_peer(protocol_id protocol, dht::peer value, discovery::source source,
                                     std::chrono::system_clock::time_point expires_at) {
   impl_->upsert_routing_peer(std::move(protocol), std::move(value), source, expires_at);
}

boost::asio::awaitable<void> peer_store::async_upsert_rendezvous(rendezvous::registration value) {
   return impl::async_upsert_rendezvous_owned(impl_, std::move(value));
}

boost::asio::awaitable<void> peer_store::async_register_rendezvous(rendezvous::registration value,
                                                                   std::size_t max_registrations_per_peer) {
   return impl::async_register_rendezvous_owned(impl_, std::move(value), max_registrations_per_peer);
}

boost::asio::awaitable<void> peer_store::async_remove_rendezvous(peer_id peer, std::string namespace_name) {
   return impl::async_remove_rendezvous_owned(impl_, std::move(peer), std::move(namespace_name));
}

boost::asio::awaitable<void> peer_store::async_hydrate() {
   return impl::async_hydrate_owned(impl_);
}

boost::asio::awaitable<peer_store::prune_result>
peer_store::async_prune_expired(std::chrono::system_clock::time_point now) {
   return impl::async_prune_expired_owned(impl_, now);
}

boost::asio::awaitable<void> peer_store::async_flush() {
   return impl::async_flush_owned(impl_);
}

boost::asio::awaitable<void> peer_store::async_close() {
   return impl::async_close_owned(impl_);
}

std::optional<peer_store::record> peer_store::find(const peer_id& peer) const {
   return impl_->find(peer);
}

std::optional<public_key> peer_store::find_public_key(const peer_id& peer) const {
   return impl_->find_public_key(peer);
}

std::vector<peer_store::record> peer_store::snapshot(std::size_t limit) const {
   return impl_->snapshot(limit);
}

std::vector<peer_store::record> peer_store::candidates(std::uint64_t capability, std::size_t limit) const {
   return impl_->candidates(capability, limit, {});
}

std::vector<peer_store::record> peer_store::candidates(std::uint64_t capability, std::size_t limit,
                                                     std::span<const peer_id> excluded_peers) const {
   return impl_->candidates(capability, limit, excluded_peers);
}

std::vector<peer_store::record> peer_store::scored_candidates(std::size_t limit) const {
   return impl_->scored_candidates(limit);
}

std::vector<peer_store::record> peer_store::scored_candidates(discovery::source source, std::size_t limit) const {
   return impl_->scored_candidates(source, limit);
}

std::vector<rendezvous::registration> peer_store::discover_rendezvous(std::string_view namespace_name,
                                                                      std::uint64_t after_sequence,
                                                                      std::size_t limit) const {
   return impl_->discover_rendezvous(namespace_name, after_sequence, limit);
}

peer_store::persistence_status peer_store::persistence_state() const {
   return impl_->persistence_state();
}

} // namespace forge::net::p2p
