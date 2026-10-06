#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <exception>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>

import forge.asio.blocking;
import forge.asio.runtime;
import forge.codec.hex;
import forge.crypto.digest.sha256;
import forge.net.p2p.diagnostics;
import forge.net.p2p.connection_gater;
import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.p2p.identify;
import forge.net.p2p.node;
import forge.net.p2p.protocol;
import forge.net.p2p.private_network;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;
import forge.net.tls.options;
import forge.net.yamux.options;

#include "forge_connection_fixture.hxx"
#include "forge_coordinated_fixture.hxx"

namespace forge::test::libp2p_interop {
namespace {
using namespace std::chrono_literals;
namespace p2p = forge::net::p2p;
using arguments = std::map<std::string, std::string>;

std::string quote(std::string_view text) {
   auto out = std::string{"\""};
   for (const auto c : text) {
      if (static_cast<unsigned char>(c) < 32) {
         throw std::runtime_error{"control byte in coordinated evidence"};
      }
      if (c == '"' || c == '\\') {
         out += '\\';
      }
      out += c;
   }
   return out + '"';
}

bool hex(std::string_view value, std::size_t size) {
   return value.size() == size &&
          std::ranges::all_of(value, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

void write(const std::string& path, std::string_view text) {
   const auto pending = path + ".coordinated.tmp";
   auto output = std::ofstream{pending};
   output << text << '\n';
   output.close();
   if (!output) {
      throw std::runtime_error{"cannot write coordinated artifact"};
   }
   std::filesystem::rename(pending, path);
}

arguments fields(const std::string& path) {
   if (std::filesystem::file_size(path) > 8192) {
      throw std::runtime_error{"coordinated control exceeds bound"};
   }
   auto input = std::ifstream{path};
   auto out = arguments{};
   for (auto line = std::string{}; std::getline(input, line);) {
      const auto equal = line.find('=');
      if (equal == std::string::npos || equal == 0 || equal + 1 == line.size() ||
          line.find('\0') != std::string::npos || out.size() == 8 ||
          !out.emplace(line.substr(0, equal), line.substr(equal + 1)).second) {
         throw std::runtime_error{"invalid/duplicate coordinated control field"};
      }
   }
   if (!input.eof()) {
      throw std::runtime_error{"cannot read coordinated control"};
   }
   return out;
}

void gate(const arguments& args, std::string_view action, unsigned sequence) {
   const auto deadline = std::chrono::steady_clock::now() + 60s;
   while (std::chrono::steady_clock::now() < deadline) {
      if (std::filesystem::exists(args.at("stop-file"))) {
         throw std::runtime_error{"coordinated actor canceled"};
      }
      if (!std::filesystem::exists(args.at("control-file"))) {
         std::this_thread::sleep_for(10ms);
         continue;
      }
      const auto control = fields(args.at("control-file"));
      if (control.size() != 3 || control.at("case-token") != args.at("case-token") ||
          (control.at("sequence") != "1" && control.at("sequence") != "2")) {
         throw std::runtime_error{"coordinated gate sequence/token mismatch"};
      }
      const auto current = static_cast<unsigned>(control.at("sequence").front() - '0');
      if (current > sequence || (current == sequence && control.at("action") != action)) {
         throw std::runtime_error{"coordinated gate action mismatch"};
      }
      if (current == sequence) {
         return;
      }
      std::this_thread::sleep_for(10ms);
   }
   throw std::runtime_error{"coordinated start gate expired"};
}

std::chrono::milliseconds validate(const arguments& args) {
   const auto required = std::set<std::string>{"command",   "scenario",   "transport",   "coord-role", "case-token",
                                               "bind-ip",   "ready-file", "result-file", "stop-file",  "control-file",
                                               "plan-file", "store-dir",  "timeout-ms"};
   if (std::ranges::any_of(args,
                           [&](const auto& value) {
                              return !required.contains(value.first) && value.first != "pnet-key-file" &&
                                     value.first != "pnet-fingerprint";
                           }) ||
       std::ranges::any_of(required, [&](const auto& name) { return !args.contains(name) || args.at(name).empty(); })) {
      throw std::runtime_error{"coordinated-live requires exactly its supported flag/value pairs"};
   }
   auto consumed = std::size_t{};
   const auto budget = std::stoul(args.at("timeout-ms"), &consumed);
   const auto address = boost::asio::ip::make_address(args.at("bind-ip"));
   const auto private_profile = args.at("scenario") == "coordinated_dial_port_reuse_private_pnet";
   if (args.at("command") != "coordinated-live" ||
       (!private_profile && args.at("scenario") != "coordinated_dial_port_reuse") ||
       (private_profile && (args.at("transport") != "tcp-pnet-noise" || !args.contains("pnet-key-file") ||
                            args.at("pnet-key-file").empty() || !args.contains("pnet-fingerprint") ||
                            !hex(args.at("pnet-fingerprint"), 64))) ||
       (!private_profile &&
        (args.at("transport") != "tcp" || args.contains("pnet-key-file") || args.contains("pnet-fingerprint"))) ||
       (args.at("coord-role") != "initiator" && args.at("coord-role") != "responder") ||
       !hex(args.at("case-token"), 32) || consumed != args.at("timeout-ms").size() || budget < 1000 || budget > 30000 ||
       !address.is_v4() || address.is_unspecified() || address.is_multicast()) {
      throw std::runtime_error{"invalid coordinated-live identity/role/transport/budget contract"};
   }
   return std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(budget)};
}

std::string ready(const arguments& args, const p2p::node& node, const p2p::endpoint& local, std::string_view status,
                  std::string_view extra = {}) {
   auto advertised = local;
   advertised.peer = node.local_peer();
   return "{\"implementation\":\"forge\",\"status\":" + quote(status) +
          ",\"case_token\":" + quote(args.at("case-token")) + ",\"peer_id\":" + quote(node.local_peer().to_string()) +
          ",\"listen_addrs\":[" + quote(advertised.to_string()) + "],\"listener_address\":" + quote(local.to_string()) +
          ",\"listener_port\":" + std::to_string(local.transport.port) + std::string{extra} + "}";
}

p2p::diagnostics::session winner(const p2p::node& node, const p2p::peer_id& expected, p2p::endpoint remote,
                                 p2p::endpoint local, bool source) {
   const auto snapshot = node.diagnostics();
   if (snapshot.sessions.size() != 1) {
      throw std::runtime_error{"coordinated winner is not unique"};
   }
   const auto session = capture_identified_connection(node, expected, remote, snapshot.sessions.front().direction);
   remote.peer.reset();
   local.peer.reset();
   if (!session.local_endpoint || !session.remote_endpoint ||
       session.local_endpoint->transport.authority() != local.transport.authority() ||
       session.remote_endpoint->transport.authority() != remote.transport.authority() ||
       session.muxer.value != "/yamux/1.0.0" || session.authentication == p2p::peer_authentication::unverified ||
       (source && session.direction != p2p::diagnostics::session_direction::outbound)) {
      throw std::runtime_error{"winning connection lacks expected identity/owned tuple/outgoing source"};
   }
   return session;
}

std::string connection_receipt(const p2p::node& node, const p2p::diagnostics::session& session, bool source,
                               std::string_view application = {}) {
   const auto outbound = session.direction == p2p::diagnostics::session_direction::outbound;
   auto security = std::string{};
   if (session.authentication == p2p::peer_authentication::noise) {
      security = "/noise";
   } else if (session.authentication == p2p::peer_authentication::libp2p_tls) {
      security = "/tls/1.0.0";
   } else {
      throw std::runtime_error{"coordinated receipt lacks native TCP authentication"};
   }
   auto roles = std::string{"null"};
   auto role_source = std::string{"null"};
   if (session.security_role && session.yamux_role) {
      auto security_role = std::optional<std::string_view>{};
      auto yamux_role = std::optional<std::string_view>{};
      switch (*session.security_role) {
      case forge::net::tls::endpoint_role::client: security_role = "initiator"; break;
      case forge::net::tls::endpoint_role::server: security_role = "responder"; break;
      }
      switch (*session.yamux_role) {
      case forge::net::yamux::side::initiator: yamux_role = "initiator"; break;
      case forge::net::yamux::side::responder: yamux_role = "responder"; break;
      }
      if (security_role && yamux_role) {
         roles = "{\"security_role\":" + quote(*security_role) + ",\"yamux_role\":" + quote(*yamux_role) + "}";
         role_source = quote("forge.node.diagnostics.session.security_role+yamux_role");
      }
   }
   // Requested role remains separate from the successful native delegate facts.
   return "{\"connection\":{\"source\":\"forge.node.diagnostics.authenticated-session\",\"connection_id\":" +
          std::to_string(session.id) + ",\"local_peer_id\":" + quote(node.local_peer().to_string()) +
          ",\"remote_peer_id\":" + quote(session.remote_peer.to_string()) +
          ",\"local_address\":" + quote(session.local_endpoint->to_string()) +
          ",\"remote_address\":" + quote(session.remote_endpoint->to_string()) +
          ",\"transport\":\"tcp\",\"security\":" + quote(security) + ",\"muxer\":" + quote(session.muxer.value) +
          ",\"early_muxer_negotiation\":" + (session.used_early_muxer_negotiation ? "true" : "false") +
          "}"
          ",\"native_outgoing_winner\":" +
          (outbound ? "true" : "false") + ",\"physical_direction\":" + quote(outbound ? "outbound" : "inbound") +
          ",\"direction_source\":\"forge.node.diagnostics.session.direction\""
          ",\"requested_role\":" +
          quote(source ? "initiator" : "responder") + ",\"roles\":" + roles + ",\"role_source\":" + role_source +
          (application.empty() ? std::string{} : ",\"application\":" + std::string{application}) + "}";
}

std::string frame_receipt(std::span<const std::uint8_t> frame) {
   return "{\"framed_hex\":" + quote(forge::codec::hex::encode(frame)) +
          ",\"raw\":false,\"read\":{\"framed_bytes\":" + std::to_string(frame.size()) +
          ",\"framed_sha256\":" + quote(forge::crypto::digest::sha256::hash(frame).str()) +
          ",\"frames\":1,\"complete_frames\":true,\"invalid_or_over_limit\":false}}";
}

std::uint64_t native_dial_rejections(const p2p::node& node) {
   const auto metrics = node.metrics();
   return metrics.gater_peer_dial_rejections + metrics.gater_address_dial_rejections;
}

std::string application_receipt(const p2p::diagnostics::session& session, const p2p::stream& stream,
                                std::span<const std::uint8_t> request_frame,
                                std::span<const std::uint8_t> response_frame,
                                std::uint64_t blocked_before, std::uint64_t blocked_after) {
   if (blocked_before != 0 || blocked_after != blocked_before || request_frame.empty() ||
       !std::ranges::equal(request_frame, response_frame)) {
      throw std::runtime_error{"coordinated echo lacks matching observed wire frames"};
   }
   return "{\"protocol\":\"/forge/interop/relay-echo/1\",\"connection_id\":" + std::to_string(session.id) +
          ",\"stream_id\":" + std::to_string(stream.id()) +
          ",\"native_dial_attempts_before\":" + std::to_string(blocked_before) +
          ",\"native_dial_attempts_after\":" + std::to_string(blocked_after) +
          ",\"dial_observation_source\":\"forge.node.metrics.sealed-gater-rejections\"" +
          ",\"fresh_dial\":false,\"request\":" + frame_receipt(request_frame) +
          ",\"response\":" + frame_receipt(response_frame) + "}";
}

std::string result(const arguments& args, const p2p::node& node, std::string_view status, std::string_view receipt,
                   bool joined, bool native_joined, bool admitted, std::string_view error = {}) {
   const auto fingerprint = args.contains("pnet-fingerprint") ? quote(args.at("pnet-fingerprint")) : "null";
   return "{\"schema_version\":1,\"implementation\":\"forge\",\"scenario\":" + quote(args.at("scenario")) +
          ",\"case_token\":" + quote(args.at("case-token")) + ",\"actor_role\":" + quote(args.at("coord-role")) +
          ",\"local_peer_id\":" + quote(node.local_peer().to_string()) + ",\"pnet_fingerprint\":" + fingerprint +
          ",\"status\":" + quote(status) + ",\"joined\":" + (joined ? "true" : "false") +
          ",\"native_dial_joined\":" + (native_joined ? "true" : "false") +
          ",\"operation_admitted\":" + (admitted ? "true" : "false") +
          ",\"admission_source\":\"coordinated_actor.preflight\",\"receipt\":" + std::string{receipt} +
          ",\"error\":" + (error.empty() ? "null" : quote(error)) + ",\"resources\":{\"file_descriptors\":" +
          std::to_string(node.diagnostics().resources.system.file_descriptors) + "}}";
}

boost::asio::awaitable<coordinated_support::exchange> probe_payload(const coordinated_support& support,
                                                                    std::shared_ptr<p2p::stream> stream,
                                                                    std::vector<std::uint8_t> request) {
   auto observed = coordinated_support::exchange{};
   co_await support.write_payload(*stream, request, &observed.request_frame);
   observed.payload = co_await support.read_payload(*stream, &observed.response_frame);
   co_return observed;
}
} // namespace

void coordinated_support::dial_gate::seal() noexcept {
   _sealed.store(true, std::memory_order_release);
}
bool coordinated_support::dial_gate::intercept_peer_dial(const p2p::peer_id&) noexcept {
   return !_sealed.load(std::memory_order_acquire);
}
bool coordinated_support::dial_gate::intercept_address_dial(const p2p::peer_id&, const p2p::endpoint&) noexcept {
   return !_sealed.load(std::memory_order_acquire);
}

void coordinated_fixture_self_test() {
   // Validator/gater regressions only, never synthetic native acceptance.
   auto args = arguments{{"command", "coordinated-live"},
                         {"scenario", "coordinated_dial_port_reuse"},
                         {"transport", "tcp"},
                         {"coord-role", "initiator"},
                         {"case-token", std::string(32, 'a')},
                         {"bind-ip", "127.0.0.1"},
                         {"ready-file", "ready"},
                         {"result-file", "result"},
                         {"stop-file", "stop"},
                         {"control-file", "control"},
                         {"plan-file", "plan"},
                         {"store-dir", "store"},
                         {"timeout-ms", "20000"}};
   if (validate(args) != 20s) {
      throw std::runtime_error{"coordinated native budget validation regressed"};
   }
   auto reject = [](const arguments& invalid) {
      auto rejected = false;
      try {
         static_cast<void>(validate(invalid));
      } catch (const std::exception&) {
         rejected = true;
      }
      if (!rejected) {
         throw std::runtime_error{"invalid coordinated fixture contract accepted"};
      }
   };
   for (const auto& [name, value] : arguments{{"coord-role", "source"},
                                              {"bind-ip", "0.0.0.0"},
                                              {"case-token", "foreign"},
                                              {"timeout-ms", "0"},
                                              {"unknown", "value"}}) {
      auto invalid = args;
      invalid[name] = value;
      reject(invalid);
   }
   auto private_args = args;
   private_args["scenario"] = "coordinated_dial_port_reuse_private_pnet";
   private_args["transport"] = "tcp-pnet-noise";
   reject(private_args);
   private_args["pnet-key-file"] = "key";
   private_args["pnet-fingerprint"] = std::string(64, 'b');
   if (validate(private_args) != 20s) {
      throw std::runtime_error{"coordinated private contract rejected"};
   }
   args["pnet-key-file"] = "key";
   reject(args);
   auto gate = coordinated_support::dial_gate{};
   if (!gate.intercept_peer_dial({}) || !gate.intercept_address_dial({}, {})) {
      throw std::runtime_error{"coordinated initial dial was not admitted"};
   }
   gate.seal();
   if (gate.intercept_peer_dial({}) || gate.intercept_address_dial({}, {})) {
      throw std::runtime_error{"retained probe can open a replacement socket"};
   }
}

int run_coordinated_fixture(const arguments& input, const coordinated_support& support) {
   auto args = input;
   args.try_emplace("timeout-ms", "20000");
   const auto budget = validate(args);
   if (!support.make_options || !support.register_echo || !support.read_payload || !support.write_payload) {
      throw std::runtime_error{"coordinated fixture composition is incomplete"};
   }
   auto options = support.make_options(args);
   const auto private_profile = args.at("scenario") == "coordinated_dial_port_reuse_private_pnet";
   if ((private_profile && (!options.private_network || !options.private_network->protector)) ||
       (!private_profile && options.private_network) || options.allow_insecure_test_mode ||
       options.relay_policy.service_enabled || options.relay_policy.client_enabled || options.path_policy.allow_relay ||
       options.path_policy.allow_hole_punch) {
      throw std::runtime_error{"coordinated actor requires its authenticated TCP profile without Relay/DCUtR"};
   }
   if (private_profile) {
      const auto fingerprint = options.private_network->protector->fingerprint();
      if (forge::codec::hex::encode(fingerprint.bytes) != args.at("pnet-fingerprint")) {
         throw std::runtime_error{"coordinated PSK fingerprint differs from installed key"};
      }
   }
   auto dial_gate = std::make_shared<coordinated_support::dial_gate>();
   options.connection_gater = dial_gate;
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto node = p2p::node{runtime, std::move(options)};
   const auto source = args.at("coord-role") == "initiator";
   auto handler_mutex = std::mutex{};
   auto handler_peer = std::optional<p2p::peer_id>{};
   auto handler_local = std::optional<p2p::endpoint>{};
   auto handler_remote = std::optional<p2p::endpoint>{};
   auto handler_receipt = std::string{};
   auto handler_failure = std::exception_ptr{};
   support.register_echo(node, [&](p2p::node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
      try {
         auto expected = std::optional<p2p::peer_id>{};
         auto local = std::optional<p2p::endpoint>{};
         auto remote = std::optional<p2p::endpoint>{};
         {
            const auto lock = std::scoped_lock{handler_mutex};
            expected = handler_peer;
            local = handler_local;
            remote = handler_remote;
         }
         if (source || !expected || !local || !remote || incoming.session.remote_peer != *expected) {
            throw std::runtime_error{"coordinated probe is responder-only and bound to the planned peer"};
         }
         const auto captured = winner(node, *expected, *remote, *local, false);
         const auto blocked_before = native_dial_rejections(node);
         auto request_frame = std::vector<std::uint8_t>{};
         auto response_frame = std::vector<std::uint8_t>{};
         const auto payload = co_await support.read_payload(incoming.stream, &request_frame);
         const auto text = "coordinated:" + args.at("case-token");
         if (payload != std::vector<std::uint8_t>{text.begin(), text.end()}) {
            throw std::runtime_error{"coordinated probe challenge mismatch"};
         }
         co_await support.write_payload(incoming.stream, payload, &response_frame);
         require_same_connection(node, captured);
         static_cast<void>(endpoint_connection_receipt(node, *expected, incoming.stream, captured.direction));
         const auto proof = connection_receipt(
             node, captured, false, application_receipt(captured, incoming.stream, request_frame, response_frame,
                 blocked_before, native_dial_rejections(node)));
         co_await incoming.stream.async_close();
         const auto lock = std::scoped_lock{handler_mutex};
         if (!handler_receipt.empty()) {
            throw std::runtime_error{"duplicate coordinated native probe"};
         }
         handler_receipt = proof;
      } catch (...) {
         const auto lock = std::scoped_lock{handler_mutex};
         handler_failure = std::current_exception();
         throw;
      }
   });
   auto failure = std::exception_ptr{};
   auto receipt = std::string{"null"};
   auto status = std::string{"error"};
   auto native_joined = false;
   auto admitted = false;
   try {
      forge::asio::blocking::run(runtime, node.async_hydrate_peer_state());
      forge::asio::blocking::run(runtime,
                                 node.async_listen(p2p::parse_endpoint("/ip4/" + args.at("bind-ip") + "/tcp/0")));
      const auto local = node.local_endpoint();
      if (!local || !local->is_direct_tcp() || local->transport.port == 0) {
         throw std::runtime_error{"coordinated fixture lacks its real TCP listener"};
      }
      write(args.at("ready-file"), ready(args, node, *local, "ready"));
      gate(args, "start", 1);
      const auto plan = fields(args.at("plan-file"));
      if (plan.size() != 3 || plan.at("case-token") != args.at("case-token")) {
         throw std::runtime_error{"invalid coordinated peer plan"};
      }
      const auto expected = p2p::peer_id::from_string(plan.at("peer-id"));
      const auto remote = p2p::parse_endpoint(plan.at("addr"));
      if (expected == node.local_peer() || !remote.is_direct_tcp() || !remote.peer || *remote.peer != expected ||
          remote.transport.host_type != p2p::endpoint::host_kind::ip4 || remote.transport.port == 0) {
         throw std::runtime_error{"coordinated target must be an identity-bound concrete IPv4 TCP listener"};
      }
      const auto address = boost::asio::ip::make_address(remote.transport.host);
      if (address.is_unspecified() || address.is_multicast()) {
         throw std::runtime_error{"coordinated target is not a concrete unicast listener"};
      }
      require_fresh_identify(node, expected);
      {
         const auto lock = std::scoped_lock{handler_mutex};
         handler_peer = expected;
         handler_local = local;
         handler_remote = remote;
      }
      admitted = true;
      write(args.at("result-file"), result(args, node, "started", receipt, false, false, admitted));
      auto strand = boost::asio::make_strand(runtime.context());
      auto cancel = std::make_shared<boost::asio::cancellation_signal>();
      auto connected =
          boost::asio::co_spawn(strand,
                                node.async_connect_coordinated(
                                    remote, {.expected_peer = expected,
                                             .local_source = *local,
                                             .side = source ? p2p::node::coordinated_connect_options::role::initiator
                                                            : p2p::node::coordinated_connect_options::role::responder,
                                             .timeout = budget}),
                                boost::asio::bind_cancellation_slot(cancel->slot(), boost::asio::use_future));
      auto requested_cancel = false;
      while (connected.wait_for(10ms) != std::future_status::ready) {
         if (!requested_cancel && std::filesystem::exists(args.at("stop-file"))) {
            requested_cancel = true;
            boost::asio::post(strand, [cancel] { cancel->emit(boost::asio::cancellation_type::all); });
         }
      }
      native_joined = true;
      const auto session_info = connected.get(); // API completion includes native losing-worker join.
      if (session_info.remote_peer != expected || session_info.path != p2p::path::kind::direct) {
         throw std::runtime_error{"coordinated API returned another peer/path"};
      }
      const auto identify_deadline = std::chrono::steady_clock::now() + budget;
      while (true) {
         const auto snapshot = node.diagnostics();
         if (snapshot.sessions.size() != 1 || !snapshot.sessions.front().identify_error.empty()) {
            throw std::runtime_error{"coordinated winner is ambiguous or Identify failed"};
         }
         if (snapshot.sessions.front().identify_state == p2p::identify::state::identified) {
            break;
         }
         if (std::chrono::steady_clock::now() >= identify_deadline || std::filesystem::exists(args.at("stop-file"))) {
            throw std::runtime_error{"coordinated winner Identify deadline/cancellation"};
         }
         std::this_thread::sleep_for(10ms);
      }
      const auto captured = winner(node, expected, remote, *local, source);
      dial_gate->seal();
      receipt = connection_receipt(node, captured, source);
      status = "connected";
      write(args.at("result-file"), result(args, node, status, receipt, false, native_joined, admitted));
      const auto stop_deadline = std::chrono::steady_clock::now() + 30s;
      auto probed = false;
      auto last_sequence = std::string{"1"};
      while (std::chrono::steady_clock::now() < stop_deadline) {
         if (std::filesystem::exists(args.at("stop-file"))) {
            if (!probed) {
               throw std::runtime_error{"coordinated actor stopped before retained-connection probe"};
            }
            status = "ok";
            break;
         }
         const auto control = fields(args.at("control-file"));
         if (control.size() != 3 || control.at("case-token") != args.at("case-token") ||
             (control.at("sequence") != "1" && control.at("sequence") != "2") ||
             control.at("sequence") < last_sequence ||
             (control.at("sequence") == "1" && control.at("action") != "start") ||
             (control.at("sequence") == "2" && (!source || control.at("action") != "probe"))) {
            throw std::runtime_error{"coordinated probe sequence/token mismatch"};
         }
         last_sequence = control.at("sequence");
         if (control.at("sequence") == "2" && !probed) {
            if (!source || control.at("action") != "probe") {
               throw std::runtime_error{"probe is initiator-only"};
            }
            require_same_connection(node, captured);
            const auto attempts_before = native_dial_rejections(node);
            auto stream = std::make_shared<p2p::stream>(forge::asio::blocking::run(
                runtime,
                node.async_open_protocol_stream(expected, {.value = "/forge/interop/relay-echo/1"},
                                                {.allow_relay = false, .timeout = budget, .allow_hole_punch = false})));
            require_same_connection(node, captured);
            if (native_dial_rejections(node) != attempts_before) {
               throw std::runtime_error{"retained-connection probe unexpectedly dialed"};
            }
            const auto challenge = "coordinated:" + args.at("case-token");
            const auto request = std::vector<std::uint8_t>{challenge.begin(), challenge.end()};
            auto exchanged = boost::asio::co_spawn(runtime.context(), probe_payload(support, stream, request),
                                                   boost::asio::use_future);
            const auto io_deadline = std::chrono::steady_clock::now() + budget;
            try {
               while (exchanged.wait_for(10ms) != std::future_status::ready) {
                  if (std::chrono::steady_clock::now() >= io_deadline ||
                      std::filesystem::exists(args.at("stop-file"))) {
                     stream->request_cancel();
                  }
               }
            } catch (...) {
               const auto polling_failure = std::current_exception();
               stream->request_cancel();
               try {
                  static_cast<void>(exchanged.get());
               } catch (...) {
               }
               std::rethrow_exception(polling_failure);
            }
            const auto observed = exchanged.get();
            if (observed.payload != request) {
               throw std::runtime_error{"coordinated native Yamux echo failed"};
            }
            require_same_connection(node, captured);
            static_cast<void>(endpoint_connection_receipt(node, expected, *stream, captured.direction));
            receipt = connection_receipt(
                node, captured, source,
                application_receipt(captured, *stream, observed.request_frame, observed.response_frame,
                    attempts_before, native_dial_rejections(node)));
            forge::asio::blocking::run(runtime, stream->async_close());
            probed = true;
         }
         if (!source && !probed) {
            const auto lock = std::scoped_lock{handler_mutex};
            if (handler_failure) {
               std::rethrow_exception(handler_failure);
            }
            if (!handler_receipt.empty()) {
               receipt = handler_receipt;
               probed = true;
            }
         }
         if (probed && status != "exchanged") {
            status = "exchanged";
            write(args.at("result-file"), result(args, node, status, receipt, false, native_joined, admitted));
         }
         std::this_thread::sleep_for(10ms);
      }
      if (status != "ok") {
         throw std::runtime_error{"coordinated probe/stop gate expired"};
      }
   } catch (...) {
      failure = std::current_exception();
      status = "error";
   }
   auto joined = false;
   try {
      forge::asio::blocking::run(runtime, node.async_stop());
      joined = true;
   } catch (...) {
      if (!failure) {
         failure = std::current_exception();
      }
      status = "error";
   }
   auto error = std::string{};
   if (failure) {
      try {
         std::rethrow_exception(failure);
      } catch (const std::exception& exception) {
         error = exception.what();
      } catch (...) {
         error = "unknown coordinated actor failure";
      }
   }
   write(args.at("result-file"), result(args, node, status, receipt, joined, native_joined, admitted, error));
   if (failure) {
      std::rethrow_exception(failure);
   }
   return 0;
}

} // namespace forge::test::libp2p_interop
