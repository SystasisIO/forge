#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/asio/awaitable.hpp>

import forge.asio.blocking;
import forge.asio.runtime;
import forge.codec.hex;
import forge.crypto.asymmetric;
import forge.crypto.asymmetric.ed25519;
import forge.crypto.digest.sha256;
import forge.multiformats.multiaddr;
import forge.net.p2p.dht;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.envelope;
import forge.net.p2p.exceptions;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.rendezvous;
import forge.net.p2p.stream;

#include "forge_connection_fixture.hxx"
#include "forge_private_profile_fixture.hxx"

namespace forge::test::libp2p_interop {
namespace {
using namespace std::chrono_literals;
constexpr auto echo = "/forge/interop/relay-echo/1";

void validate_identify_record(const forge::net::p2p::signed_envelope& envelope,
                              const forge::net::p2p::peer_id& peer) {
   const auto standard_type = std::array<std::uint8_t, 2>{0x03, 0x01};
   auto record = forge::net::p2p::rendezvous::peer_record{};
   if (std::ranges::equal(envelope.payload_type, standard_type)) {
      envelope.verify("libp2p-peer-record", peer);
      record = forge::net::p2p::rendezvous::codec::decode_peer_record(envelope.payload);
   } else {
      record = forge::net::p2p::rendezvous::codec::open_peer_record(envelope, peer);
   }
   if (record.peer != peer || record.peer != envelope.signer() || record.sequence == 0 ||
       record.endpoints.empty()) {
      throw std::runtime_error{"private Identify signed peer record mismatch"};
   }
}

std::string quote(std::string_view value) {
   constexpr auto digits = "0123456789abcdef";
   auto out = std::string{"\""};
   for (const auto character : value) {
      const auto byte = static_cast<unsigned char>(character);
      if (byte < 32) { out += "\\u00"; out += digits[byte >> 4]; out += digits[byte & 15]; }
      else { if (character == '\\' || character == '"') { out += '\\'; } out += character; }
   }
   return out + '"';
}

std::string listener_diagnostics(const forge::net::p2p::diagnostics::snapshot& snapshot) {
   auto out = std::string{"{\"source\":\"forge.node.diagnostics\",\"sessions_opened\":"} +
       std::to_string(snapshot.metrics.sessions_opened) + ",\"sessions_closed\":" +
       std::to_string(snapshot.metrics.sessions_closed) + ",\"sessions_pruned\":" +
       std::to_string(snapshot.metrics.sessions_pruned) + ",\"sessions\":[";
   for (auto index = std::size_t{}; index < snapshot.sessions.size(); ++index) {
      const auto& session = snapshot.sessions[index];
      if (index) { out += ','; }
      out += "{\"connection_id\":" + std::to_string(session.id) + ",\"closed\":" +
          (session.closed ? "true" : "false") + ",\"identify_state\":" +
          std::to_string(static_cast<unsigned>(session.identify_state)) + ",\"identify_error\":" +
          quote(session.identify_error.substr(0, 512)) + "}";
   }
   return out + "]}";
}

void write(const std::string& path, const std::string& json) {
   const auto target = std::filesystem::path{path};
   std::filesystem::create_directories(target.parent_path());
   const auto pending = target.string() + ".private.tmp";
   { auto output = std::ofstream{pending}; output << json << '\n'; if (!output) { throw std::runtime_error{"private evidence write failed"}; } }
   std::filesystem::rename(pending, target);
}

std::vector<std::uint8_t> frame(std::span<const std::uint8_t> bytes) {
   auto out = std::vector<std::uint8_t>{};
   auto size = bytes.size();
   while (size >= 128) { out.push_back(static_cast<std::uint8_t>((size & 127) | 128)); size >>= 7; }
   out.push_back(static_cast<std::uint8_t>(size)); out.insert(out.end(), bytes.begin(), bytes.end()); return out;
}

boost::asio::awaitable<std::vector<std::uint8_t>> read_public(forge::net::p2p::stream& stream, bool raw = false) {
   auto bytes = std::vector<std::uint8_t>{};
   for (;;) {
      auto part = co_await stream.async_read();
      if (part.empty() || bytes.size() + part.size() > 8196) { throw std::runtime_error{"private response truncated or oversized"}; }
      bytes.insert(bytes.end(), part.begin(), part.end());
      if (raw) {
         if (bytes.size() > 32) { throw std::runtime_error{"Ping response exceeds 32 bytes"}; }
         if (bytes.size() == 32) { co_return bytes; }
         continue;
      }
      auto size = std::size_t{0}; auto prefix = std::size_t{0}; auto complete = false;
      for (const auto byte : bytes) {
         if (prefix == 3) { throw std::runtime_error{"private response header exceeds bound"}; }
         size |= std::size_t{byte & 127U} << (7 * prefix++);
         if (!(byte & 128U)) {
            if (size == 0 || size > 8192 || (prefix > 1 && byte == 0)) { throw std::runtime_error{"invalid private response frame"}; }
            complete = true; break;
         }
      }
      if (complete && bytes.size() > prefix + size) { throw std::runtime_error{"private response contains trailing frame bytes"}; }
      if (complete && bytes.size() == prefix + size) { co_return bytes; }
   }
}

std::string receipt(const std::vector<std::uint8_t>& bytes, bool raw) {
   return "{\"framed_hex\":\"" + forge::codec::hex::encode(bytes) + "\",\"raw\":" + (raw ? "true" : "false") +
       ",\"read\":{\"framed_bytes\":" + std::to_string(bytes.size()) + ",\"framed_sha256\":\"" +
       forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{bytes}).str() +
       "\",\"frames\":1,\"complete_frames\":true,\"invalid_or_over_limit\":false}}";
}

std::string protocol(const std::string& name) {
   if (name == "ping_private_tcp_yamux_pnet") { return "/ipfs/ping/1.0.0"; }
   if (name == "identify_private_tcp_yamux_pnet") { return "/ipfs/id/1.0.0"; }
   if (name == "kademlia_amino_private_tcp_yamux_pnet") { return "/ipfs/kad/1.0.0"; }
   if (name == "rendezvous_rust_private_tcp_yamux_pnet") { return "/rendezvous/1.0.0"; }
   if (name == "tcp_yamux_private_pnet" || name == "multistream_select_private_pnet" ||
       name == "noise_identity_private_pnet" || name == "tls_identity_private_pnet" || name.starts_with("inline_muxer_")) { return echo; }
   throw std::runtime_error{"unknown private contract"};
}

std::string base(const std::map<std::string, std::string>& args, const forge::net::p2p::node& value) {
   const auto fingerprint = args.contains("pnet-fingerprint") ? args.at("pnet-fingerprint") : std::string{};
   return std::string{"{\"implementation\":\"forge\",\"role\":\""} + (args.at("command") == "listen" ? "listener" : "dialer") +
       "\",\"scenario\":\"" + args.at("scenario") + "\",\"status\":\"ok\",\"local_peer_id\":\"" +
       value.local_peer().to_string() + "\",\"pnet_fingerprint\":\"" + fingerprint + "\"";
}

std::string rejection(const std::map<std::string, std::string>& args, const forge::net::p2p::node& value) {
   const auto metrics = value.metrics();
   const auto dialer = args.at("command") == "dial";
   const auto attempted = dialer ? metrics.path_direct_attempts : metrics.handshakes_completed + metrics.handshakes_failed;
   return base(args, value).replace(base(args, value).find("\"status\":\"ok\""), 13, "\"status\":\"rejected\"") +
       ",\"control_kind\":\"" + args.at("pnet-control") + "\",\"correlation_token\":\"" + args.at("pnet-correlation") +
       "\",\"counter_source\":\"forge.node.metrics\",\"attempted_connections\":" + std::to_string(attempted) +
       ",\"established_connections\":" + std::to_string(metrics.sessions_opened) +
       ",\"identify_streams\":" + std::to_string(metrics.protocol_streams_opened + metrics.protocol_streams_accepted) +
       ",\"application_streams\":" + std::to_string(metrics.protocol_streams_opened + metrics.protocol_streams_accepted) +
       ",\"rejected_before_identify\":" + (metrics.sessions_opened == 0 && metrics.protocol_streams_opened == 0 && metrics.protocol_streams_accepted == 0 ? "true" : "false") +
       (dialer ? ",\"expected_peer_id\":\"" + args.at("peer-id") + "\"" : "") + "}";
}
} // namespace

int private_profile_self_test() {
   namespace p2p = forge::net::p2p;
   namespace ed25519 = forge::crypto::asymmetric::ed25519;
   const auto private_key = ed25519::private_key::regenerate(ed25519::private_key_secret{});
   const auto public_bytes = private_key.get_public_key().serialize();
   const auto key = p2p::public_key{.type = p2p::public_key::type::ed25519,
                                  .data = {public_bytes.begin(), public_bytes.end()}};
   const auto peer = p2p::make_peer_id(key);
   const auto signer = forge::crypto::asymmetric::private_key{
       forge::crypto::asymmetric::private_key::storage_type{private_key}};
   const auto record = p2p::rendezvous::peer_record{
       .peer = peer, .endpoints = {forge::multiformats::multiaddr::parse("/ip4/127.0.0.1/tcp/4001")},
       .sequence = 1};
   const auto standard_type = std::array<std::uint8_t, 2>{0x03, 0x01};
   const auto payload = p2p::rendezvous::codec::encode_peer_record(record);
   const auto standard = p2p::signed_envelope::seal(key, signer, "libp2p-peer-record", standard_type, payload);
   const auto legacy = p2p::rendezvous::codec::seal_peer_record(record, key, signer);
   validate_identify_record(standard, peer);
   validate_identify_record(legacy, peer);
   const auto reject = [&](const p2p::signed_envelope& envelope, const p2p::peer_id& expected) {
      try { validate_identify_record(envelope, expected); }
      catch (const std::exception&) { return; }
      throw std::runtime_error{"private Identify negative self-test accepted an invalid record"};
   };
   reject(p2p::signed_envelope::seal(key, signer, "wrong-domain", standard_type, payload), peer);
   reject(p2p::signed_envelope::seal(key, signer, "wrong-domain", legacy.payload_type, payload), peer);
   const auto unknown_type = std::array<std::uint8_t, 1>{0x7f};
   reject(p2p::signed_envelope::seal(key, signer, "libp2p-peer-record", unknown_type, payload), peer);
   reject(p2p::signed_envelope::seal(key, signer, "libp2p-routing-state", unknown_type, payload), peer);
   auto tampered = standard;
   tampered.signature.front() ^= 1;
   reject(tampered, peer);
   auto other_secret = ed25519::private_key_secret{};
   other_secret.front() = 1;
   const auto other_bytes = ed25519::private_key::regenerate(other_secret).get_public_key().serialize();
   const auto other_peer = p2p::make_peer_id({.type = p2p::public_key::type::ed25519,
                                            .data = {other_bytes.begin(), other_bytes.end()}});
   reject(standard, other_peer);
   auto changed = record;
   changed.peer = other_peer;
   reject(p2p::signed_envelope::seal(key, signer, "libp2p-peer-record", standard_type,
                                   p2p::rendezvous::codec::encode_peer_record(changed)), peer);
   changed = record;
   changed.sequence = 0;
   reject(p2p::signed_envelope::seal(key, signer, "libp2p-peer-record", standard_type,
                                   p2p::rendezvous::codec::encode_peer_record(changed)), peer);
   changed = record;
   changed.endpoints.clear();
   reject(p2p::signed_envelope::seal(key, signer, "libp2p-peer-record", standard_type,
                                   p2p::rendezvous::codec::encode_peer_record(changed)), peer);
   return 0;
}

int run_private_profile_fixture(const std::map<std::string, std::string>& args, const private_profile_support& support) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto value = forge::net::p2p::node{runtime, support.make_options(args)};
   support.register_echo(value);
   forge::asio::blocking::run(runtime, value.async_hydrate_peer_state());
   forge::asio::blocking::run(runtime, value.async_listen(forge::net::p2p::endpoint{
       .transport = {.host_type = forge::net::p2p::endpoint::host_kind::ip4,
                     .protocol = forge::net::p2p::endpoint::protocol_kind::tcp, .host = "127.0.0.1", .port = 0}}));
   const auto controlled = args.contains("pnet-control");
   auto result = std::string{};
   const auto open = forge::net::p2p::node::open_options{.allow_relay = false, .allow_hole_punch = false};
   if (args.at("command") == "listen") {
      const auto local = value.local_endpoint();
      if (!local) { throw std::runtime_error{"private listener lacks endpoint"}; }
      auto advertised = *local;
      if (advertised.peer && *advertised.peer != value.local_peer()) {
         throw std::runtime_error{"private listener endpoint has a conflicting peer"};
      }
      advertised.peer = value.local_peer();
      write(args.at("ready-file"), "{\"implementation\":\"forge\",\"role\":\"listener\",\"status\":\"ready\",\"peer_id\":\"" +
          value.local_peer().to_string() + "\",\"listen_addrs\":[\"" + advertised.to_string() + "\"]}");
      auto last_diagnostics = std::string{};
      while (!std::filesystem::exists(args.at("stop-file"))) {
         const auto snapshot = value.diagnostics();
         if (!controlled && result.empty()) {
            const auto observed = listener_diagnostics(snapshot);
            if (last_diagnostics != observed) {
               last_diagnostics = observed;
               // Separate diagnostic artifact, never a completion barrier or
               // acceptance result. Preserve Identify failure before cleanup.
               write(args.at("result-file") + ".connection-diagnostics.json", observed);
            }
         }
         if (!controlled && result.empty() && snapshot.sessions.size() == 1 &&
             snapshot.sessions.front().identify_state == forge::net::p2p::identify::state::identified) {
            const auto peer = snapshot.sessions.front().remote_peer;
            const auto& endpoint = snapshot.sessions.front().remote_endpoint;
            if (!endpoint) { throw std::runtime_error{"private reverse probe lacks retained remote endpoint"}; }
            const auto captured = capture_identified_connection(value, peer, *endpoint,
                forge::net::p2p::diagnostics::session_direction::inbound);
            // A reverse native echo on the retained inbound session supplies
            // actual stream.authentication(), not a configured security label.
            auto stream = forge::asio::blocking::run(runtime, value.async_open_protocol_stream(peer, {.value = echo}, open));
            const auto connection = endpoint_connection_receipt(value, peer, stream, forge::net::p2p::diagnostics::session_direction::inbound);
            const auto payload = std::vector<std::uint8_t>{'p','r','i','v','a','t','e','-','r','e','c','e','i','p','t'};
            const auto request = frame(payload);
            forge::asio::blocking::run(runtime, stream.async_write(request));
            const auto response = forge::asio::blocking::run(runtime, read_public(stream));
            if (request != response) { throw std::runtime_error{"private reverse probe mismatch"}; }
            forge::asio::blocking::run(runtime, stream.async_close());
            require_same_connection(value, captured);
            result = base(args, value) + ",\"connection_receipt\":" + connection + ",\"probe\":" + receipt(response, false) + "}";
            write(args.at("result-file"), result);
         }
         std::this_thread::sleep_for(25ms);
      }
      if (controlled) { result = rejection(args, value); }
      if (result.empty()) {
         forge::asio::blocking::run(runtime, value.async_stop());
         throw std::runtime_error{"private listener lacks reverse stream completion; see connection-diagnostics.json"};
      }
   } else {
      const auto peer = forge::net::p2p::peer_id::from_string(args.at("peer-id"));
      const auto remote = forge::net::p2p::parse_endpoint(args.at("addr"));
      require_fresh_identify(value, peer);
      try {
         (void)forge::asio::blocking::run(runtime, value.async_connect(remote, {.expected_peer = peer, .allow_relay = false, .allow_hole_punch = false}));
      } catch (const std::exception&) {
         if (!controlled) { throw; }
         result = rejection(args, value);
      }
      if (result.empty()) {
         if (controlled) { throw std::runtime_error{"private negative control authenticated a peer"}; }
         const auto captured = capture_identified_connection(value, peer, remote);
         const auto id = protocol(args.at("scenario"));
         if (id == "/ipfs/id/1.0.0") {
            auto rejected = false;
            try {
               auto unused = forge::asio::blocking::run(runtime, value.async_open_protocol_stream(
                   peer, {.value = "/forge/interop/private-unknown/1"}, open));
            } catch (const forge::net::p2p::exceptions::unsupported_protocol&) { rejected = true; }
            if (!rejected) { throw std::runtime_error{"private Identify barrier protocol was not rejected"}; }
            require_same_connection(value, captured);
         }
         auto request = std::vector<std::uint8_t>{};
         const auto raw = id == "/ipfs/ping/1.0.0";
         if (id == echo) {
            const auto text = args.at("payload"); request = frame(std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
         } else if (raw) { for (auto i = 1; i <= 32; ++i) { request.push_back(static_cast<std::uint8_t>(i)); } }
         else if (id == "/ipfs/kad/1.0.0") {
            request = forge::net::p2p::dht::codec::encode({.type = forge::net::p2p::dht::message_type::find_node,
                .key_value = forge::net::p2p::make_dht_key(peer)}, forge::net::p2p::amino_v1());
         } else if (id == "/rendezvous/1.0.0") {
            const auto registration = forge::asio::blocking::run(runtime, value.async_rendezvous_register(peer, {
                .namespace_name = "forge.discovery", .signed_peer_record = support.signed_record(value), .ttl = 7200s}));
            if (registration.status_value != forge::net::p2p::rendezvous::status::ok || registration.ttl != 7200s) { throw std::runtime_error{"private rendezvous registration rejected"}; }
            request = forge::net::p2p::rendezvous::codec::encode(forge::net::p2p::rendezvous::message{
                .type = forge::net::p2p::rendezvous::message_type::discover,
                .discover_value = forge::net::p2p::rendezvous::discover_request{.namespace_name = "forge.discovery", .limit = 10}});
         }
         auto stream = forge::asio::blocking::run(runtime, value.async_open_protocol_stream(peer, {.value = id}, open));
         const auto connection = endpoint_connection_receipt(value, peer, stream);
         if (!request.empty()) { forge::asio::blocking::run(runtime, stream.async_write(request)); }
         const auto response = forge::asio::blocking::run(runtime, read_public(stream, raw));
         if ((id == echo || raw) && request != response) { throw std::runtime_error{"private response differs from request"}; }
         auto validation = std::string{};
         if (id == "/ipfs/id/1.0.0") {
            auto prefix = std::size_t{0}; while (response.at(prefix++) & 128U) {}
            const auto document = forge::net::p2p::identify::decode(std::span<const std::uint8_t>{response}.subspan(prefix));
            forge::net::p2p::validate_public_key(forge::net::p2p::decode_public_key(document.public_key), peer);
            const auto envelope = forge::net::p2p::signed_envelope::decode(document.signed_peer_record);
            validate_identify_record(envelope, peer);
            validation = ",\"identify_verified\":true";
         } else if (id == "/rendezvous/1.0.0") {
            const auto message = forge::net::p2p::rendezvous::codec::decode(response);
            if (message.type != forge::net::p2p::rendezvous::message_type::discover_response || !message.discover_response_value ||
                message.discover_response_value->status_value != forge::net::p2p::rendezvous::status::ok ||
                message.discover_response_value->registrations.size() != 1 || message.discover_response_value->cookie.empty()) {
               throw std::runtime_error{"private Rendezvous lacks successful one-record discovery"};
            }
            const auto& found = message.discover_response_value->registrations.front();
            const auto record = forge::net::p2p::rendezvous::codec::open_peer_record(
                forge::net::p2p::signed_envelope::decode(found.signed_peer_record), value.local_peer());
            if (found.namespace_name != "forge.discovery" || found.ttl != 7200s || record.peer != value.local_peer() ||
                record.sequence == 0 || record.endpoints.empty()) { throw std::runtime_error{"private Rendezvous record mismatch"}; }
            validation = ",\"rendezvous_verified\":true";
         }
         forge::asio::blocking::run(runtime, stream.async_close());
         require_same_connection(value, captured);
         result = base(args, value) + ",\"connection_receipt\":" + connection + ",\"protocol\":\"" + id +
             "\",\"stream_closed\":true,\"response\":" + receipt(response, raw) +
             (request.empty() ? "" : ",\"request\":" + receipt(request, raw)) + validation;
         if (args.at("scenario") == "multistream_select_private_pnet") {
            auto rejected = false;
            try { auto unused = forge::asio::blocking::run(runtime, value.async_open_protocol_stream(peer, {.value = "/forge/interop/private-unknown/1"}, open)); }
            catch (const forge::net::p2p::exceptions::unsupported_protocol&) { rejected = true; }
            if (!rejected) { throw std::runtime_error{"private unknown protocol was not rejected"}; }
            result += ",\"unknown_protocol_rejected\":true";
         }
         result += "}";
      }
      write(args.at("result-file"), result);
      // The runner releases this owner only after counterpart capture, avoiding
      // a timing-based guess about whether the listener observed the session.
      while (!controlled && !std::filesystem::exists(args.at("stop-file"))) { std::this_thread::sleep_for(25ms); }
   }
   forge::asio::blocking::run(runtime, value.async_stop());
   write(args.at("result-file"), result);
   return 0;
}
} // namespace forge::test::libp2p_interop
