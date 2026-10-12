#include "forge_plugin_interop_fixture.hxx"
#include "dht_value_fixture.hxx"

namespace forge::test::libp2p_interop {
namespace {
namespace p2p = forge::net::p2p;
namespace np = forge::plugins::net::p2p::node;
using object = forge::mutable_variant_object;
using namespace std::chrono_literals;

forge::variant value_record_receipt(const p2p::dht::record& record) {
   return object{}("key_hex", forge::codec::hex::encode(record.key_value.bytes))
       ("value_hex", forge::codec::hex::encode(record.value))
       ("value_bytes", static_cast<std::uint64_t>(record.value.size()))
       ("value_sha256", forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{record.value}).str());
}
} // namespace

forge::variant forge_plugin_interop_fixture::dht_session_facts(const p2p::diagnostics::session& session) const {
   if (session.closed || session.identify_state != p2p::identify::state::identified ||
       session.path != p2p::path::kind::direct || !session.local_endpoint || !session.remote_endpoint) {
      throw std::runtime_error{"DHT service lacks an identified retained direct session"};
   }
   auto authentication = std::string{};
   switch (session.authentication) {
   case p2p::peer_authentication::noise: authentication = "noise"; break;
   case p2p::peer_authentication::libp2p_tls: authentication = "libp2p_tls"; break;
   case p2p::peer_authentication::quic_tls: authentication = "quic_tls"; break;
   default: throw std::runtime_error{"DHT service session is not authenticated"};
   }
   const auto transport = argument("transport");
   const auto expected = transport == "quic" ? p2p::peer_authentication::quic_tls
       : transport == "tcp-tls" || transport == "tcp-pnet-tls" ? p2p::peer_authentication::libp2p_tls
       : p2p::peer_authentication::noise;
   if (session.authentication != expected ||
       (expected != p2p::peer_authentication::quic_tls && session.muxer.value != "/yamux/1.0.0")) {
      throw std::runtime_error{"DHT service session disagrees with configured security or TCP muxer"};
   }
   return object{}("source", "node.diagnostics-api.session")("session_id", session.id)
       ("local_peer_id", _local_peer)("remote_peer_id", session.remote_peer.to_string())
       ("identify_state", "identified")("authentication", authentication)("muxer", session.muxer.value)
       ("local_address", session.local_endpoint->to_string())("remote_address", session.remote_endpoint->to_string())
       ("path", "direct")
       ("direction", session.direction == p2p::diagnostics::session_direction::inbound ? "inbound" : "outbound");
}

boost::asio::awaitable<forge::variant> forge_plugin_interop_fixture::dht_session_receipt(
    p2p::node::session_info connected) {
   const auto executor = co_await boost::asio::this_coro::executor;
   auto timer = boost::asio::steady_timer{executor};
   const auto deadline = std::chrono::steady_clock::now() + 4s;
   while (std::chrono::steady_clock::now() < deadline) {
      const auto state = api<np::diagnostics_api>()->snapshot();
      for (const auto& session : state.sessions) {
         if (session.id != connected.id || session.remote_peer != connected.remote_peer || session.closed ||
             session.identify_state != p2p::identify::state::identified) { continue; }
         if (session.path != connected.path || session.muxer != connected.muxer) {
            throw std::runtime_error{"DHT service connect result and retained session disagree"};
         }
         co_return dht_session_facts(session);
      }
      timer.expires_after(5ms);
      co_await timer.async_wait(boost::asio::use_awaitable);
   }
   throw std::runtime_error{"DHT service session did not complete Identify inside owned deadline"};
}

boost::asio::awaitable<void> forge_plugin_interop_fixture::dht_exchange() {
   const auto mode = argument("payload");
   if (mode != "put_only" && mode != "get_only") {
      throw std::runtime_error{"unknown DHT value service mode"};
   }
   const auto expected = dht_value_fixture::for_scenario(argument("scenario"));
   const auto peer = p2p::peer_id::from_string(argument("peer-id"));
   set_phase("service.dht.connect");
   const auto connected = co_await api<np::api>()->connect(
       forge::multiformats::multiaddr::parse(argument("addr")),
       {.expected_peer = peer, .allow_relay = false, .timeout = 6s, .direct_attempt_timeout = 2s,
        .allow_hole_punch = false});
   if (connected.remote_peer != peer || connected.id == 0 || connected.path != p2p::path::kind::direct) {
      throw std::runtime_error{"DHT service connect did not return the expected direct peer"};
   }
   set_phase("service.dht.session_receipt");
   const auto session = co_await dht_session_receipt(connected);
   auto receipt = object{}("source", "official-node.dht-api")("operation", mode)
       ("profile", p2p::builtins::kad_dht.value)("session", session)
       ("connect", object{}("source", "node.api.connect")("session_id", connected.id)
           ("remote_peer_id", connected.remote_peer.to_string())("muxer", connected.muxer.value)
           ("identify_state", static_cast<std::uint64_t>(connected.identify_state)))
       ("expected_peer_id", peer.to_string())("dial_address", argument("addr"));
   auto matches = false;
   auto reached = false;
   if (mode == "put_only") {
      set_phase("service.dht.put_value");
      const auto result = co_await api<np::dht_api>()->put_value(p2p::builtins::kad_dht,
          {.key_value = expected.key, .value = expected.value}, {.timeout = 5s});
      matches = result.selected.key_value.bytes == expected.key.bytes && result.selected.value == expected.value;
      reached = result.attempted > 0 && result.accepted > 0 && result.quorum_reached;
      receipt("attempted", static_cast<std::uint64_t>(result.attempted))
          ("accepted", static_cast<std::uint64_t>(result.accepted))("quorum_reached", result.quorum_reached)
          ("record", value_record_receipt(result.selected));
   } else {
      set_phase("service.dht.get_value");
      const auto result = co_await api<np::dht_api>()->get_value(p2p::builtins::kad_dht, expected.key, {.timeout = 5s});
      matches = result.selected && result.selected->key_value.bytes == expected.key.bytes &&
          result.selected->value == expected.value;
      reached = result.responses > 0 && result.valid_records > 0 && result.quorum_reached;
      receipt("responses", static_cast<std::uint64_t>(result.responses))
          ("valid_records", static_cast<std::uint64_t>(result.valid_records))("quorum_reached", result.quorum_reached)
          ("record", result.selected ? value_record_receipt(*result.selected) : forge::variant{});
   }
   receipt("record_matches_golden", matches);
   {
      const auto lock = std::lock_guard{_mutex};
      _service = std::move(receipt);
   }
   if (!reached || !matches) {
      throw std::runtime_error{"DHT value service did not reach remote quorum with the golden record"};
   }
}

void forge_plugin_interop_fixture::observe_dht_service(bool stopping) {
   const auto state = api<np::diagnostics_api>()->snapshot();
   const auto profile = std::find_if(state.dht_profiles.begin(), state.dht_profiles.end(), [](const auto& value) {
      return value.protocol == p2p::builtins::kad_dht;
   });
   if (profile == state.dht_profiles.end() || profile->kind != "amino-v1" || profile->mode != "server" ||
       !profile->values) {
      throw std::runtime_error{"configured DHT service profile is not an actual Amino value server"};
   }
   auto sessions = std::map<std::uint64_t, forge::variant>{};
   for (const auto& session : state.sessions) {
      if (!session.closed && session.identify_state == p2p::identify::state::identified) {
         sessions.emplace(session.id, dht_session_facts(session));
      }
   }
   const auto actual_profile = object{}("protocol", profile->protocol.value)("kind", profile->kind)
       ("mode", profile->mode)("values", profile->values)("routing_active", static_cast<std::uint64_t>(profile->routing_active))
       ("routing_candidates", static_cast<std::uint64_t>(profile->routing_candidates))
       ("persistence_failures", profile->persistence_failures)("persistence_degraded", profile->persistence_degraded)
       ("durability_uncertain", profile->durability_uncertain)("persistence_closing", profile->persistence_closing)
       ("persistence_closed", profile->persistence_closed);
   const auto lock = std::lock_guard{_mutex};
   for (auto& [id, session] : sessions) {
      constexpr auto capture_limit = std::size_t{64};
      if (!_service_sessions.contains(id) && _service_sessions.size() >= capture_limit) {
         throw std::runtime_error{"DHT service session capture budget exceeded"};
      }
      _service_sessions.insert_or_assign(id, std::move(session));
   }
   _service_diagnostics = object{}("source", "node.diagnostics-api.snapshot")
       ("phase", stopping ? "before-stop" : "running")("profile", actual_profile)
       ("dht_queries", state.metrics.dht_queries)("dht_responses", state.metrics.dht_responses)
       ("handshakes_completed", state.metrics.handshakes_completed)("sessions_opened", state.metrics.sessions_opened)
       ("active_sessions", static_cast<std::uint64_t>(state.connections.active_sessions))
       ("protocol_streams_opened", state.metrics.protocol_streams_opened)
       ("protocol_streams_accepted", state.metrics.protocol_streams_accepted);
   if (argument("command") == "listen") {
      auto observed = forge::variants{};
      for (const auto& [_, session] : _service_sessions) { observed.push_back(session); }
      _service = object{}("source", "official-node.native-dht-service-and-diagnostics-api")
          ("operation", "serve")("profile", profile->protocol.value)("sessions", observed)
          ("record_observation", "not-queried");
   }
}

} // namespace forge::test::libp2p_interop
