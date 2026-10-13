#include "forge_plugin_interop_fixture.hxx"
#include <variant>

namespace forge::test::libp2p_interop {
namespace {
namespace p2p = forge::net::p2p;
namespace np = forge::plugins::net::p2p::node;
using object = forge::mutable_variant_object;
using value = forge::config::core::value;
using namespace std::chrono_literals;
constexpr std::size_t seed_limit = 4 * 513;
constexpr std::size_t observation_limit = 602;
constexpr std::size_t item_limit = 64 * 1024;
constexpr std::size_t capture_limit = 4 * 1024 * 1024;

std::int64_t unix_ms() {
   return std::chrono::duration_cast<std::chrono::milliseconds>(
       std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string identify_name(p2p::identify::state state) {
   switch (state) {
   case p2p::identify::state::unknown: return "unknown";
   case p2p::identify::state::identifying: return "identifying";
   case p2p::identify::state::identified: return "identified";
   case p2p::identify::state::failed: return "failed";
   }
   throw std::runtime_error{"invalid native Identify state"};
}

std::string reachability_name(p2p::reachability::state state) {
   switch (state) {
   case p2p::reachability::state::unknown: return "unknown";
   case p2p::reachability::state::publicly_reachable: return "public";
   case p2p::reachability::state::private_network: return "private";
   case p2p::reachability::state::blocked: return "blocked";
   case p2p::reachability::state::relay_only: return "relay_only";
   }
   throw std::runtime_error{"invalid native reachability state"};
}

std::string authentication_name(p2p::peer_authentication state) {
   switch (state) {
   case p2p::peer_authentication::unverified: return "unverified";
   case p2p::peer_authentication::quic_tls: return "quic_tls";
   case p2p::peer_authentication::libp2p_tls: return "libp2p_tls";
   case p2p::peer_authentication::noise: return "noise";
   }
   throw std::runtime_error{"invalid native authentication state"};
}

bool actual_tcp(const p2p::endpoint& endpoint) {
   return endpoint.is_direct_tcp() && endpoint.transport.port != 0 &&
       (endpoint.transport.host_type == p2p::endpoint::host_kind::ip4 ||
        endpoint.transport.host_type == p2p::endpoint::host_kind::ip6);
}

object direct_facts(const p2p::diagnostics::session& session, const p2p::peer_id& local) {
   if (!p2p::valid_peer_id(local) || !p2p::valid_peer_id(session.remote_peer) ||
       session.remote_peer == local || session.id == 0 || session.closed ||
       session.path != p2p::path::kind::direct || session.relay_peer || session.carrier_session_id ||
       session.circuit_endpoint || session.authentication != p2p::peer_authentication::noise ||
       session.muxer.value != "/yamux/1.0.0" || session.identify_state != p2p::identify::state::identified ||
       !session.local_endpoint || !session.remote_endpoint || !actual_tcp(*session.local_endpoint) ||
       !actual_tcp(*session.remote_endpoint) ||
       (session.local_endpoint->peer && *session.local_endpoint->peer != local) ||
       (session.remote_endpoint->peer && *session.remote_endpoint->peer != session.remote_peer)) {
      throw std::runtime_error{"AutoRelay carrier lacks correlated authenticated socket facts"};
   }
   if (session.direction != p2p::diagnostics::session_direction::inbound &&
       session.direction != p2p::diagnostics::session_direction::outbound) {
      throw std::runtime_error{"invalid native session direction"};
   }
   return object{}("source", "official-node.diagnostics_api.session")("session_id", session.id)
       ("local_peer_id", local.to_string())("remote_peer_id", session.remote_peer.to_string())
       ("path", "direct")("authentication", authentication_name(session.authentication))
       ("authentication_basis", "native_session_upgrade_output")("muxer", session.muxer.value)
       ("identify_state", identify_name(session.identify_state))("identified", true)("closed", session.closed)
       ("direction", session.direction == p2p::diagnostics::session_direction::inbound ? "inbound" : "outbound")
       ("local_address", session.local_endpoint->to_string())("remote_address", session.remote_endpoint->to_string());
}

p2p::diagnostics::options bounded_snapshot() {
   return {.max_peers = 16, .max_sessions = 32, .max_endpoints_per_peer = 16,
           .max_protocols_per_peer = 128, .max_relay_reservations_per_peer = 4, .max_dht_profiles = 4};
}

std::string read_seeds(const std::filesystem::path& path, bool allow_missing) {
   const auto status = std::filesystem::symlink_status(path);
   if (allow_missing && status.type() == std::filesystem::file_type::not_found) { return {}; }
   if (!std::filesystem::is_regular_file(status)) {
      throw std::runtime_error{"AutoRelay seed input is not a bounded regular file"};
   }
   const auto size = std::filesystem::file_size(path);
   if (size > seed_limit) { throw std::runtime_error{"AutoRelay seed input bound"}; }
   auto input = std::ifstream{path, std::ios::binary};
   if (!input) { throw std::runtime_error{"cannot open AutoRelay seed input"}; }
   auto bytes = std::string(static_cast<std::size_t>(size) + 1, '\0');
   input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
   bytes.resize(static_cast<std::size_t>(input.gcount()));
   if (input.bad() || bytes.size() > seed_limit) { throw std::runtime_error{"AutoRelay seed read bound"}; }
   return bytes;
}

std::vector<p2p::endpoint> seeds(std::string_view previous, std::string_view next, std::size_t& offset,
                               const std::set<std::string>& connected, bool stopping) {
   if (next.size() > seed_limit || !next.starts_with(previous) || offset > next.size()) {
      throw std::runtime_error{"AutoRelay seed rewrite/truncation/bound"};
   }
   auto unique = connected;
   auto result = std::vector<p2p::endpoint>{};
   auto cursor = offset;
   while (cursor < next.size()) {
      const auto end = next.find('\n', cursor);
      if (end == std::string_view::npos) {
         if (stopping || next.size() - cursor > 512) { throw std::runtime_error{"incomplete AutoRelay seed"}; }
         break;
      }
      const auto line = next.substr(cursor, end - cursor);
      if (line.empty() || line.size() > 512) { throw std::runtime_error{"invalid AutoRelay seed line"}; }
      const auto endpoint = p2p::parse_endpoint(line);
      if (!endpoint.peer || !p2p::valid_peer_id(*endpoint.peer) || !actual_tcp(endpoint) ||
          endpoint.transport.host_type != p2p::endpoint::host_kind::ip4 ||
          endpoint.transport.host != "127.0.0.1" || endpoint.to_string() != line) {
         throw std::runtime_error{"AutoRelay seed must be canonical loopback TCP with expected peer"};
      }
      if (unique.insert(std::string{line}).second) { result.emplace_back(endpoint); }
      if (unique.size() > 4 || (stopping && !result.empty())) {
         throw std::runtime_error{"AutoRelay seed bound or unconsumed admission at stop"};
      }
      cursor = end + 1;
   }
   offset = cursor;
   return result;
}

value relay_profile(bool service) {
   const auto parsed = forge::codec::yaml::read_document(
       "relay:\n  client-enabled: true\n  server-enabled: false\n  auto-discovery-enabled: true\n"
       "  public-allowed: false\n  reservation-ttl-ms: 8000\n  max-candidates: 4\n"
       "  target-reservations: 1\n  refresh-margin-ms: 3000\n  max-parallel-reservations: 1\n"
       "  candidate-backoff-ms: 1000\n  max-active-relays: 4\n  max-reservations: 4\n"
       "  max-streams-per-reservation: 4\n  max-relay-bytes: 1048576\n  max-queued-bytes: 262144\n"
       "  max-duration-ms: 20000\n  handshake-timeout-ms: 3000\n",
       {.source_name = "configured-autorelay-profile", .max_depth = 8});
   const auto* relay = parsed.value.try_get("relay");
   if (!parsed.ok() || !relay || !relay->is_object()) { throw std::runtime_error{"AutoRelay profile parse failed"}; }
   auto profile = *relay;
   (*profile.as_object())["client-enabled"] = value{!service};
   (*profile.as_object())["server-enabled"] = value{service};
   (*profile.as_object())["auto-discovery-enabled"] = value{!service};
   return profile;
}

void fresh_paths(const std::map<std::string, std::string>& args, bool service) {
   auto paths = std::set<std::filesystem::path>{};
   for (const auto* key : {"ready-file", "result-file", "stop-file", "store-dir", "seed-file"}) {
      if (service && std::string_view{key} == "seed-file") { continue; }
      const auto it = args.find(key);
      if (it == args.end() || it->second.empty()) { throw std::runtime_error{"missing AutoRelay artifact"}; }
      const auto path = std::filesystem::path{it->second};
      if (!path.is_absolute() || path != std::filesystem::weakly_canonical(path) || !paths.insert(path).second ||
          !std::filesystem::is_directory(path.parent_path()) || std::filesystem::is_symlink(std::filesystem::symlink_status(path))) {
         throw std::runtime_error{"AutoRelay artifact path must be unique canonical absolute non-symlink"};
      }
      if (!std::filesystem::exists(path)) { continue; }
      if (std::string_view{key} == "store-dir" && std::filesystem::is_directory(path) && std::filesystem::is_empty(path)) { continue; }
      if (std::string_view{key} == "seed-file" && read_seeds(path, false).empty()) { continue; }
      throw std::runtime_error{"AutoRelay artifacts/store must start fresh"};
   }
   for (const auto* key : {"ready-file", "result-file"}) {
      auto path = std::filesystem::path{args.at(key)};
      path += ".tmp";
      if (!paths.insert(path).second || std::filesystem::exists(path) ||
          std::filesystem::is_symlink(std::filesystem::symlink_status(path))) {
         throw std::runtime_error{"AutoRelay temporary artifact collision"};
      }
   }
   // No artifact may be inside the ObjectDB directory, even if the store does not exist yet.
   const auto store = std::filesystem::path{args.at("store-dir")};
   for (const auto& path : paths) {
      const auto relative = path.lexically_relative(store);
      if (path != store && !relative.empty() && *relative.begin() != "..") {
         throw std::runtime_error{"AutoRelay artifact inside store"};
      }
   }
}
} // namespace

void forge_plugin_interop_fixture::relay_state::append(forge::variants& destination, forge::variant item, std::size_t limit) {
   const auto encoded = forge::codec::json::write_value(item, {.max_bytes = item_limit});
   auto lock = std::lock_guard{mutex};
   if (overflow || destination.size() >= limit || !encoded.ok() || encoded.text.size() > capture_limit - capture_bytes) {
      overflow = true;
      throw std::runtime_error{"AutoRelay capture bound"};
   }
   capture_bytes += encoded.text.size();
   destination.emplace_back(std::move(item));
}

void forge_plugin_interop_fixture::relay_state::begin_echo() {
   auto lock = std::lock_guard{mutex};
   if (role != "destination" || admission_closed || overflow || pending_echoes >= 4 || admitted_echoes >= 16) {
      throw std::runtime_error{"AutoRelay echo admission closed or bounded"};
   }
   ++pending_echoes;
   ++admitted_echoes;
}

void forge_plugin_interop_fixture::relay_state::finish_echo() noexcept {
   auto lock = std::lock_guard{mutex};
   if (pending_echoes == 0) { overflow = true; } else { --pending_echoes; }
}

bool forge_plugin_interop_fixture::relay_state::failed() {
   auto lock = std::lock_guard{mutex};
   return overflow;
}

void forge_plugin_interop_fixture::configure_relay_document(forge::config::core::document& config) {
   const auto service = argument("command") == "autorelay-service";
   if ((!service && argument("command") != "autorelay-destination") || argument("transport") != "tcp" ||
       argument("scenario") != "autorelay") { throw std::runtime_error{"AutoRelay supports only explicit public TCP roles"}; }
   for (const auto* key : {"addr", "peer-id", "payload", "dns-server", "pnet-key-file", "pnet-fingerprint",
                          "pnet-control", "pnet-correlation", "features", "version", "actor", "case-token",
                          "control-file", "pubsub-policy"}) {
      if (_args.contains(key)) { throw std::runtime_error{"irrelevant AutoRelay argument"}; }
   }
   if (service && _args.contains("seed-file")) { throw std::runtime_error{"service cannot use seed admission"}; }
   fresh_paths(_args, service);
   _result_safe = true;
   _relay = std::make_shared<relay_state>();
   _relay->role = service ? "service" : "destination";
   _relay->seed_seen = !service && std::filesystem::exists(argument("seed-file"));
   const auto root = std::string{"plugins.net.p2p.node."};
   config.set(root + "relay", relay_profile(service));
   config.set(root + "path.policy", service ? "direct-only" : "relay-only");
   config.set(root + "pubsub.enabled", false);
   config.set(root + "peer-exchange.enabled", false);
   config.set(root + "mdns.enabled", false);
   for (const auto* role : {"client-v1-enabled", "client-v2-enabled", "service-v1-enabled", "service-v2-enabled", "ping-enabled"}) {
      config.set(root + "reachability." + role, false);
   }
   config.set(root + "diagnostics.max-peers", std::uint64_t{16});
   config.set(root + "diagnostics.max-sessions", std::uint64_t{32});
   config.set(root + "diagnostics.max-endpoints-per-peer", std::uint64_t{16});
   config.set(root + "diagnostics.max-protocols-per-peer", std::uint64_t{128});
   config.set(root + "diagnostics.max-relay-reservations-per-peer", std::uint64_t{4});
}

void forge_plugin_interop_fixture::start_relay() {
   _relay->started = std::chrono::steady_clock::now();
   _relay->next_sample = _relay->started + 100ms;
   _relay_diagnostics = api<np::diagnostics_api>();
   const auto events = api<np::events_api>();
   _relay_events.emplace(events->host_events());
   const auto initial = run(_relay_events->async_read(), "autorelay.events.initial");
   if (!initial || initial->generation != events->reachability_status().generation) {
      throw std::runtime_error{"AutoRelay host subscription initial state absent or inconsistent"};
   }
   _relay->host_events = object{}("source", "official-node.events_api.host_events")
       ("initial_consumed", true)("initial_generation", initial->generation);
   sample_relay("before_seed");
   if (_relay->role == "destination") {
      _echo.emplace(api<np::protocol_api>()->register_protocol({"/forge/interop/relay-echo/1"},
          [this](p2p::node::incoming_protocol_stream incoming) { return receive_relay(std::move(incoming)); }));
   }
}

forge::variant forge_plugin_interop_fixture::relay_observation(
    const p2p::diagnostics::snapshot& snapshot, std::int64_t elapsed, std::int64_t wall, std::string_view phase) {
   if (snapshot.peers.size() > 16 || snapshot.sessions.size() > 32 || snapshot.relay_reservations.size() > 16 ||
       snapshot.network.local_endpoints.size() > 16) { throw std::runtime_error{"AutoRelay diagnostics bounds"}; }
   auto addresses = forge::variants{};
   for (const auto& endpoint : snapshot.network.local_endpoints) { addresses.emplace_back(endpoint.to_string()); }
   auto leases = forge::variants{};
   for (const auto& lease : snapshot.relay_reservations) {
      if (lease.relay_endpoints.size() > 16) { throw std::runtime_error{"AutoRelay lease endpoint bound"}; }
      auto endpoints = forge::variants{};
      for (const auto& endpoint : lease.relay_endpoints) { endpoints.emplace_back(endpoint.to_string()); }
      auto remote = forge::variant{};
      if (lease.remote_limit) {
         remote = object{}("duration_seconds", lease.remote_limit->duration.count())("data_bytes", lease.remote_limit->data);
      }
      leases.emplace_back(object{}("relay_peer_id", lease.relay_peer.to_string())("id", lease.id)
          ("expires_unix_ms", std::chrono::duration_cast<std::chrono::milliseconds>(lease.expires_at).count())
          ("ttl_ms", lease.ttl.count())("max_streams", static_cast<std::uint64_t>(lease.max_streams))
          ("max_bytes", lease.max_bytes)("max_queued_bytes", static_cast<std::uint64_t>(lease.max_queued_bytes))
          ("voucher_present", lease.voucher.has_value())("remote_limit", remote)("endpoints", endpoints));
   }
   auto sessions = forge::variants{};
   for (const auto& session : snapshot.sessions) {
      auto facts = object{}("connection_id", session.id)("peer_id", session.remote_peer.to_string())
          ("identified", session.identify_state == p2p::identify::state::identified)
          ("identify_state", identify_name(session.identify_state))
          ("direct", session.path == p2p::path::kind::direct)("closed", session.closed)
          ("authentication", authentication_name(session.authentication))("muxer", session.muxer.value);
      if (session.path == p2p::path::kind::direct) {
         if (session.local_endpoint) { facts("local_address", session.local_endpoint->to_string()); }
         if (session.remote_endpoint) { facts("remote_address", session.remote_endpoint->to_string()); }
      } else {
         if (session.relay_peer) { facts("relay_peer_id", session.relay_peer->to_string()); }
         if (session.circuit_endpoint) { facts("circuit_address", session.circuit_endpoint->to_string()); }
         if (session.carrier_session_id) { facts("carrier_session_id", *session.carrier_session_id); }
      }
      sessions.emplace_back(std::move(facts));
   }
   auto peers = forge::variants{};
   for (const auto& peer : snapshot.peers) {
      if (peer.protocols.size() > 128) { throw std::runtime_error{"AutoRelay protocol bound"}; }
      auto protocols = forge::variants{};
      for (const auto& protocol : peer.protocols) { protocols.emplace_back(protocol.value); }
      peers.emplace_back(object{}("peer_id", peer.peer.to_string())("protocols", protocols));
   }
   const auto& manager = snapshot.autorelay;
   auto autorelay = object{}("enabled", manager.enabled)("running", manager.running)("permitted", manager.permitted)
       ("stopping", manager.stopping)("candidates", static_cast<std::uint64_t>(manager.candidates))
       ("pending_reservations", static_cast<std::uint64_t>(manager.pending_reservations))
       ("reservations", static_cast<std::uint64_t>(manager.reservations))
       ("automatic_reservations", static_cast<std::uint64_t>(manager.automatic_reservations))
       ("waiting_refreshes", static_cast<std::uint64_t>(manager.waiting_refreshes))
       ("max_candidates", static_cast<std::uint64_t>(manager.max_candidates))
       ("max_parallel_reservations", static_cast<std::uint64_t>(manager.max_parallel_reservations))
       ("target_reservations", static_cast<std::uint64_t>(manager.target_reservations))
       ("refreshes", manager.refreshes)("attempts", manager.attempts)("successes", manager.successes)
       ("failures", manager.failures)("renewals", manager.renewals)("invalidated_completions", manager.invalidated_completions);
   return object{}("elapsed_ms", elapsed)("unix_ms", wall)("phase", std::string{phase})
       ("reachability", reachability_name(snapshot.reachability.host.effective))("addresses", addresses)
       ("reservations", leases)("autorelay", autorelay)("sessions", sessions)("peers", peers)
       ("relay_bytes", snapshot.metrics.relay_bytes)
       ("service_reservations", static_cast<std::uint64_t>(snapshot.metrics.active_relay_reservations))
       ("discovery_attempts", snapshot.metrics.relay_discovery_attempts)("stopped", snapshot.metrics.stopped);
}

void forge_plugin_interop_fixture::sample_relay(std::string_view phase) {
   const auto snapshot = _relay_diagnostics->snapshot(bounded_snapshot());
   const auto observation = relay_observation(snapshot,
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - _relay->started).count(), unix_ms(), phase);
   if (phase == "before_seed" && (!snapshot.relay_reservations.empty() ||
       std::any_of(snapshot.network.local_endpoints.begin(), snapshot.network.local_endpoints.end(),
                   [](const auto& endpoint) { return endpoint.relayed.has_value(); }))) {
      throw std::runtime_error{"AutoRelay fresh fixture already has outbound relay ownership"};
   }
   _relay->append(_relay->observations, observation, observation_limit);
}

bool forge_plugin_interop_fixture::poll_relay() {
   auto changed = false;
   if (_relay->role == "destination") {
      const auto next = read_seeds(argument("seed-file"), !_relay->seed_seen);
      _relay->seed_seen = _relay->seed_seen || std::filesystem::exists(argument("seed-file"));
      auto offset = _relay->seed_offset;
      const auto admitted = seeds(_relay->seed_prefix, next, offset, _relay->connected_seeds, false);
      _relay->seed_prefix = next;
      _relay->seed_offset = offset;
      for (const auto& endpoint : admitted) {
         const auto connected = run(api<np::api>()->connect(endpoint.to_multiaddr(),
             {.expected_peer = endpoint.peer, .allow_relay = false, .timeout = 4s, .allow_hole_punch = false}), "autorelay.seed.connect");
         if (connected.remote_peer != *endpoint.peer || connected.path != p2p::path::kind::direct) {
            throw std::runtime_error{"AutoRelay seed connect identity/path mismatch"};
         }
         const auto facts = run(relay_seed_receipt(connected), "autorelay.seed.session-receipt");
         _relay->append(_relay->seed_connections, object{}("source", "official-node.api.connect")
             ("dial_address", endpoint.to_string())("expected_peer_id", endpoint.peer->to_string())
             ("session", facts)("unix_ms", unix_ms()), 4);
         _relay->connected_seeds.insert(endpoint.to_string());
         changed = true;
      }
   }
   if (std::chrono::steady_clock::now() >= _relay->next_sample) {
      sample_relay("running");
      _relay->next_sample = std::chrono::steady_clock::now() + 100ms;
      changed = true;
   }
   return changed;
}

boost::asio::awaitable<forge::variant> forge_plugin_interop_fixture::relay_seed_receipt(p2p::node::session_info connected) {
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   const auto deadline = std::chrono::steady_clock::now() + 4s;
   while (std::chrono::steady_clock::now() < deadline) {
      const auto snapshot = _relay_diagnostics->snapshot(bounded_snapshot());
      for (const auto& session : snapshot.sessions) {
         if (session.id != connected.id) { continue; }
         if (session.closed || session.remote_peer != connected.remote_peer || session.path != connected.path ||
             session.muxer != connected.muxer || session.direction != p2p::diagnostics::session_direction::outbound) {
            throw std::runtime_error{"AutoRelay seed retained session disagrees with connect"};
         }
         if (session.identify_state == p2p::identify::state::failed) { throw std::runtime_error{"AutoRelay seed Identify failed"}; }
         if (session.identify_state == p2p::identify::state::identified) { co_return direct_facts(session, snapshot.network.local_peer); }
      }
      timer.expires_after(5ms);
      co_await timer.async_wait(boost::asio::use_awaitable);
   }
   throw std::runtime_error{"AutoRelay seed Identify deadline"};
}

forge::variant forge_plugin_interop_fixture::relay_session_facts(
    const p2p::diagnostics::snapshot& snapshot, const p2p::node::session_info& incoming, p2p::peer_authentication stream_auth) {
   const auto& local = snapshot.network.local_peer;
   const auto found = std::find_if(snapshot.sessions.begin(), snapshot.sessions.end(),
                                 [&](const auto& session) { return session.id == incoming.id; });
   if (!p2p::valid_peer_id(local) || !p2p::valid_peer_id(incoming.remote_peer) || incoming.remote_peer == local ||
       incoming.id == 0 || found == snapshot.sessions.end()) { throw std::runtime_error{"AutoRelay inner session missing"}; }
   const auto& session = *found;
   if (session.closed || session.remote_peer != incoming.remote_peer || incoming.path != p2p::path::kind::relay ||
       session.path != incoming.path || session.direction != p2p::diagnostics::session_direction::inbound ||
       session.authentication != p2p::peer_authentication::noise || stream_auth != session.authentication ||
       session.muxer.value != "/yamux/1.0.0" || session.muxer != incoming.muxer ||
       session.identify_state != p2p::identify::state::identified || session.local_endpoint || session.remote_endpoint ||
       !session.relay_peer || session.relay_peer != incoming.relay_peer || !p2p::valid_peer_id(*session.relay_peer) ||
       *session.relay_peer == local || *session.relay_peer == session.remote_peer ||
       !session.carrier_session_id || session.carrier_session_id != incoming.carrier_session_id ||
       *session.carrier_session_id == 0 || *session.carrier_session_id == session.id ||
       !session.circuit_endpoint || !incoming.circuit_endpoint ||
       session.circuit_endpoint->to_string() != incoming.circuit_endpoint->to_string()) {
      throw std::runtime_error{"AutoRelay inner stream/session authentication or ownership mismatch"};
   }
   const auto& route = *session.circuit_endpoint;
   if (!route.relayed || route.peer != session.relay_peer || route.relayed->target != session.remote_peer ||
       route.transport.protocol != p2p::endpoint::protocol_kind::tcp || route.transport.port == 0 ||
       (route.transport.host_type != p2p::endpoint::host_kind::ip4 && route.transport.host_type != p2p::endpoint::host_kind::ip6)) {
      throw std::runtime_error{"AutoRelay logical circuit does not match authenticated identities"};
   }
   const auto carrier = std::find_if(snapshot.sessions.begin(), snapshot.sessions.end(),
       [&](const auto& outer) { return outer.id == *session.carrier_session_id; });
   if (carrier == snapshot.sessions.end() || carrier->remote_peer != *session.relay_peer) {
      throw std::runtime_error{"AutoRelay exact native carrier not retained"};
   }
   const auto physical = direct_facts(*carrier, local);
   auto route_socket = route;
   route_socket.relayed.reset();
   route_socket.peer.reset();
   auto remote_socket = *carrier->remote_endpoint;
   remote_socket.peer.reset();
   if (route_socket.to_string() != remote_socket.to_string()) {
      throw std::runtime_error{"AutoRelay logical carrier route disagrees with actual outer socket"};
   }
   return object{}("source", "official-node.protocol-handler-and-diagnostics_api")("session_id", session.id)
       ("local_peer_id", local.to_string())("remote_peer_id", session.remote_peer.to_string())
       ("authentication", authentication_name(session.authentication))("stream_authentication", authentication_name(stream_auth))
       ("authentication_basis", "native_inner_session_upgrade_output")("muxer", session.muxer.value)
       ("identify_state", identify_name(session.identify_state))("identified", true)("closed", session.closed)
       ("path", "relay")("direction", "inbound")("relay_peer_id", session.relay_peer->to_string())
       ("circuit_address", route.to_string())("carrier_session_id", *session.carrier_session_id)("carrier", physical);
}

boost::asio::awaitable<forge::variant> forge_plugin_interop_fixture::relay_session_receipt(
    p2p::node::session_info incoming, p2p::peer_authentication auth) {
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   const auto deadline = std::chrono::steady_clock::now() + 4s;
   while (std::chrono::steady_clock::now() < deadline) {
      const auto snapshot = _relay_diagnostics->snapshot(bounded_snapshot());
      const auto session = std::find_if(snapshot.sessions.begin(), snapshot.sessions.end(),
                                       [&](const auto& value) { return value.id == incoming.id; });
      if (session != snapshot.sessions.end()) {
         if (session->closed || session->identify_state == p2p::identify::state::failed) {
            throw std::runtime_error{"AutoRelay inner session closed or Identify failed"};
         }
         if (session->identify_state == p2p::identify::state::identified) {
            const auto outer = std::find_if(snapshot.sessions.begin(), snapshot.sessions.end(),
                [&](const auto& value) { return session->carrier_session_id && value.id == *session->carrier_session_id; });
            if (outer != snapshot.sessions.end() && outer->identify_state == p2p::identify::state::identified) {
               co_return relay_session_facts(snapshot, incoming, auth);
            }
         }
      }
      timer.expires_after(5ms);
      co_await timer.async_wait(boost::asio::use_awaitable);
   }
   throw std::runtime_error{"AutoRelay inner/carrier receipt deadline"};
}

boost::asio::awaitable<void> forge_plugin_interop_fixture::receive_relay(p2p::node::incoming_protocol_stream incoming) {
   auto phase = std::string{"autorelay.echo.admission"};
   try {
      const auto state = _relay;
      state->begin_echo();
      const auto finish = [state](void*) noexcept { state->finish_echo(); };
      const auto pending = std::unique_ptr<void, decltype(finish)>{state.get(), finish};
      if (incoming.protocol.value != "/forge/interop/relay-echo/1") { throw std::runtime_error{"unexpected AutoRelay echo protocol"}; }
      phase = "autorelay.echo.request-read";
      const auto bytes = co_await read_frame(incoming.stream);
      if (bytes.empty() || bytes.size() > 4096) { throw std::runtime_error{"AutoRelay echo payload bound"}; }
      phase = "autorelay.echo.session-receipt";
      const auto session = co_await relay_session_receipt(incoming.session, incoming.stream.authentication());
      phase = "autorelay.echo.response-write";
      co_await incoming.stream.async_write(frame(bytes));
      phase = "autorelay.echo.stream-close";
      co_await incoming.stream.async_close();
      phase = "autorelay.echo.capture";
      state->append(state->echoes, object{}("source", "official-node.protocol-handler")
          ("protocol", incoming.protocol.value)("session", session)("request", payload_receipt(bytes))
          ("response", payload_receipt(bytes))("response_write_completed", true)("stream_closed", true)("unix_ms", unix_ms()), 16);
   } catch (...) {
      incoming.stream.request_cancel();
      auto lock = std::lock_guard{_mutex};
      _errors.emplace_back(current_failure(phase));
      throw;
   }
}

void forge_plugin_interop_fixture::prepare_relay() {
   { auto lock = std::lock_guard{_relay->mutex}; _relay->admission_closed = true; }
   if (_relay->role == "destination") {
      const auto next = read_seeds(argument("seed-file"), !_relay->seed_seen);
      auto offset = _relay->seed_offset;
      static_cast<void>(seeds(_relay->seed_prefix, next, offset, _relay->connected_seeds, true));
      _relay->seed_prefix = next;
      _relay->seed_offset = offset;
   }
   sample_relay("pre_stop");
}

void forge_plugin_interop_fixture::verify_relay_detached() {
   auto rejected = false;
   try { static_cast<void>(_relay_diagnostics->snapshot()); }
   catch (const np::exceptions::plugin_not_initialized&) {
      auto lock = std::lock_guard{_relay->mutex};
      _relay->diagnostics_after_detach = object{current_failure("autorelay.post-join.diagnostics-detached")}
          ("source", "retained-official-node.diagnostics_api.snapshot")("rejected", true)
          ("after_plugins_joined", true)("after_scheduler_joined", true)("native_snapshot_read", false);
      rejected = true;
   }
   if (!rejected) { throw std::runtime_error{"AutoRelay diagnostics remained callable after detach"}; }
   if (!_relay_events || _relay_events->active()) { throw std::runtime_error{"AutoRelay subscription active after node join"}; }
   const auto late = run(_relay_events->async_read(), "autorelay.post-join.host-event-read");
   if (late || _relay_events->active()) { throw std::runtime_error{"AutoRelay subscription produced event after node join"}; }
   auto lock = std::lock_guard{_relay->mutex};
   _relay->host_events = object{_relay->host_events}("active_after_join", false)("read_after_join", forge::variant{})
       ("after_plugins_joined", true)("after_scheduler_joined", true);
   _relay->shutdown_observed = true;
}

bool forge_plugin_interop_fixture::relay_failed() const {
   if (!_relay) { return false; }
   auto lock = std::lock_guard{_relay->mutex};
   return _relay->overflow || (_plugins_joined && _relay->pending_echoes != 0);
}

forge::variant forge_plugin_interop_fixture::relay_receipt(bool final) const {
   if (!_relay) { return {}; }
   auto lock = std::lock_guard{_relay->mutex};
   const auto complete = final && _plugins_joined && _scheduler_joined && _relay->shutdown_observed &&
       _relay->admission_closed && _relay->pending_echoes == 0 && !_relay->overflow;
   return object{}("role", _relay->role)("peer_id", _local_peer)("transport", "tcp")("scenario", "autorelay")
       ("reservations_basis", "diagnostics.snapshot.relay_reservations")
       ("operation_basis", "official_shell_startup_authenticated_connect_only")
       ("operation_source", "official-shell.startup-and-node.api.connect")
       ("complete", complete)("error", _relay->overflow ? forge::variant{"autorelay_capture_failed"} : forge::variant{})
       ("observations", _relay->observations)("seed_connections", _relay->seed_connections)("echoes", _relay->echoes)
       ("pending_echoes", static_cast<std::uint64_t>(_relay->pending_echoes))
       ("admitted_echoes", static_cast<std::uint64_t>(_relay->admitted_echoes))
       ("echo_admission_closed", _relay->admission_closed)("overflow", _relay->overflow)
       ("diagnostics_after_detach", _relay->diagnostics_after_detach)("host_events", _relay->host_events)
       ("observation_scope", "after-official-startup-before-seeds-through-pre-stop; no-native-post-stop-snapshot");
}

int forge_plugin_interop_fixture::relay_self_test() {
   auto count = 0u;
   const auto check = [&count](bool condition) {
      if (!condition) { throw std::runtime_error{"configured AutoRelay self-test assertion"}; }
      ++count;
   };
   const auto reject = [&count](auto operation) {
      auto rejected = false;
      try { operation(); } catch (...) { rejected = true; }
      if (!rejected) { throw std::runtime_error{"configured AutoRelay self-test missing rejection"}; }
      ++count;
   };
   const auto local = p2p::peer_id::from_string("12D3KooWBda3fFGfHQnbNMo9XYqmKkMrw6ChD64LHwzw1ue3Ysri");
   const auto remote = p2p::peer_id::from_string("12D3KooWEEpgMh1ErSouV5AiUaJPUuFzatJopeMGk3JgdtFpiLjx");
   auto bytes = remote.to_bytes();
   bytes.back() ^= 1;
   const auto relay_bytes = p2p::peer_id::from_bytes(bytes);
   check(p2p::valid_peer_id(relay_bytes));
   const auto line = std::string{"/ip4/127.0.0.1/tcp/41002/p2p/"} + relay_bytes.to_string();
   auto offset = std::size_t{};
   check(seeds({}, line.substr(0, 20), offset, {}, false).empty() && offset == 0);
   check(seeds(line.substr(0, 20), line + '\n', offset, {}, false).size() == 1 && offset == line.size() + 1);
   const auto connected = std::set<std::string>{line};
   check(seeds(line + '\n', line + '\n' + line + '\n', offset, connected, false).empty());
   const auto parse_bad = [&](std::string input, bool stopping = false) {
      reject([&] {
         auto cursor = std::size_t{};
         static_cast<void>(seeds({}, input, cursor, {}, stopping));
      });
   };
   parse_bad(line, true);
   parse_bad(line + '\n', true);
   parse_bad("\n");
   parse_bad(std::string(513, 'x'));
   parse_bad(std::string(seed_limit + 1, 'x'));
   parse_bad("/ip4/127.0.0.1/tcp/0/p2p/" + relay_bytes.to_string() + '\n');
   parse_bad("/ip4/127.0.0.2/tcp/41002/p2p/" + relay_bytes.to_string() + '\n');
   parse_bad("/dns4/localhost/tcp/41002/p2p/" + relay_bytes.to_string() + '\n');
   parse_bad("/ip4/127.0.0.1/udp/41002/quic-v1/p2p/" + relay_bytes.to_string() + '\n');
   parse_bad("/ip4/127.0.0.1/tcp/41002\n");
   parse_bad(line + "/p2p-circuit/p2p/" + remote.to_string() + '\n');
   auto five = std::string{};
   for (auto port = 41002; port != 41007; ++port) {
      five += "/ip4/127.0.0.1/tcp/" + std::to_string(port) + "/p2p/" + relay_bytes.to_string() + '\n';
   }
   parse_bad(five);
   reject([&] {
      auto cursor = std::size_t{0};
      static_cast<void>(seeds(line + '\n', {}, cursor, {}, false));
   });
   auto atomic_cursor = std::size_t{0};
   reject([&] {
      static_cast<void>(seeds({}, line + "\ninvalid\n", atomic_cursor, {}, false));
   });
   check(atomic_cursor == 0);
   for (const auto service : {false, true}) {
      const auto profile = relay_profile(service);
      const auto& fields = *profile.as_object();
      check(std::get<bool>(fields.at("client-enabled").storage) == !service);
      check(std::get<bool>(fields.at("server-enabled").storage) == service);
      check(std::get<bool>(fields.at("auto-discovery-enabled").storage) == !service);
      check(!fields.contains("require-reservation"));
   }
   auto snapshot = p2p::diagnostics::snapshot{};
   snapshot.network.local_peer = local;
   auto carrier = p2p::diagnostics::session{};
   carrier.id = 1;
   carrier.remote_peer = relay_bytes;
   carrier.authentication = p2p::peer_authentication::noise;
   carrier.muxer = {"/yamux/1.0.0"};
   carrier.identify_state = p2p::identify::state::identified;
   carrier.local_endpoint = p2p::parse_endpoint("/ip4/127.0.0.1/tcp/41001");
   carrier.remote_endpoint = p2p::parse_endpoint(line);
   auto inner = p2p::diagnostics::session{};
   inner.id = 2;
   inner.remote_peer = remote;
   inner.authentication = p2p::peer_authentication::noise;
   inner.muxer = carrier.muxer;
   inner.path = p2p::path::kind::relay;
   inner.direction = p2p::diagnostics::session_direction::inbound;
   inner.identify_state = p2p::identify::state::identified;
   inner.relay_peer = relay_bytes;
   inner.carrier_session_id = carrier.id;
   inner.circuit_endpoint = p2p::parse_endpoint(line + "/p2p-circuit/p2p/" + remote.to_string());
   snapshot.sessions = {carrier, inner};
   const auto incoming = p2p::node::session_info{.remote_peer = remote, .path = inner.path,
       .relay_peer = relay_bytes, .muxer = inner.muxer, .circuit_endpoint = inner.circuit_endpoint,
       .carrier_session_id = carrier.id, .id = inner.id};
   const auto facts = relay_session_facts(snapshot, incoming, p2p::peer_authentication::noise);
   check(facts["carrier_session_id"].as_uint64() == 1 && facts["session_id"].as_uint64() == 2);
   check(!facts.get_object().contains("local_address") && !facts.get_object().contains("remote_address"));
   check(facts["carrier"]["remote_address"].get_string() == line);
   const auto bad_snapshot = [&](auto mutate) {
      reject([&] {
         auto invalid = snapshot;
         mutate(invalid);
         static_cast<void>(relay_session_facts(invalid, incoming, p2p::peer_authentication::noise));
      });
   };
   bad_snapshot([](auto& invalid) { invalid.sessions.pop_back(); });
   bad_snapshot([](auto& invalid) { invalid.sessions.front().closed = true; });
   bad_snapshot([](auto& invalid) { invalid.sessions.back().closed = true; });
   bad_snapshot([](auto& invalid) { invalid.sessions.front().remote_endpoint.reset(); });
   bad_snapshot([](auto& invalid) { invalid.sessions.front().local_endpoint->transport.port = 0; });
   bad_snapshot([&](auto& invalid) { invalid.sessions.front().remote_peer = remote; });
   bad_snapshot([](auto& invalid) { invalid.sessions.front().authentication = p2p::peer_authentication::unverified; });
   bad_snapshot([](auto& invalid) { invalid.sessions.back().authentication = p2p::peer_authentication::libp2p_tls; });
   bad_snapshot([](auto& invalid) { invalid.sessions.back().identify_state = p2p::identify::state::failed; });
   bad_snapshot([](auto& invalid) { invalid.sessions.front().identify_state = p2p::identify::state::unknown; });
   bad_snapshot([](auto& invalid) { invalid.sessions.back().muxer = {"/other/1"}; });
   bad_snapshot([](auto& invalid) { invalid.sessions.back().carrier_session_id = 2; });
   bad_snapshot([](auto& invalid) { invalid.sessions.back().path = p2p::path::kind::direct; });
   bad_snapshot([&](auto& invalid) { invalid.sessions.back().remote_endpoint = carrier.remote_endpoint; });
   bad_snapshot([&](auto& invalid) { invalid.sessions.back().local_endpoint = carrier.local_endpoint; });
   bad_snapshot([&](auto& invalid) { invalid.sessions.back().circuit_endpoint->relayed->target = local; });
   bad_snapshot([](auto& invalid) { ++invalid.sessions.back().circuit_endpoint->transport.port; });
   bad_snapshot([](auto& invalid) { invalid.sessions.front().path = p2p::path::kind::relay; });
   bad_snapshot([&](auto& invalid) { invalid.sessions.back().relay_peer = remote; });
   reject([&] { static_cast<void>(relay_session_facts(snapshot, incoming, p2p::peer_authentication::unverified)); });
   auto observation = relay_observation(snapshot, 123, 456, "pre_stop");
   snapshot.metrics.relay_bytes = 99;
   check(observation["relay_bytes"].as_uint64() == 0 && observation["phase"].get_string() == "pre_stop");
   auto state = relay_state{};
   state.role = "destination";
   state.append(state.observations, observation, observation_limit);
   state.begin_echo();
   check(state.pending_echoes == 1 && state.admitted_echoes == 1 && !state.failed());
   state.finish_echo();
   state.admission_closed = true;
   reject([&] { state.begin_echo(); });
   check(state.pending_echoes == 0);
   state.capture_bytes = capture_limit;
   reject([&] { state.append(state.observations, observation, observation_limit); });
   check(state.failed());
   // The literal/string overloads used by startup replace existing fields;
   // unlike the variant append overload, they must not create duplicate keys.
   auto envelope = object{}("source", "official-shell.configuration-document")("role", "listener");
   envelope = object{envelope}("source", "official-shell.accepted-document");
   envelope("role", std::string{"destination"});
   const auto envelope_json = forge::codec::json::write_value(envelope);
   if (!envelope_json.ok()) { throw std::runtime_error{"AutoRelay envelope encoding failed"}; }
   const auto envelope_read = forge::codec::json::read_value(envelope_json.text);
   check(envelope.size() == 2 && envelope_read.ok() && envelope_read.value.get_object().size() == 2 &&
       envelope_read.value["source"].get_string() == "official-shell.accepted-document" &&
       envelope_read.value["role"].get_string() == "destination");
   for (const auto error : {"pubsub_fixture_operation_failed", "autorelay_fixture_operation_failed"}) {
      auto result = object{}("error", forge::variant{})("complete", true)("status", "ok");
      publish_failure(result, error);
      const auto encoded = forge::codec::json::write_value(result);
      if (!encoded.ok()) { throw std::runtime_error{"failure receipt encoding failed"}; }
      const auto parsed = forge::codec::json::read_value(encoded.text);
      check(parsed.ok() && parsed.value.get_object().size() == 3 && parsed.value["error"].get_string() == error &&
          !parsed.value["complete"].as_bool() && parsed.value["status"].get_string() == "error");
      for (const auto key : {"error", "complete", "status"}) {
         auto matches = 0u;
         for (const auto& field : result) { if (field.key() == key) { ++matches; } }
         check(matches == 1);
      }
   }
   std::cout << "configured AutoRelay fixture self-test: " << count << " checks\n";
   return 0;
}

} // namespace forge::test::libp2p_interop
