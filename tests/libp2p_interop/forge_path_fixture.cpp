#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

import forge.asio.blocking;
import forge.asio.runtime;
import forge.crypto.digest.sha256;
import forge.net.p2p.endpoint;
import forge.net.p2p.diagnostics;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.node;
import forge.net.p2p.protocol;
import forge.net.p2p.relay;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;

#include "forge_path_fixture.hxx"

namespace forge::test::libp2p_interop {
namespace {
using namespace std::chrono_literals;

std::string quote(std::string_view value) {
   auto out = std::string{"\""};
   for (const unsigned char c : value) {
      if (c < 32) { throw std::runtime_error{"control byte in path evidence text"}; }
      if (c == '"' || c == '\\') { out += '\\'; }
      out += static_cast<char>(c);
   }
   return out + '"';
}

void write_atomic(const std::filesystem::path& path, std::string_view value) {
   const auto temporary = std::filesystem::path{path.string() + ".tmp"};
   auto output = std::ofstream{temporary};
   output << value << '\n';
   output.close();
   if (!output) { throw std::runtime_error{"cannot write path artifact"}; }
   std::filesystem::rename(temporary, path);
}

forge_path_fixture::arguments read_fields(const std::filesystem::path& path) {
   if (std::filesystem::file_size(path) > 8192) { throw std::runtime_error{"path control exceeds bound"}; }
   auto out = forge_path_fixture::arguments{};
   auto input = std::ifstream{path};
   for (auto line = std::string{}; std::getline(input, line);) {
      const auto equal = line.find('=');
      if (equal == std::string::npos || equal == 0 || equal + 1 == line.size() || out.size() >= 16 ||
          line.find('\0') != std::string::npos || !out.emplace(line.substr(0, equal), line.substr(equal + 1)).second) {
         throw std::runtime_error{"invalid/duplicate path control field"};
      }
   }
   if (!input.eof()) { throw std::runtime_error{"cannot read path control"}; }
   return out;
}

} // namespace

forge_path_fixture::forge_path_fixture(std::string token) : _token{std::move(token)} {
   if (_token.size() != 32 || !std::ranges::all_of(_token, [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
       })) { throw std::runtime_error{"invalid path case token"}; }
}

void forge_path_fixture::record(std::string_view kind, std::string_view source, std::string fields) {
   if (!source.starts_with("forge.") || fields.size() > 32 * 1024 || fields.size() < 2 ||
       fields.front() != '{' || fields.back() != '}') { throw std::runtime_error{"invalid native capture"}; }
   const auto lock = std::scoped_lock{_mutex};
   if (_events.size() == 256) { _overflow = true; return; }
   const auto captured_at = std::chrono::steady_clock::now();
   const auto suffix = fields.size() > 2 ? ',' + fields.substr(1) : "}";
   _events.push_back("{\"sequence\":" + std::to_string(_events.size() + 1) +
       ",\"mono_ns\":" + std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
           captured_at.time_since_epoch()).count()) + ",\"kind\":" + quote(kind) +
       ",\"source\":" + quote(source) + suffix);
}

std::string forge_path_fixture::result(std::string_view peer, bool finalized, bool joined,
                                       std::string_view error) const {
   const auto lock = std::scoped_lock{_mutex};
   auto events = std::string{"["};
   for (const auto& event : _events) {
      if (events.size() > 1) { events += ','; }
      events += event;
   }
   return "{\"schema_version\":1,\"implementation\":\"forge\",\"local_peer_id\":" + quote(peer) +
       ",\"case_token\":" + quote(_token) + ",\"overflow\":" + (_overflow ? "true" : "false") +
       ",\"finalized\":" + (finalized ? "true" : "false") + ",\"joined\":" + (joined ? "true" : "false") +
       ",\"error\":" + (error.empty() ? "null" : quote(error)) + ",\"events\":" + events + "]}";
}

boost::asio::awaitable<void> forge_path_fixture::echo(
    forge::net::p2p::node& node, forge::net::p2p::stream& stream,
    const forge::net::p2p::peer_id& peer, forge::net::p2p::path::kind requested_path,
    std::uint64_t session_id, std::string_view phase, bool server) {
   if (!stream.valid() || stream.authentication() == forge::net::p2p::peer_authentication::unverified ||
       (phase != "relay_before" && phase != "direct_after" && phase != "relay_after")) {
      throw std::runtime_error{"path echo requires an existing authenticated stream/owner and no new dial"};
   }
   const auto before = node.metrics();
   const auto circuit = requested_path == forge::net::p2p::path::kind::relay;
   if (circuit == (stream.authentication() == forge::net::p2p::peer_authentication::quic_tls)) {
      throw std::runtime_error{"actual stream authentication/path mismatch"};
   }
   auto owner = std::optional<forge::net::p2p::diagnostics::session>{};
   const auto snapshot = node.diagnostics();
   for (const auto& session : snapshot.sessions) {
      if (session.id == session_id && !session.closed && session.remote_peer == peer &&
          (session.path == forge::net::p2p::path::kind::relay) == circuit &&
          session.authentication == stream.authentication()) {
         owner = session;
      }
   }
   if (!owner || (!circuit && !owner->remote_endpoint) ||
       (circuit && (!owner->circuit_endpoint || !owner->carrier_session_id || owner->muxer.value.empty()))) {
      throw std::runtime_error{"missing native stream/session path facts"};
   }
   auto carrier_fields = std::string{};
   if (circuit) {
      const auto carrier = std::ranges::find(snapshot.sessions, *owner->carrier_session_id,
                                              &forge::net::p2p::diagnostics::session::id);
      if (carrier == snapshot.sessions.end() || carrier->closed || !owner->relay_peer ||
          carrier->remote_peer != *owner->relay_peer || carrier->path != forge::net::p2p::path::kind::direct ||
          carrier->authentication != forge::net::p2p::peer_authentication::quic_tls ||
          !carrier->local_endpoint || !carrier->remote_endpoint) {
         throw std::runtime_error{"circuit lacks its authenticated native QUIC carrier owner"};
      }
      carrier_fields = ",\"endpoint_basis\":\"logical_authenticated_circuit_route\""
          ",\"carrier_basis\":\"forge.node.diagnostics.native_carrier\""
          ",\"carrier_connection_id\":" + quote(std::to_string(carrier->id)) +
          ",\"carrier_remote_peer_id\":" + quote(carrier->remote_peer.to_string()) +
          ",\"carrier_local_address\":" + quote(carrier->local_endpoint->to_string()) +
          ",\"carrier_remote_address\":" + quote(carrier->remote_endpoint->to_string());
   }
   const auto connection = std::to_string(owner->id);
   const auto path = requested_path == forge::net::p2p::path::kind::relay ? "relay" : "direct";
   const auto security = stream.authentication() == forge::net::p2p::peer_authentication::noise ? "/noise" : "/tls/1.0.0";
   auto first = false;
   {
      const auto lock = std::scoped_lock{_mutex};
      first = std::ranges::find(_captured_connections, owner->id) == _captured_connections.end();
      if (first) { _captured_connections.push_back(owner->id); }
   }
   if (first) {
      const auto local_address = owner->local_endpoint ? quote(owner->local_endpoint->to_string()) : std::string{"null"};
      record("authenticated_connection", "forge.authenticated_stream.diagnostics", "{\"connection_id\":" + quote(connection) +
          ",\"remote_peer_id\":" + quote(peer.to_string()) + ",\"path\":" + quote(path) + ",\"authenticated\":true"
          ",\"remote_address\":" + quote((circuit ? owner->circuit_endpoint : owner->remote_endpoint)->to_string()) +
          ",\"local_address\":" + local_address + carrier_fields +
          ",\"security\":" + quote(security) + ",\"muxer\":" + quote(owner->muxer.value) +
          ",\"transport\":" + quote(requested_path == forge::net::p2p::path::kind::relay ? "circuit" : "quic") +
          ",\"authentication_basis\":" + quote(requested_path == forge::net::p2p::path::kind::relay ?
              "native_relay_inner_upgrade" : "native_quic_authenticated_output") +
          ",\"relay_peer_id\":" + (owner->relay_peer ? quote(owner->relay_peer->to_string()) : "null") +
          ",\"direction\":" + quote(owner->direction == forge::net::p2p::diagnostics::session_direction::inbound ? "inbound" : "outbound") + "}");
   }
   const auto text = "path:" + _token + ':' + std::string{phase};
   const auto request = std::vector<std::uint8_t>{text.begin(), text.end()};
   auto response = std::vector<std::uint8_t>{};
   if (server) {
      response = co_await stream.async_read_frame({.max_size = 128});
      if (response != request) { throw std::runtime_error{"path challenge mismatch"}; }
      co_await stream.async_write_frame(response);
   } else {
      co_await stream.async_write_frame(request);
      response = co_await stream.async_read_frame({.max_size = 128});
      if (response != request) { throw std::runtime_error{"path echo mismatch"}; }
   }
   const auto after = node.metrics();
   record("echo", "forge.path_echo.io", "{\"phase\":" + quote(phase) + ",\"protocol\":\"/forge/interop/path-echo/1\""
       ",\"connection_id\":" + quote(connection) + ",\"stream_id\":" + quote(std::to_string(stream.id())) +
       ",\"remote_peer_id\":" + quote(peer.to_string()) + ",\"path\":" + quote(path) + ",\"fresh_dial\":false,\"io_basis\":\"retained_native_stream\",\"server\":" + (server ? "true" : "false") +
       ",\"read_bytes\":" + std::to_string(response.size()) + ",\"write_bytes\":" + std::to_string(request.size()) +
       ",\"read_sha256\":" + quote(forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{response}).str()) +
       ",\"write_sha256\":" + quote(forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{request}).str()) +
       ",\"dial_attempts_before\":" + std::to_string(before.path_direct_attempts + before.path_relay_attempts) +
       ",\"dial_attempts_after\":" + std::to_string(after.path_direct_attempts + after.path_relay_attempts) + "}");
}

int forge_path_fixture::run(const arguments& args, const support& composition) {
   if (!composition.make_options || !composition.listen_endpoint || args.at("transport") != "quic" ||
       args.contains("pnet-key-file")) {
      throw std::runtime_error{"path-live requires ordinary native QUIC composition"};
   }
   auto fixture = forge_path_fixture{args.at("case-token")};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto node = forge::net::p2p::node{runtime, composition.make_options(args)};
   std::string error;
   auto joined = false;
   auto expected = std::optional<forge::net::p2p::peer_id>{};
   auto native_listener = std::optional<forge::net::p2p::endpoint>{};
   auto expect_relay_after = std::atomic_bool{false};
   auto last_snapshot = std::string{};
   auto snapshot = [&] {
      const auto native = node.diagnostics();
      const auto& metrics = native.metrics;
      auto direct = std::string{"["};
      auto relay = std::string{"["};
      auto identify = std::string{"["};
      for (const auto& session : native.sessions) {
         if (session.closed) { continue; }
         if (identify.size() > 1) { identify += ','; }
         identify += "{\"connection_id\":" + quote(std::to_string(session.id)) +
             ",\"peer_id\":" + quote(session.remote_peer.to_string()) +
             ",\"local_address\":" + (session.local_endpoint ? quote(session.local_endpoint->to_string()) : "null") +
             ",\"state\":" + std::to_string(static_cast<unsigned>(session.identify_state)) +
             ",\"error\":" + quote(session.identify_error) + "}";
         if (!expected || session.remote_peer != *expected) { continue; }
         auto& ids = session.path == forge::net::p2p::path::kind::relay ? relay : direct;
         if (ids.size() > 1) { ids += ','; }
         ids += quote(std::to_string(session.id));
      }
      auto observations = std::string{"["};
      for (const auto& peer : native.peers) {
         if (!peer.observed_endpoint) { continue; }
         if (observations.size() > 1) { observations += ','; }
         observations += "{\"observer_peer_id\":" + quote(peer.peer.to_string()) +
             ",\"observed_address\":" + quote(peer.observed_endpoint->to_string()) + "}";
      }
      auto fields = "{\"remote_peer_id\":" + (expected ? quote(expected->to_string()) : "null") +
          ",\"direct_connection_ids\":" + direct + "],\"relay_connection_ids\":" + relay + "]"
          ",\"identify\":" + identify + "]"
          ",\"address_observations\":" + observations + "]"
          ",\"initial_native_listener\":" + (native_listener ? quote(native_listener->to_string()) : "null") +
          ",\"hole_punch_attempts\":" + std::to_string(metrics.hole_punch_attempts) +
          ",\"hole_punch_successes\":" + std::to_string(metrics.hole_punch_successes) +
          ",\"hole_punch_failures\":" + std::to_string(metrics.hole_punch_failures) +
          ",\"sessions_opened\":" + std::to_string(metrics.sessions_opened) +
          ",\"path_direct_attempts\":" + std::to_string(metrics.path_direct_attempts) +
          ",\"path_relay_attempts\":" + std::to_string(metrics.path_relay_attempts) +
          ",\"stopped\":" + (metrics.stopped ? "true" : "false") + "}";
      if (fields != last_snapshot) { fixture.record("native_snapshot", "forge.node.diagnostics", fields); last_snapshot = fields; }
   };
   node.register_protocol_handler(forge::net::p2p::protocol_id{"/forge/interop/path-echo/1"},
       [&](forge::net::p2p::node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
          try {
             if (incoming.session.path == forge::net::p2p::path::kind::relay) {
                co_await fixture.echo(node, incoming.stream, incoming.session.remote_peer, incoming.session.path, incoming.session.id, "relay_before", true);
                if (expect_relay_after.load(std::memory_order_acquire)) {
                   co_await fixture.echo(node, incoming.stream, incoming.session.remote_peer, incoming.session.path, incoming.session.id, "relay_after", true);
                }
             } else {
                co_await fixture.echo(node, incoming.stream, incoming.session.remote_peer, incoming.session.path, incoming.session.id, "direct_after", true);
             }
             co_await incoming.stream.async_close();
          } catch (const std::exception& failure) {
             fixture.record("application_error", "forge.path_echo.native_failure",
                 "{\"remote_peer_id\":" + quote(incoming.session.remote_peer.to_string()) +
                 ",\"stream_id\":" + quote(std::to_string(incoming.stream.id())) +
                 ",\"error\":" + quote(failure.what()) + "}");
             throw;
          }
       });
   try {
      forge::asio::blocking::run(runtime, node.async_listen(composition.listen_endpoint(args)));
      native_listener = node.local_endpoint();
      if (!native_listener || !native_listener->is_direct_quic()) {
         throw std::runtime_error{"path fixture requires its actual direct QUIC listener"};
      }
      forge::asio::blocking::run(runtime, node.async_start());
      auto ready_fields = std::string{};
      if (args.at("path-role") != "relay") {
         auto address = forge::net::p2p::parse_endpoint(args.at("relay-addr"));
         const auto peer = forge::net::p2p::peer_id::from_string(args.at("relay-peer-id"));
         if (!address.peer || *address.peer != peer) { throw std::runtime_error{"relay address/peer mismatch"}; }
         forge::asio::blocking::run(runtime, node.async_connect(address, {.expected_peer = peer}));
         if (args.at("path-role") == "destination") {
            const auto reservation = forge::asio::blocking::run(runtime, node.async_reserve_relay(peer));
            if (reservation.relay_peer != peer || reservation.relay_endpoints.empty()) { throw std::runtime_error{"no actual relay reservation"}; }
            // A circuit dial address is a configuration DTO, not a socket receipt.
            ready_fields = ",\"circuit_addr\":" + quote(args.at("relay-addr") + "/p2p-circuit/p2p/" + node.local_peer().to_string());
         }
      }
      auto address = *native_listener;
      address.peer = node.local_peer();
      write_atomic(args.at("ready-file"), "{\"status\":\"ready\",\"implementation\":\"forge\",\"peer_id\":" +
          quote(node.local_peer().to_string()) + ",\"case_token\":" + quote(args.at("case-token")) +
          ",\"listen_addrs\":[" + quote(address.to_string()) + "],\"path_bindings\":\"native_public_diagnostics_v1\"" +
          ready_fields + "}");
      const auto deadline = std::chrono::steady_clock::now() + 55s;
      auto sequence = std::uint64_t{};
      while (!std::filesystem::exists(args.at("stop-file"))) {
         if (std::chrono::steady_clock::now() >= deadline) { throw std::runtime_error{"path fixture deadline"}; }
         if (std::filesystem::exists(args.at("control-file"))) {
            const auto control = read_fields(args.at("control-file"));
            const auto next = std::stoull(control.at("sequence"));
            if (next > sequence) {
               if (next != sequence + 1 || control.at("case-token") != args.at("case-token")) {
                  throw std::runtime_error{"path control sequence/token mismatch"};
               }
               const auto plan = read_fields(args.at("plan-file"));
               if (plan.at("case-token") != args.at("case-token")) { throw std::runtime_error{"path plan token mismatch"}; }
               const auto peer = forge::net::p2p::peer_id::from_string(plan.at("peer-id"));
               const auto action = control.at("action");
               if (action == "bind") {
                  if (expected) { throw std::runtime_error{"peer already bound"}; }
                  const auto& outcome = plan.at("outcome");
                  if (outcome != "success" && outcome != "failed" && outcome != "cancelled") {
                     throw std::runtime_error{"invalid path case outcome"};
                  }
                  expect_relay_after.store(outcome != "success", std::memory_order_release);
                  expected = peer;
                  for (const auto& session : node.diagnostics().sessions) {
                     if (!session.closed && session.remote_peer == peer && session.path != forge::net::p2p::path::kind::relay) {
                        throw std::runtime_error{"preexisting direct session"};
                     }
                  }
                  fixture.record("baseline", "forge.node.diagnostics", "{\"remote_peer_id\":" + quote(peer.to_string()) +
                      ",\"direct_connection_ids\":[]}");
               } else if (!expected || peer != *expected) { throw std::runtime_error{"unbound control peer"}; }
               else if (action == "connect") {
                  auto circuit = forge::net::p2p::parse_endpoint(plan.at("circuit-addr"));
                  if (!circuit.relayed || circuit.relayed->target != peer || !circuit.peer ||
                      circuit.peer->to_string() != plan.at("relay-peer-id")) {
                     throw std::runtime_error{"circuit relay/target peer mismatch"};
                  }
                  // async_connect owns only direct address dialing. Establish
                  // the circuit through its existing protocol-open policy and
                  // verify a real Ping before the independent relay challenge.
                  static_cast<void>(forge::asio::blocking::run(runtime, node.async_ping(peer, {
                      .allow_relay = true, .relay_peer = *circuit.peer, .timeout = 10s,
                      .allow_hole_punch = false})));
               } else if (action == "barrier" || action == "direct_after" || action == "relay_after") {
                  throw std::runtime_error{"application initiator must be donor's observed existing connection; general Forge async_open can dial"};
               } else if (action == "release") {
                  fixture.record("barrier_observed", "forge.path_echo.control", "{\"case_token\":" + quote(args.at("case-token")) + "}");
               } else if (action == "cancel") {
                  snapshot();
                  fixture.record("cancellation_requested", "forge.node.async_cancel_hole_punch.request",
                      "{\"remote_peer_id\":" + quote(peer.to_string()) + "}");
                  const auto accepted = forge::asio::blocking::run(runtime, node.async_cancel_hole_punch(peer));
                  fixture.record("native_cancel_completed", "forge.node.async_cancel_hole_punch.return",
                      "{\"remote_peer_id\":" + quote(peer.to_string()) + ",\"accepted\":" + (accepted ? "true" : "false") + "}");
                  if (!accepted) { throw std::runtime_error{"no active shared peer owner accepted cancellation"}; }
                  snapshot();
                  fixture.record("native_service_joined", "forge.node.async_cancel_hole_punch.join",
                      "{\"remote_peer_id\":" + quote(peer.to_string()) + ",\"cancelled\":true,\"accepted\":true}");
               } else { throw std::runtime_error{"unsupported path control"}; }
               sequence = next;
               fixture.record("control_completed", "forge.path_control.native_call", "{\"control_sequence\":" + std::to_string(next) +
                   ",\"action\":" + quote(action) + "}");
            }
         }
         snapshot();
         write_atomic(args.at("result-file"), fixture.result(node.local_peer().to_string(), false, false));
         std::this_thread::sleep_for(25ms);
      }
   } catch (const std::exception& failure) { error = failure.what(); }
   try {
      forge::asio::blocking::run(runtime, node.async_stop());
      snapshot();
      joined = true;
   } catch (const std::exception& failure) { if (error.empty()) { error = failure.what(); } }
   write_atomic(args.at("result-file"), fixture.result(node.local_peer().to_string(), true, joined, error));
   return error.empty() && joined ? 0 : 2;
}

} // namespace forge::test::libp2p_interop
