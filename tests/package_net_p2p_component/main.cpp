#include <chrono>
#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>

import forge.chrono.timestamp;
import forge.net.p2p.dht;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.address_resolution;
import forge.net.p2p.dialing;
import forge.net.p2p.endpoint;
import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identity;
import forge.net.p2p.ipns;
import forge.net.p2p.lifecycle;
import forge.net.p2p.mdns_policy;
import forge.net.p2p.provider_registration;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.reachability_policy;
import forge.net.p2p.relay;
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
static_assert(!p2p::relay::policy{}.service_enabled);
static_assert(p2p::relay::policy{}.client_enabled);
static_assert(p2p::relay::policy{}.target_reservations == 2);
static_assert(std::is_same_v<decltype(p2p::node::options{}.relay_policy), p2p::relay::policy>);
static_assert(requires(p2p::node& node, p2p::peer_id peer) {
   { node.async_reserve_relay(peer) } -> std::same_as<boost::asio::awaitable<p2p::relay::reservation::info>>;
   { node.async_refresh_relay_candidates() }
       -> std::same_as<boost::asio::awaitable<std::vector<p2p::relay::reservation::info>>>;
   { node.async_cancel_relay(peer) } -> std::same_as<boost::asio::awaitable<void>>;
   { node.async_attempt_hole_punch(peer) } -> std::same_as<boost::asio::awaitable<p2p::hole_punch::status>>;
   { node.async_cancel_hole_punch(peer) } -> std::same_as<boost::asio::awaitable<bool>>;
});

static_assert(requires(forge::net::p2p::node& node, forge::multiformats::multiaddr address) {
   node.async_connect(address);
   node.async_connect(address, forge::net::p2p::node::connect_options{});
   forge::net::p2p::bootstrap_peer{.address = address};
});

static_assert(p2p::node::coordinated_connect_options{}.timeout == std::chrono::seconds{10});
static_assert(requires(p2p::node& node, p2p::endpoint remote, p2p::node::coordinated_connect_options options) {
   { node.async_connect_coordinated(remote, options) } -> std::same_as<boost::asio::awaitable<p2p::node::session_info>>;
});
static_assert(std::is_same_v<decltype(p2p::pubsub::options{}.scoring),
                             std::optional<p2p::pubsub::scoring_params>>);
static_assert(requires(const p2p::node& node) {
   { node.pubsub_scores() } -> std::same_as<p2p::pubsub::score_snapshot>;
});
static_assert(requires(std::span<const std::uint8_t> bytes, const p2p::pubsub::options& options) {
   { p2p::pubsub::codec::decode_received(bytes, options) } -> std::same_as<p2p::pubsub::codec::received_rpc>;
});
static_assert(std::is_copy_constructible_v<p2p::pubsub::partial_topic>);
static_assert(requires(p2p::node& node, p2p::pubsub::topic topic, p2p::pubsub::handler full,
                      p2p::pubsub::partial_options partial, p2p::pubsub::partial_topic registration,
                      p2p::peer_id peer, p2p::pubsub::partial_message value, std::stop_token stop,
                      std::vector<std::uint8_t> group) {
   { node.async_subscribe(topic, full, partial) } -> std::same_as<boost::asio::awaitable<p2p::pubsub::partial_topic>>;
   { node.async_unsubscribe(registration) } -> std::same_as<boost::asio::awaitable<void>>;
   { node.async_advertise_partial(registration, group) } -> std::same_as<boost::asio::awaitable<void>>;
   { node.async_forget_partial(registration, group) } -> std::same_as<boost::asio::awaitable<void>>;
   { node.async_partial_peers(registration) } -> std::same_as<boost::asio::awaitable<std::vector<p2p::peer_id>>>;
   { node.async_send_partial(registration, peer, value, stop) } -> std::same_as<boost::asio::awaitable<void>>;
});

int main() {
   auto extension_options = p2p::pubsub::options{};
   extension_options.preferred = p2p::pubsub::version::v1_3;
   extension_options.partial_messages = true;
   p2p::pubsub::validate(extension_options);
   const auto extension = p2p::pubsub::rpc{
       .control_value = p2p::pubsub::control{
           .dont_want = {p2p::pubsub::control::idontwant{.message_ids = {{1, 2, 3}}}},
           .extensions = p2p::pubsub::extensions{.partial_messages = true}},
       .partial = p2p::pubsub::partial_message{.subject = p2p::pubsub::topic{"package-partial"},
           .group_id = std::vector<std::uint8_t>{1}, .metadata = std::vector<std::uint8_t>{}}};
   const auto extension_frame = p2p::pubsub::codec::encode(extension, extension_options);
   const auto extension_received = p2p::pubsub::codec::decode_received(extension_frame, extension_options);
   if (extension_received.extension_advertisements != 1 || !extension_received.value.control_value ||
       !extension_received.value.control_value->extensions ||
       extension_received.value.control_value->extensions->partial_messages != true ||
       !extension_received.value.partial || extension_received.value.partial->data ||
       !extension_received.value.partial->metadata || !extension_received.value.partial->metadata->empty() ||
       extension_received.value.partial->group_id != extension.partial->group_id) {
      return 1;
   }
   auto unsigned_options = p2p::pubsub::options{};
   unsigned_options.signatures = p2p::pubsub::signature_policy::strict_no_sign;
   const auto unsigned_message = p2p::pubsub::message{.data = {'p'}, .subject = {"package-unsigned"}};
   const auto unsigned_frame = p2p::pubsub::codec::encode(p2p::pubsub::rpc{.messages = {unsigned_message}}, unsigned_options);
   const auto unsigned_received = p2p::pubsub::codec::decode_received(unsigned_frame, unsigned_options);
   if (unsigned_received.value.messages.size() != 1U || !unsigned_received.invalid_messages.empty()) {
      return 1;
   }
   const auto& unsigned_roundtrip = unsigned_received.value.messages.front();
   if (unsigned_roundtrip.from || !unsigned_roundtrip.seqno.empty() || !unsigned_roundtrip.signature.empty() ||
       !unsigned_roundtrip.key.empty() || unsigned_roundtrip.subject != unsigned_message.subject ||
       unsigned_roundtrip.data != unsigned_message.data) {
      return 1;
   }
   auto scoring = p2p::pubsub::scoring_params{};
   auto topic_scoring = p2p::pubsub::topic_score_params{};
   topic_scoring.invalid_message_deliveries_weight = -100.0;
   scoring.topics.emplace(p2p::pubsub::topic{"package-scoring"}, topic_scoring);
   p2p::pubsub::validate(scoring);
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
