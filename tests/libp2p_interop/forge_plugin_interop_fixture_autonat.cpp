#include "forge_plugin_interop_fixture.hxx"

#include <boost/asio/ip/address.hpp>

namespace forge::test::libp2p_interop {
namespace {
namespace np = forge::plugins::net::p2p::node;
namespace p2p = forge::net::p2p;
using object = forge::mutable_variant_object;
using value = forge::config::core::value;
using namespace std::chrono_literals;

std::string state_name(p2p::reachability::state state) {
   switch (state) {
   case p2p::reachability::state::unknown: return "unknown";
   case p2p::reachability::state::publicly_reachable: return "public";
   case p2p::reachability::state::private_network: return "private";
   case p2p::reachability::state::blocked: return "blocked";
   case p2p::reachability::state::relay_only: return "relay_only";
   }
   throw std::runtime_error{"invalid native AutoNAT state"};
}

std::string phase_name(p2p::lifecycle_phase phase) {
   switch (phase) {
   case p2p::lifecycle_phase::idle: return "idle";
   case p2p::lifecycle_phase::hydrating: return "hydrating";
   case p2p::lifecycle_phase::listening: return "listening";
   case p2p::lifecycle_phase::bootstrapping: return "bootstrapping";
   case p2p::lifecycle_phase::maintenance: return "maintenance";
   case p2p::lifecycle_phase::stopping: return "stopping";
   case p2p::lifecycle_phase::stopped: return "stopped";
   }
   throw std::runtime_error{"invalid native lifecycle phase"};
}

p2p::endpoint normalized(p2p::endpoint endpoint) {
   endpoint.peer.reset();
   return endpoint;
}

bool same_address(const p2p::endpoint& left, const p2p::endpoint& right) {
   return normalized(left).to_string() == normalized(right).to_string();
}

bool isolated_autonat_tcp(const p2p::endpoint& endpoint, bool allow_zero = false) {
   // Closed test lane from isolated_network.py, not a generic public-routability classifier.
   // Zero is allowed only for pre-start bind/probe requests, never actual sockets.
   return endpoint.is_direct_tcp() && endpoint.transport.host_type == p2p::endpoint::host_kind::ip4 &&
       endpoint.transport.zone.empty() && (allow_zero || endpoint.transport.port != 0) &&
       (endpoint.transport.host == "11.0.0.1" || endpoint.transport.host == "11.0.0.2" ||
        endpoint.transport.host == "11.0.0.3");
}

std::string protocol(std::uint32_t version) {
   return version == 1 ? p2p::builtins::autonat_v1.value : p2p::builtins::autonat_v2_dial_request.value;
}

p2p::diagnostics::options snapshot_bounds() {
   return {.max_peers = 16, .max_sessions = 32, .max_endpoints_per_peer = 16,
           .max_protocols_per_peer = 128, .max_relay_reservations_per_peer = 4, .max_dht_profiles = 4};
}

p2p::endpoint actual_listener(const std::vector<p2p::endpoint>& endpoints, const p2p::endpoint& requested) {
   auto found = std::optional<p2p::endpoint>{};
   for (const auto& endpoint : endpoints) {
      if (endpoint.transport.host_type != requested.transport.host_type ||
          endpoint.transport.protocol != requested.transport.protocol ||
          endpoint.transport.host != requested.transport.host || !isolated_autonat_tcp(endpoint) ||
          (requested.transport.port != 0 && endpoint.transport.port != requested.transport.port)) { continue; }
      if (found && !same_address(*found, endpoint)) {
         throw std::runtime_error{"AutoNAT requested candidate has multiple actual listeners"};
      }
      found = normalized(endpoint);
   }
   if (!found) { throw std::runtime_error{"AutoNAT requested candidate has no actual listener"}; }
   return *found;
}

bool inbound_to(const forge::variant& connections, const p2p::endpoint& target, bool secured_allowed) {
   for (const auto& connection : connections.get_array()) {
      const auto& fields = connection.get_object();
      if (fields["direction"].as_string() != "inbound" || !fields["authenticated"].as_bool() ||
          (fields["stage"].as_string() != "upgraded" &&
           !(secured_allowed && fields["stage"].as_string() == "secured"))) { continue; }
      if (same_address(p2p::parse_endpoint(fields["local_addr"].as_string()), target)) { return true; }
   }
   return false;
}
} // namespace

void forge_plugin_interop_fixture::configure_autonat_document(forge::config::core::document& config) {
   const auto scenario = argument("scenario");
   if ((scenario != "autonat_v1" && scenario != "autonat_v2") || argument("transport") != "tcp" ||
       (argument("command") != "listen" && argument("command") != "dial")) {
      throw std::runtime_error{"configured AutoNAT fixture supports public TCP Noise v1/v2 roles only"};
   }
   for (const auto* key : {"payload", "dns-server", "pnet-key-file", "pnet-fingerprint", "pnet-control",
                          "pnet-correlation", "features", "seed-file"}) {
      if (_args.contains(key)) { throw std::runtime_error{"irrelevant configured AutoNAT argument"}; }
   }
   if (argument("bind-ip").empty()) { throw std::runtime_error{"AutoNAT requires the isolated runner's bind IP"}; }
   auto error = boost::system::error_code{};
   const auto address = boost::asio::ip::make_address(argument("bind-ip"), error);
   if (error) { throw std::runtime_error{"AutoNAT bind IP must be numeric"}; }
   auto state = std::make_shared<autonat_state>();
   state->service = argument("command") == "listen";
   state->version = scenario == "autonat_v1" ? 1u : 2u;
   state->bind = {.transport = {.host_type = address.is_v4() ? p2p::endpoint::host_kind::ip4 : p2p::endpoint::host_kind::ip6,
                                .protocol = p2p::endpoint::protocol_kind::tcp, .host = address.to_string(), .port = 0}};
   if (!isolated_autonat_tcp(state->bind, true)) {
      throw std::runtime_error{"AutoNAT bind must belong to the closed isolated IPv4 fixture lane"};
   }
   state->probe = argument("probe-addr").empty() ? state->bind : p2p::parse_endpoint(argument("probe-addr"));
   if (!isolated_autonat_tcp(state->probe, true) || state->probe.peer ||
       (state->version == 1 && state->probe.transport.host != state->bind.transport.host) ||
       (state->service && !same_address(state->probe, state->bind))) {
      throw std::runtime_error{"AutoNAT probe must be an isolated IPv4 TCP candidate for this role/version"};
   }
   // v2 has one candidate, exactly as the raw CLI. Port 0 must become a declarative
   // pre-start value; this reservation is not a successful node listener or dialback.
   if (!state->service && !argument("probe-addr").empty() && state->probe.transport.port == 0) {
      const auto ip = boost::asio::ip::make_address(state->probe.transport.host);
      state->port_reservation = std::make_unique<boost::asio::ip::tcp::acceptor>(_shell->runtime().context());
      state->port_reservation->open(ip.is_v4() ? boost::asio::ip::tcp::v4() : boost::asio::ip::tcp::v6());
      state->port_reservation->bind({ip, 0});
      state->probe.transport.port = state->port_reservation->local_endpoint().port();
      state->reservation = object{}("source", "boost.asio.tcp.acceptor.local_endpoint")
          ("requested_address", argument("probe-addr"))("reserved_address", state->probe.to_string())
          ("released_before_startup", false)("node_listener_proof", false);
   }
   auto observation = make_autonat_connection_observer(_admission);
   state->gater = std::move(observation.first);
   state->connections = std::move(observation.second);
   const auto root = std::string{"plugins.net.p2p.node."};
   config.set(root + "listen", value::array_type{value{state->probe.to_string()}});
   if (state->probe.transport.port != 0) {
      config.set(root + "advertised-endpoints", value::array_type{value{state->probe.to_string()}});
   }
   config.set(root + "pubsub.enabled", false);
   config.set(root + "mdns.enabled", false);
   config.set(root + "peer-exchange.enabled", false);
   config.set(root + "reachability.client-v1-enabled", !state->service && state->version == 1);
   config.set(root + "reachability.client-v2-enabled", !state->service && state->version == 2);
   config.set(root + "reachability.service-v1-enabled", state->service && state->version == 1);
   config.set(root + "reachability.service-v2-enabled", state->service && state->version == 2);
   config.set(root + "reachability.ping-enabled", false);
   config.set(root + "diagnostics.max-peers", std::uint64_t{16});
   config.set(root + "diagnostics.max-sessions", std::uint64_t{32});
   config.set(root + "diagnostics.max-endpoints-per-peer", std::uint64_t{16});
   config.set(root + "diagnostics.max-protocols-per-peer", std::uint64_t{128});
   _autonat = std::move(state);
   _result_safe = true;
}

void forge_plugin_interop_fixture::release_autonat_port() {
   if (!_autonat->port_reservation) { return; }
   set_phase("autonat.pre-start.port-release");
   auto error = boost::system::error_code{};
   _autonat->port_reservation->close(error);
   if (error) { throw boost::system::system_error{error}; }
   _autonat->port_reservation.reset();
   _autonat->reservation = object{_autonat->reservation}("released_before_startup", true);
}

forge::variant forge_plugin_interop_fixture::autonat_host(const p2p::host_event& host) {
   if (host.autonat_v2.size() > 64) { throw std::runtime_error{"AutoNAT host address capture bound"}; }
   auto addresses = forge::variants{};
   for (const auto& item : host.autonat_v2) {
      addresses.emplace_back(object{}("address", normalized(item.address).to_string())("state", state_name(item.value)));
   }
   return object{}("generation", host.generation)("resync_required", host.resync_required)
       ("phase", phase_name(host.phase))("effective", state_name(host.effective))
       ("autonat_v1", state_name(host.autonat_v1))("autonat_v2", addresses);
}

forge::variant forge_plugin_interop_fixture::autonat_snapshot(const p2p::diagnostics::snapshot& snapshot) {
   const auto& state = snapshot.reachability;
   return object{}("source", "official-node.diagnostics_api.snapshot")
       ("lifecycle_phase", phase_name(snapshot.lifecycle.phase))
       ("active_sessions", static_cast<std::uint64_t>(snapshot.connections.active_sessions))
       ("resources", object{}("system_memory", static_cast<std::uint64_t>(snapshot.resources.system.memory))
           ("inbound_connections", static_cast<std::uint64_t>(snapshot.resources.connections.inbound_connections))
           ("outbound_connections", static_cast<std::uint64_t>(snapshot.resources.connections.outbound_connections))
           ("active_dials", static_cast<std::uint64_t>(snapshot.resources.active_dials))
           ("active_service_scopes", static_cast<std::uint64_t>(snapshot.resources.active_service_scopes)))
       ("reachability", object{}("host", autonat_host(state.host))
           ("client_v1_enabled", state.client_v1_enabled)("client_v2_enabled", state.client_v2_enabled)
           ("service_v1_enabled", state.service_v1_enabled)("service_v2_enabled", state.service_v2_enabled)
           ("internet_egress_allowed", state.internet_egress_allowed)
           ("pending_probes", static_cast<std::uint64_t>(state.pending_probes))
           ("waiting_probes", static_cast<std::uint64_t>(state.waiting_probes))
           ("active_handlers", static_cast<std::uint64_t>(state.active_handlers))("probe_errors", state.probe_errors))
       ("metrics", object{}("reachability_checks", snapshot.metrics.reachability_checks)
           ("reachability_public", snapshot.metrics.reachability_public)("reachability_private", snapshot.metrics.reachability_private));
}

void forge_plugin_interop_fixture::start_autonat() {
   set_phase("autonat.started.actual-listener");
   const auto endpoints = api<np::api>()->local_endpoints();
   const auto actual = actual_listener(endpoints, _autonat->probe);
   if (endpoints.size() != 1) { throw std::runtime_error{"AutoNAT requires exactly one actual candidate"}; }
   _autonat->probe = actual;
   _autonat->diagnostics = api<np::diagnostics_api>();
   _autonat->events.emplace(api<np::events_api>()->host_events());
   const auto initial = run(_autonat->events->async_read(), "autonat.events.initial");
   if (!initial || initial->resync_required) { throw std::runtime_error{"AutoNAT initial host event missing or resync required"}; }
   _autonat->host = *initial;
   _autonat->host_events = object{}("source", "official-node.events_api.host_events")
       ("initial_consumed", true)("initial_generation", initial->generation)("initial", autonat_host(*initial));
   const auto snapshot = _autonat->diagnostics->snapshot(snapshot_bounds());
   const auto& roles = snapshot.reachability;
   if (!roles.internet_egress_allowed || roles.client_v1_enabled != (!_autonat->service && _autonat->version == 1) ||
       roles.client_v2_enabled != (!_autonat->service && _autonat->version == 2) ||
       roles.service_v1_enabled != (_autonat->service && _autonat->version == 1) ||
       roles.service_v2_enabled != (_autonat->service && _autonat->version == 2) ||
       roles.pending_probes != 0 || roles.waiting_probes != 0 || snapshot.metrics.reachability_checks != 0) {
      throw std::runtime_error{"AutoNAT accepted roles or preconnect state disagree with actual native policy"};
   }
   _autonat->initial_probe_errors = roles.probe_errors;
   _autonat->initial_checks = snapshot.metrics.reachability_checks;
   _autonat->initial_public = snapshot.metrics.reachability_public;
   _autonat->initial_private = snapshot.metrics.reachability_private;
   _autonat->diagnostics_before_connect = autonat_snapshot(snapshot);
}

forge::variant forge_plugin_interop_fixture::autonat_connections() const {
   if (!_autonat || !_autonat->connections) { return {}; }
   const auto encoded = _autonat->connections->json();
   if (encoded.size() > 256 * 1024) { throw std::runtime_error{"AutoNAT actual connection JSON capture bound"}; }
   const auto decoded = forge::codec::json::read_value(encoded);
   if (!decoded.ok() || !decoded.value.is_array() || decoded.value.get_array().size() > 64) {
      throw std::runtime_error{"AutoNAT actual connection capture malformed"};
   }
   return decoded.value;
}

boost::asio::awaitable<forge::variant> forge_plugin_interop_fixture::autonat_control_receipt(p2p::node::session_info connected) {
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   const auto deadline = std::chrono::steady_clock::now() + 4s;
   while (std::chrono::steady_clock::now() < deadline) {
      const auto snapshot = _autonat->diagnostics->snapshot(snapshot_bounds());
      for (const auto& session : snapshot.sessions) {
         if (session.id != connected.id) { continue; }
         if (session.closed || session.remote_peer != connected.remote_peer || session.path != p2p::path::kind::direct ||
             session.relay_peer || session.carrier_session_id || session.circuit_endpoint ||
             session.authentication != p2p::peer_authentication::noise || session.muxer.value != "/yamux/1.0.0" ||
             session.direction != p2p::diagnostics::session_direction::outbound ||
             !session.local_endpoint || !session.remote_endpoint || !isolated_autonat_tcp(*session.local_endpoint) ||
             !isolated_autonat_tcp(*session.remote_endpoint) || session.identify_state == p2p::identify::state::failed) {
            throw std::runtime_error{"AutoNAT control lacks actual authenticated direct socket facts"};
         }
         if (session.identify_state != p2p::identify::state::identified) { continue; }
         co_return object{}("source", "official-node.api.connect-and-node.diagnostics_api.session")
             ("session_id", session.id)("local_peer_id", snapshot.network.local_peer.to_string())
             ("remote_peer_id", session.remote_peer.to_string())("path", "direct")("authentication", "noise")
             ("authentication_basis", "native_session_upgrade_output")("muxer", session.muxer.value)
             ("identify_state", "identified")("identified", true)("closed", session.closed)
             ("direction", "outbound")("local_address", normalized(*session.local_endpoint).to_string())
             ("remote_address", normalized(*session.remote_endpoint).to_string());
      }
      timer.expires_after(5ms);
      co_await timer.async_wait(boost::asio::use_awaitable);
   }
   throw std::runtime_error{"AutoNAT actual control Identify deadline"};
}

void forge_plugin_interop_fixture::dial_autonat() {
   set_phase("autonat.control.target");
   const auto observer = p2p::peer_id::from_string(argument("peer-id"));
   const auto remote = p2p::parse_endpoint(argument("addr"));
   if (!p2p::valid_peer_id(observer) || observer.to_string() == _local_peer || !isolated_autonat_tcp(remote) ||
       (remote.peer && *remote.peer != observer)) { throw std::runtime_error{"AutoNAT control target mismatch"}; }
   const auto connected = run(api<np::api>()->connect(remote.to_multiaddr(),
       {.expected_peer = observer, .allow_relay = false, .timeout = 5s, .allow_hole_punch = false}), "autonat.control.connect");
   if (connected.remote_peer != observer || connected.path != p2p::path::kind::direct) {
      throw std::runtime_error{"AutoNAT official connect did not authenticate the expected observer"};
   }
   _autonat->observer = observer.to_string();
   _autonat->control = run(autonat_control_receipt(connected), "autonat.control.Identify");
   set_phase("autonat.autonomous-exchange.observation");
   // Native refresh/cooldown/quorum are untouched. Observe completion, never trigger/retry a probe.
   const auto deadline = std::chrono::steady_clock::now() + 30s;
   while (std::chrono::steady_clock::now() < deadline) {
      if (autonat_failed()) { throw std::runtime_error{"AutoNAT connection capture failed"}; }
      const auto snapshot = _autonat->diagnostics->snapshot(snapshot_bounds());
      if (snapshot.reachability.probe_errors != _autonat->initial_probe_errors ||
          snapshot.metrics.reachability_private != _autonat->initial_private) {
         throw std::runtime_error{"AutoNAT autonomous exchange failed or returned non-public reachability"};
      }
      const auto host = api<np::events_api>()->reachability_status();
      const auto found = std::find_if(host.autonat_v2.begin(), host.autonat_v2.end(),
          [&](const auto& item) { return same_address(item.address, _autonat->probe); });
      const auto verified_v2 = found != host.autonat_v2.end() && found->value == p2p::reachability::state::publicly_reachable;
      if (snapshot.metrics.reachability_checks > _autonat->initial_checks &&
          snapshot.metrics.reachability_public > _autonat->initial_public &&
          snapshot.reachability.pending_probes == 0 && snapshot.reachability.waiting_probes == 0 &&
          inbound_to(autonat_connections(), _autonat->probe, _autonat->version == 1) &&
          (_autonat->version == 1 || verified_v2)) {
         if (_autonat->version == 1 && (host.autonat_v1 != p2p::reachability::state::unknown ||
                                       !host.autonat_v2.empty())) {
            throw std::runtime_error{"single-observer AutoNAT v1 must retain native default quorum/unknown state"};
         }
         _autonat->host = host;
         if (_autonat->version == 2) {
            _autonat->v2_address_state = object{}("address", normalized(found->address).to_string())
                ("state", state_name(found->value));
         }
         _autonat->reached = true;
         return;
      }
      std::this_thread::sleep_for(5ms);
   }
   throw std::runtime_error{"AutoNAT autonomous exchange lacked completed native/actual inbound evidence"};
}

void forge_plugin_interop_fixture::prepare_autonat() {
   if (!_autonat->diagnostics || !_autonat->events) {
      throw std::runtime_error{"AutoNAT observation setup incomplete"};
   }
   const auto snapshot = _autonat->diagnostics->snapshot(snapshot_bounds());
   _autonat->diagnostics_before_stop = autonat_snapshot(snapshot);
   _autonat->host = api<np::events_api>()->reachability_status();
   _autonat->host_events = object{_autonat->host_events}("before_stop", autonat_host(_autonat->host));
   static_cast<void>(autonat_connections());
}

void forge_plugin_interop_fixture::verify_autonat_detached() {
   if (!_autonat->diagnostics || !_autonat->events) {
      throw std::runtime_error{"AutoNAT retained observation ownership missing"};
   }
   auto rejected = false;
   try { static_cast<void>(_autonat->diagnostics->snapshot()); }
   catch (const np::exceptions::plugin_not_initialized&) {
      _autonat->diagnostics_after_detach = object{current_failure("autonat.post-join.diagnostics-detached")}
          ("source", "retained-official-node.diagnostics_api.snapshot")("rejected", true)
          ("after_plugins_joined", true)("after_scheduler_joined", true)("native_snapshot_read", false);
      rejected = true;
   }
   if (!rejected || !_autonat->events || _autonat->events->active()) {
      throw std::runtime_error{"AutoNAT retained API/subscription remained active after join"};
   }
   auto queued = forge::variants{};
   for (;;) {
      const auto event = run(_autonat->events->async_read(), "autonat.post-join.host-event-drain");
      if (!event) { break; }
      if (queued.size() == 64) { throw std::runtime_error{"AutoNAT joined host event capture bound"}; }
      queued.emplace_back(autonat_host(*event));
   }
   _autonat->host_events = object{_autonat->host_events}("active_after_join", false)
       ("queued_events_drained_after_join", queued)("read_after_join", forge::variant{})
       ("after_plugins_joined", true)("after_scheduler_joined", true);
   _autonat->detached = true;
}

bool forge_plugin_interop_fixture::autonat_failed() const {
   return _autonat && (!_autonat->connections || !_autonat->connections->complete());
}

void forge_plugin_interop_fixture::autonat_ready(object& ready) const {
   ready("scenario", argument("scenario"))("transport", "tcp")("version", _autonat->version)
       ("protocol", protocol(_autonat->version))("service_enabled", _autonat->service)
       ("accepted_configuration", _settings);
}

forge::variant forge_plugin_interop_fixture::autonat_receipt(bool final) const {
   if (!_autonat) { return {}; }
   const auto failed = autonat_failed();
   return object{}("version", _autonat->version)("protocol", protocol(_autonat->version))("transport", "tcp")
       ("protocol_basis", "configured_selected_protocol_not_negotiation_capture")
       ("operation_basis", "official_shell_startup_authenticated_connect_only")
       ("operation_source", "official-shell.startup-and-node.api.connect")
       ("observation_scope", "after-official-startup-before-connect-through-pre-stop; no-native-post-stop-snapshot")
       ("observer_peer_id", _autonat->observer.empty() ? forge::variant{} : forge::variant{_autonat->observer})
       ("control_authenticated_peer", _autonat->observer.empty() ? forge::variant{} : forge::variant{_autonat->observer})
       ("control_identify_completed", !_autonat->control.is_null())("control_session", _autonat->control)
       ("requested_addr", _autonat->service ? forge::variant{} : forge::variant{_autonat->probe.to_string()})
       ("response_addr", forge::variant{})("response_addr_proof", "not_exposed_by_public_API")
       ("nonce_proof", "not_exposed_by_public_API")("autonat_probe_api_calls", 0u)("v1_vote", forge::variant{})
       ("probe_state", _autonat->version == 2 && _autonat->reached ? "public" : "unknown")
       ("autonat_v1_state", state_name(_autonat->host.autonat_v1))
       ("effective_state", state_name(_autonat->host.effective))("effective_confidence", state_name(_autonat->host.effective))
       ("autonat_v2_address_state", _autonat->v2_address_state)("current_v2_address_state", _autonat->v2_address_state)
       ("reached", _autonat->reached)
       ("reached_basis", _autonat->service ? "not_observable_from_service_api" :
           "completed_native_exchange_and_actual_authenticated_inbound; requires_independent_donor_result")
       ("v2_fixture_candidate_count", _autonat->version == 2 && !_autonat->service ? 1u : 0u)
       ("service_enabled", _autonat->service)("policy_denied", false)("internet_egress", "")
       ("actual_connections", autonat_connections())("connection_gater_trace_complete", !failed)
       ("connection_gater_trace_failure", failed ? forge::variant{"autonat_actual_connection_capture_failed"} : forge::variant{})
       ("trace_complete", final && !failed)("hosts_closed", _plugins_joined)("handlers_joined", _plugins_joined)
       ("complete", final && !failed && _plugins_joined && _scheduler_joined && _autonat->detached)
       ("diagnostics_before_connect", _autonat->diagnostics_before_connect)
       ("diagnostics_before_stop", _autonat->diagnostics_before_stop)
       ("diagnostics_after_detach", _autonat->diagnostics_after_detach)("host_events", _autonat->host_events)
       ("probe_port_reservation", _autonat->reservation);
}

int forge_plugin_interop_fixture::autonat_self_test() {
   auto count = 0u;
   const auto check = [&count](bool condition) {
      if (!condition) { throw std::runtime_error{"configured AutoNAT address self-test assertion"}; }
      ++count;
   };
   const auto eligible = [](std::string_view input, bool allow_zero = false) {
      try { return isolated_autonat_tcp(p2p::parse_endpoint(input), allow_zero); }
      catch (const std::exception&) { return false; }
   };
   // Valid baselines precede the rejection matrix; no shell, listener or network is started.
   for (const auto* host : {"11.0.0.1", "11.0.0.2", "11.0.0.3"}) {
      const auto prefix = std::string{"/ip4/"} + host + "/tcp/";
      for (const auto port : {1, 65535}) {
         check(eligible(prefix + std::to_string(port)));
         check(eligible(prefix + std::to_string(port), true));
      }
      check(eligible(prefix + "0", true));
      check(!eligible(prefix + "0"));
   }
   for (const auto* input : {
            "/ip4/10.0.0.1/tcp/4001", "/ip4/172.16.0.1/tcp/4001", "/ip4/192.168.1.1/tcp/4001",
            "/ip4/169.254.1.1/tcp/4001", "/ip4/100.64.0.1/tcp/4001", "/ip4/0.0.0.0/tcp/4001",
            "/ip4/127.0.0.1/tcp/4001", "/ip4/192.0.0.1/tcp/4001", "/ip4/192.0.2.1/tcp/4001",
            "/ip4/198.18.0.1/tcp/4001", "/ip4/198.51.100.1/tcp/4001", "/ip4/203.0.113.1/tcp/4001",
            "/ip4/224.0.0.1/tcp/4001", "/ip4/240.0.0.1/tcp/4001", "/ip4/255.255.255.255/tcp/4001",
            "/ip4/8.8.8.8/tcp/4001", "/ip4/11.0.0.0/tcp/4001", "/ip4/11.0.0.4/tcp/4001",
            "/ip6/2001:4860:4860::8888/tcp/4001", "/ip6/::1/tcp/4001",
            "/ip6/fe80::1/tcp/4001", "/ip6/::ffff:11.0.0.1/tcp/4001",
            "/dns4/11.0.0.1/tcp/4001", "/ip4/11.0.0.1/udp/4001/quic-v1",
            "/ip4/11.0.0.1/tcp/4001/ws", "/ip4/11.0.0.1/tcp/4001/wss",
            "/ip4/11.0.0.1/tcp/65536", "/ip4/11.0.0.1/tcp/65537", "/ip4/11.0.0.1/tcp/-1",
            "/ip4/11.0.0.1/tcp/4294967296", "/ip4/11.0.0.1/tcp/not-a-port"}) {
      check(!eligible(input));
      check(!eligible(input, true));
   }
   auto endpoint = p2p::parse_endpoint("/ip4/11.0.0.1/tcp/4001");
   check(isolated_autonat_tcp(endpoint));
   endpoint.transport.zone = "eth0";
   check(!isolated_autonat_tcp(endpoint, true));
   endpoint.transport.zone.clear();
   endpoint.transport.host = "011.0.0.1";
   check(!isolated_autonat_tcp(endpoint, true));
   std::cout << "configured AutoNAT address self-test: " << count << " assertions\n";
   return 0;
}

} // namespace forge::test::libp2p_interop
