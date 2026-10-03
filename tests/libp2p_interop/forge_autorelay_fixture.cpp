#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

import forge.asio.blocking;
import forge.asio.runtime;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.host_event;
import forge.net.p2p.identify;
import forge.net.p2p.lifecycle;
import forge.net.p2p.node;
import forge.net.p2p.protocol;
import forge.net.p2p.reachability;
import forge.net.p2p.relay;
import forge.net.p2p.scoring;

#include "forge_autorelay_fixture.hxx"

namespace forge::test::libp2p_interop {
namespace {
using namespace std::chrono_literals;

std::string quote(std::string_view input) {
   std::string out = "\"";
   for (const unsigned char c : input) {
      if (c == '"' || c == '\\') {
         out += '\\';
         out += static_cast<char>(c);
      } else if (c < 32) {
         constexpr auto hex = "0123456789abcdef";
         out += "\\u00";
         out += hex[c >> 4];
         out += hex[c & 15];
      } else {
         out += static_cast<char>(c);
      }
   }
   return out + "\"";
}

template <typename Range, typename Encode>
std::string array(const Range& range, Encode encode) {
   std::string out = "[";
   bool first = true;
   for (const auto& item : range) {
      if (!first) out += ',';
      first = false;
      out += encode(item);
   }
   return out + ']';
}

void write_atomic(const std::filesystem::path& path, const std::string& value) {
   const auto temporary = std::filesystem::path{path.string() + ".tmp"};
   {
      std::ofstream out{temporary};
      out << value << '\n';
      out.close();
      if (!out) throw std::runtime_error{"cannot write AutoRelay evidence"};
   }
   std::filesystem::rename(temporary, path);
}

std::string state_name(forge::net::p2p::reachability::state state) {
   using state_type = forge::net::p2p::reachability::state;
   switch (state) {
   case state_type::unknown: return "unknown";
   case state_type::publicly_reachable: return "public";
   case state_type::private_network: return "private";
   case state_type::blocked: return "blocked";
   case state_type::relay_only: return "relay_only";
   }
   return "invalid";
}

std::string sample(forge::net::p2p::node& node, std::chrono::steady_clock::time_point start,
                   std::string_view phase) {
   const auto snapshot = node.diagnostics();
   const auto now = std::chrono::system_clock::now();
   auto leases = std::vector<std::string>{};
   for (const auto& lease : snapshot.relay_reservations) {
      leases.push_back("{\"relay_peer_id\":" + quote(lease.relay_peer.to_string()) +
          ",\"id\":" + std::to_string(lease.id) +
          ",\"expires_unix_ms\":" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
              lease.expires_at).count()) + ",\"ttl_ms\":" + std::to_string(lease.ttl.count()) +
          ",\"max_streams\":" + std::to_string(lease.max_streams) +
          ",\"max_bytes\":" + std::to_string(lease.max_bytes) +
          ",\"max_queued_bytes\":" + std::to_string(lease.max_queued_bytes) +
          ",\"voucher_present\":" + (lease.voucher ? "true" : "false") +
          ",\"remote_limit\":" + (lease.remote_limit ?
              "{\"duration_seconds\":" + std::to_string(lease.remote_limit->duration.count()) +
                  ",\"data_bytes\":" + std::to_string(lease.remote_limit->data) + "}" : "null") +
          ",\"endpoints\":" + array(lease.relay_endpoints, [](const auto& e) { return quote(e.to_string()); }) + "}");
   }
   const auto& manager = snapshot.autorelay;
   const auto autorelay = "{\"enabled\":" + std::string{manager.enabled ? "true" : "false"} +
       ",\"running\":" + (manager.running ? "true" : "false") +
       ",\"permitted\":" + (manager.permitted ? "true" : "false") +
       ",\"stopping\":" + (manager.stopping ? "true" : "false") +
       ",\"candidates\":" + std::to_string(manager.candidates) +
       ",\"pending_reservations\":" + std::to_string(manager.pending_reservations) +
       ",\"reservations\":" + std::to_string(manager.reservations) +
       ",\"automatic_reservations\":" + std::to_string(manager.automatic_reservations) +
       ",\"waiting_refreshes\":" + std::to_string(manager.waiting_refreshes) +
       ",\"max_candidates\":" + std::to_string(manager.max_candidates) +
       ",\"max_parallel_reservations\":" + std::to_string(manager.max_parallel_reservations) +
       ",\"target_reservations\":" + std::to_string(manager.target_reservations) +
       ",\"refreshes\":" + std::to_string(manager.refreshes) +
       ",\"attempts\":" + std::to_string(manager.attempts) +
       ",\"successes\":" + std::to_string(manager.successes) +
       ",\"failures\":" + std::to_string(manager.failures) +
       ",\"renewals\":" + std::to_string(manager.renewals) +
       ",\"invalidated_completions\":" + std::to_string(manager.invalidated_completions) + "}";
   return "{\"elapsed_ms\":" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count()) +
       ",\"unix_ms\":" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()) +
       ",\"phase\":" + quote(phase) + ",\"reachability\":" + quote(state_name(snapshot.reachability.host.effective)) +
       ",\"addresses\":" + array(snapshot.network.local_endpoints, [](const auto& e) { return quote(e.to_string()); }) +
       ",\"reservations\":" + array(leases, [](const auto& s) { return s; }) +
       ",\"autorelay\":" + autorelay +
       ",\"sessions\":" + array(snapshot.sessions, [](const auto& s) {
          return "{\"connection_id\":" + std::to_string(s.id) + ",\"peer_id\":" + quote(s.remote_peer.to_string()) +
              ",\"identified\":" + (s.identify_state == forge::net::p2p::identify::state::identified ? "true" : "false") +
              ",\"direct\":" + (s.path == forge::net::p2p::path::kind::direct ? "true" : "false") +
              ",\"closed\":" + (s.closed ? "true" : "false") + "}";
       }) + ",\"peers\":" + array(snapshot.peers, [](const auto& p) {
          return "{\"peer_id\":" + quote(p.peer.to_string()) + ",\"protocols\":" +
              array(p.protocols, [](const auto& protocol) { return quote(protocol.value); }) + "}";
       }) + ",\"relay_bytes\":" + std::to_string(snapshot.metrics.relay_bytes) +
       ",\"service_reservations\":" + std::to_string(snapshot.metrics.active_relay_reservations) +
       ",\"discovery_attempts\":" + std::to_string(snapshot.metrics.relay_discovery_attempts) +
       ",\"stopped\":" + (snapshot.metrics.stopped ? "true" : "false") + "}";
}
} // namespace

int forge_autorelay_fixture::run(const std::map<std::string, std::string>& args, const support& composition) {
   const auto transport = args.at("transport");
   const auto role = args.at("command") == "autorelay-service" ? "service" : "destination";
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto value = forge::net::p2p::node{runtime, composition.make_options(args)};
   composition.register_echo(value);
   forge::asio::blocking::run(runtime, value.async_listen(composition.listen_endpoint(transport)));
   const auto start = std::chrono::steady_clock::now();
   auto observations = std::vector<std::string>{sample(value, start, "before_start")};
   forge::asio::blocking::run(runtime, value.async_start());
   auto write_result = [&](bool complete, std::string_view error = {}) {
      write_atomic(args.at("result-file"), "{\"schema_version\":1,\"implementation\":\"forge\",\"scenario\":\"autorelay\",\"role\":" +
          quote(role) + ",\"peer_id\":" + quote(value.local_peer().to_string()) + ",\"transport\":" + quote(transport) +
          ",\"reservations_basis\":\"diagnostics.snapshot.relay_reservations\"" +
          ",\"operation_basis\":\"async_start_authenticated_connect_only\",\"complete\":" + (complete ? "true" : "false") +
          ",\"error\":" + (error.empty() ? "null" : quote(error)) + ",\"observations\":" +
          array(observations, [](const auto& s) { return s; }) + "}");
   };
   auto listen = *value.local_endpoint();
   listen.peer = value.local_peer();
   write_atomic(args.at("ready-file"), "{\"implementation\":\"forge\",\"role\":" + quote(role) +
       ",\"peer_id\":" + quote(value.local_peer().to_string()) + ",\"listen_addrs\":[" + quote(listen.to_string()) +
       "],\"status\":\"ready\"}");
   auto connected = std::set<std::string>{};
   std::string failure;
   try {
      while (!std::filesystem::exists(args.at("stop-file"))) {
         if (std::chrono::steady_clock::now() - start > 60s || observations.size() >= 600) {
            throw std::runtime_error{"AutoRelay fixture observation deadline/limit exceeded"};
         }
         if (const auto seed = args.find("seed-file"); seed != args.end()) {
            std::ifstream input{seed->second};
            for (std::string line; std::getline(input, line);) {
               if (line.size() > 512 || connected.size() > 4) throw std::runtime_error{"seed limit exceeded"};
               if (line.empty() || !connected.insert(line).second) continue;
               const auto endpoint = forge::net::p2p::parse_endpoint(line);
               if (!endpoint.peer) throw std::runtime_error{"seed requires expected authenticated peer"};
               (void)forge::asio::blocking::run(runtime, value.async_connect(endpoint,
                   forge::net::p2p::node::connect_options{.expected_peer = endpoint.peer,
                       .allow_relay = false, .timeout = 5s, .allow_hole_punch = false}));
            }
         }
         observations.push_back(sample(value, start, "running"));
         write_result(false);
         std::this_thread::sleep_for(100ms);
      }
   } catch (const std::exception& error) {
      failure = error.what();
   }
   forge::asio::blocking::run(runtime, value.async_stop());
   observations.push_back(sample(value, start, "stopped"));
   // Allow a scheduled renewal to become due; no new work may appear after stop.
   std::this_thread::sleep_for(1200ms);
   observations.push_back(sample(value, start, "post_stop"));
   write_result(true, failure);
   return failure.empty() ? 0 : 2;
}
} // namespace forge::test::libp2p_interop
