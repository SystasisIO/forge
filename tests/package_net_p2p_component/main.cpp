#include <chrono>
#include <concepts>
#include <optional>
#include <type_traits>
#include <utility>
#include <boost/asio/awaitable.hpp>

import forge.chrono.timestamp;
import forge.net.p2p.dht;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.address_resolution;
import forge.net.p2p.dialing;
import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;
import forge.net.p2p.identity;
import forge.net.p2p.ipns;
import forge.net.p2p.lifecycle;
import forge.net.p2p.mdns_policy;
import forge.net.p2p.provider_registration;
import forge.net.p2p.reachability;
import forge.net.p2p.reachability_policy;
import forge.net.p2p.topology;
import forge.net.p2p.node;
import forge.multiformats.multiaddr;

namespace p2p = forge::net::p2p;

static_assert(std::is_same_v<decltype(std::declval<const p2p::ipns::record&>().eol()),
                             forge::chrono::timestamp>);
static_assert(requires(const p2p::node& node, forge::chrono::timestamp eol) {
   { node.create_ipns_record({}, 1, eol, std::chrono::seconds{1}) } -> std::same_as<p2p::ipns::record>;
});

static_assert(!std::is_copy_constructible_v<p2p::host_event_subscription>);
static_assert(!std::is_copy_assignable_v<p2p::host_event_subscription>);
static_assert(std::is_nothrow_move_constructible_v<p2p::host_event_subscription>);
static_assert(std::is_nothrow_move_assignable_v<p2p::host_event_subscription>);
static_assert(std::is_same_v<decltype(p2p::node::options{}.reachability_policy), p2p::reachability_policy>);
static_assert(requires(const p2p::node& node, p2p::host_event_subscription& subscription) {
   { node.reachability_status() } -> std::same_as<p2p::host_event>;
   { node.host_events() } -> std::same_as<p2p::host_event_subscription>;
   { subscription.async_read() } -> std::same_as<boost::asio::awaitable<std::optional<p2p::host_event>>>;
   { subscription.active() } noexcept -> std::same_as<bool>;
   { subscription.close() } noexcept -> std::same_as<void>;
});
static_assert(p2p::reachability_policy{}.client_v1_enabled && p2p::reachability_policy{}.client_v2_enabled);
static_assert(!p2p::reachability_policy{}.service_v1_enabled && !p2p::reachability_policy{}.service_v2_enabled);
static_assert(p2p::reachability_policy{}.ping_enabled);
static_assert(p2p::reachability_policy{}.timeout == std::chrono::seconds{20});
static_assert(p2p::reachability_policy{}.observation_ttl == std::chrono::minutes{10});
static_assert(p2p::reachability_policy{}.min_observers == 4);
static_assert(p2p::reachability_policy{}.min_reachability_observers == 3);
static_assert(p2p::reachability_policy{}.max_observations == 1024);
static_assert(p2p::reachability_policy{}.max_candidates == 256);
static_assert(p2p::reachability_policy{}.max_confirmed_per_local == 3);
static_assert(p2p::reachability_policy{}.max_pending_probes == 4);

static_assert(requires(forge::net::p2p::node& node, forge::multiformats::multiaddr address) {
   node.async_connect(address);
   node.async_connect(address, forge::net::p2p::node::connect_options{});
   forge::net::p2p::bootstrap_peer{.address = address};
});

int main() {
   p2p::validate(p2p::mdns_policy{});
   const auto scoped_text = "/ip6zone/en0/ip6/fe80::1/tcp/4001";
   const auto scoped_address = forge::multiformats::multiaddr::parse(scoped_text);
   if (forge::multiformats::multiaddr::from_bytes(scoped_address.to_bytes()).to_string() != scoped_text) {
      return 1;
   }
   const auto whole_seconds = std::chrono::sys_seconds{std::chrono::sys_days{
       std::chrono::year{9999} / std::chrono::December / 31}};
   const auto eol = forge::chrono::timestamp{whole_seconds, std::chrono::nanoseconds{999'999'999}};
   if (eol.whole_seconds() != whole_seconds || eol.subsecond() != std::chrono::nanoseconds{999'999'999}) {
      return 1;
   }
   const auto id = forge::net::p2p::peer_id{};
   auto store = forge::net::p2p::dht::record_store{
       forge::net::p2p::amino_v1(), {.persistence = forge::net::p2p::dht::record_store::make_memory_persistence()}};
   auto registration = forge::net::p2p::provider_registration{};
   const auto topology = forge::net::p2p::topology::policy{};
   const auto address_resolution = forge::net::p2p::address_resolution::policy{};
   const auto dialing = forge::net::p2p::dialing::black_hole_policy{};
   const auto event = p2p::host_event{};
   auto subscription = p2p::host_event_subscription{};
   auto moved = std::move(subscription);
   moved.close();
   return id.value.empty() && !registration.active() && forge::net::p2p::ipns::routing_prefix.size() == 6 &&
                  !subscription.active() && !moved.active() && event.generation == 0 && !event.resync_required &&
                  event.phase == p2p::lifecycle_phase::idle && event.effective == p2p::reachability::state::unknown &&
                  event.autonat_v1 == p2p::reachability::state::unknown && event.autonat_v2.empty() &&
                  event.confirmed_addresses.empty() &&
                  !store.persistence_state().closed &&
                  topology.operating_mode == forge::net::p2p::topology::mode::managed &&
                  topology.peers.low == 128 && topology.peers.target == 160 && topology.peers.high == 192 &&
                  address_resolution.bounds.max_dns_lookups == 32 && address_resolution.bounds.max_txt_records == 16 &&
                  dialing.window_size == 100 && dialing.min_successes == 5 && dialing.udp_enabled && dialing.ipv6_enabled
              ? 0
              : 1;
}
