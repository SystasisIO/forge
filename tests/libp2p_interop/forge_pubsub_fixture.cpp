#include <algorithm>
#include <array>
#include <chrono>
#include <coroutine>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include <boost/asio/awaitable.hpp>

import forge.asio.blocking;
import forge.asio.runtime;
import forge.codec.hex;
import forge.codec.json;
import forge.crypto.digest.sha256;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.node;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.stream;
import forge.net.pnet.protector;
import forge.variant.value;
import forge.variant.containers;

#include "forge_pubsub_fixture.hxx"

namespace forge::test::libp2p_interop {
namespace {
using namespace std::chrono_literals;
namespace p2p = forge::net::p2p;
namespace pubsub = p2p::pubsub;

constexpr std::size_t trace_limit = 16 * 1024 * 1024;
constexpr std::size_t event_limit = 64 * 1024;
constexpr std::size_t result_reserve = 64 * 1024;
constexpr std::size_t error_limit = 512;
constexpr std::size_t peer_limit = 512;
constexpr std::size_t line_limit = 16 * 1024;
constexpr std::size_t command_limit = 64;
constexpr std::size_t control_limit = command_limit * (line_limit + 1);
constexpr std::size_t trace_budget = trace_limit - result_reserve - 1;
static_assert(result_reserve >= 6 * (error_limit + peer_limit + 32 + 16) + 1024);

std::string bounded_error(std::string_view value) {
   auto text = std::string{};
   text.reserve(std::min(value.size(), error_limit));
   for (const unsigned char byte : value.substr(0, error_limit)) {
      text.push_back(byte >= 32 && byte <= 126 ? static_cast<char>(byte) : '?');
   }
   return text;
}
} // namespace

std::string forge_pubsub_fixture::ready_address(p2p::endpoint listener, const p2p::peer_id& local_peer) {
   if (!listener.is_direct_tcp() && !listener.is_direct_quic()) {
      throw std::runtime_error{"PubSub readiness requires a direct native listener"};
   }
   if (!p2p::valid_peer_id(local_peer)) { throw std::runtime_error{"invalid PubSub local listener identity"}; }
   if (listener.peer && *listener.peer != local_peer) {
      throw std::runtime_error{"PubSub listener peer mismatch"};
   }
   listener.peer = local_peer;
   return listener.to_string();
}

void forge_pubsub_fixture::canonical_paths(arguments& args, bool private_profile) {
   auto seen = std::set<std::filesystem::path>{};
   for (const auto flag : {"ready-file", "result-file", "control-file", "stop-file", "store-dir"}) {
      const auto path = std::filesystem::weakly_canonical(std::filesystem::absolute(args.at(flag)));
      if (!seen.insert(path).second) { throw std::runtime_error{"PubSub I/O paths must be distinct"}; }
      args.at(flag) = path.string();
   }
   for (const auto flag : {"ready-file", "result-file", "control-file", "stop-file"}) {
      const auto path = std::filesystem::path{args.at(flag)};
      if (!std::filesystem::is_directory(path.parent_path())) {
         throw std::runtime_error{"PubSub I/O parent must be an existing directory"};
      }
      const auto status = std::filesystem::symlink_status(path);
      if (flag == std::string_view{"control-file"}) {
         if (status.type() != std::filesystem::file_type::not_found &&
             (!std::filesystem::is_regular_file(status) || std::filesystem::file_size(path) != 0)) {
            throw std::runtime_error{"PubSub control must be absent or a fresh empty regular file"};
         }
      } else if (status.type() != std::filesystem::file_type::not_found) {
         throw std::runtime_error{"PubSub ready/result/stop files must be fresh"};
      }
   }
   for (const auto flag : {"ready-file", "result-file"}) {
      const auto temporary = std::filesystem::weakly_canonical(args.at(flag) + ".tmp");
      if (!seen.insert(temporary).second ||
          std::filesystem::symlink_status(temporary).type() != std::filesystem::file_type::not_found) {
         throw std::runtime_error{"PubSub temporary paths must be distinct and fresh"};
      }
   }
   const auto store = std::filesystem::symlink_status(args.at("store-dir"));
   if (store.type() != std::filesystem::file_type::not_found && !std::filesystem::is_directory(store)) {
      throw std::runtime_error{"PubSub store path must be a directory"};
   }
   if (private_profile) {
      const auto key = std::filesystem::weakly_canonical(std::filesystem::absolute(args.at("pnet-key-file")));
      if (!seen.insert(key).second || !std::filesystem::is_regular_file(key) || std::filesystem::file_size(key) > 1024) {
         throw std::runtime_error{"PubSub private key path must be distinct, regular and bounded"};
      }
      args.at("pnet-key-file") = key.string();
   }
}

namespace {
bool stop_requested(const std::filesystem::path& path) {
   const auto status = std::filesystem::symlink_status(path);
   if (status.type() == std::filesystem::file_type::not_found) { return false; }
   if (!std::filesystem::is_regular_file(status)) { throw std::runtime_error{"invalid PubSub stop file"}; }
   return true;
}
} // namespace

forge_pubsub_fixture::control_reader::control_reader(const std::filesystem::path& path)
    : _present{std::filesystem::is_regular_file(std::filesystem::symlink_status(path))} {}

void forge_pubsub_fixture::control_reader::poll(const std::filesystem::path& path,
    const std::function<void(const forge::variant&, std::uint64_t)>& apply, bool finishing) {
   const auto status = std::filesystem::symlink_status(path);
   if (status.type() == std::filesystem::file_type::not_found) {
      if (_present) { throw std::runtime_error{"PubSub control file disappeared"}; }
      return;
   }
   if (!std::filesystem::is_regular_file(status)) { throw std::runtime_error{"PubSub control must remain a regular file"}; }
   _present = true;
   const auto size = std::filesystem::file_size(path);
   if (size > control_limit || size < _observed) { throw std::runtime_error{"PubSub control truncated or over bound"}; }
   auto input = std::ifstream{path, std::ios::binary};
   if (!input) { throw std::runtime_error{"cannot read PubSub control"}; }
   auto buffer = std::array<char, line_limit>{};
   auto prefix = forge::crypto::digest::sha256::encoder{};
   auto complete = forge::crypto::digest::sha256::encoder{};
   const auto grew = size != _observed;
   auto read = [&](std::uintmax_t remaining, auto consume) {
      while (remaining != 0) {
         const auto count = static_cast<std::size_t>(std::min<std::uintmax_t>(remaining, buffer.size()));
         input.read(buffer.data(), static_cast<std::streamsize>(count));
         if (input.gcount() != static_cast<std::streamsize>(count) || input.bad()) {
            throw std::runtime_error{"PubSub control changed or read failed"};
         }
         consume(std::string_view{buffer.data(), count});
         remaining -= count;
      }
   };
   // Verify the entire previously observed prefix before executing appended commands.
   read(_observed, [&](std::string_view bytes) {
      prefix.write(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
      if (grew) { complete.write(bytes.data(), static_cast<std::uint32_t>(bytes.size())); }
   });
   if (_hash && prefix.result() != *_hash) { throw std::runtime_error{"PubSub control prefix was rewritten"}; }
   if (finishing) {
      if (grew || !_pending.empty()) { throw std::runtime_error{"unprocessed/incomplete PubSub control at stop"}; }
      return;
   }
   if (!grew) { return; }
   if (_admission_closed) { throw std::runtime_error{"PubSub command admission closed after prepare_shutdown"}; }
   auto commands = std::vector<std::pair<forge::variant, std::uint64_t>>{};
   read(size - _observed, [&](std::string_view bytes) {
      complete.write(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
      for (const auto byte : bytes) {
         if (_sequence == command_limit) { throw std::runtime_error{"PubSub command exceeds count bound"}; }
         if (byte == '\n') {
            auto decoded = forge::codec::json::read_value(_pending, {.max_depth = 8});
            if (!decoded.ok()) { throw std::runtime_error{"invalid PubSub control JSON"}; }
            commands.emplace_back(std::move(decoded.value), ++_sequence);
            _pending.clear();
         } else {
            if (_pending.size() == line_limit) { throw std::runtime_error{"PubSub command exceeds line bound"}; }
            _pending.push_back(byte);
         }
      }
   });
   _observed = size;
   _hash = complete.result();
   for (std::size_t index = 0; index != commands.size(); ++index) {
      const auto& [command, sequence] = commands[index];
      const auto preparing = command.is_object() && command.get_object().contains("kind") &&
          command["kind"].is_string() && command["kind"].get_string() == "prepare_shutdown";
      if (preparing && (index + 1 != commands.size() || !_pending.empty())) {
         throw std::runtime_error{"prepare_shutdown with queued/incomplete command"};
      }
      apply(command, sequence);
      if (preparing) { _admission_closed = true; }
   }
}

namespace {
std::string encode(const forge::variant& value, std::size_t limit) {
   auto encoded = forge::codec::json::write_value(value, {.max_bytes = limit});
   if (!encoded.ok()) { throw std::runtime_error{"cannot encode bounded PubSub evidence"}; }
   return std::move(encoded.text);
}
} // namespace

void forge_pubsub_fixture::write_atomic(const std::filesystem::path& path, const forge::variant& value) {
   const auto text = encode(value, trace_limit - 1);
   const auto temporary = std::filesystem::path{path.string() + ".tmp"};
   if (std::filesystem::symlink_status(temporary).type() != std::filesystem::file_type::not_found) {
      throw std::runtime_error{"unexpected PubSub evidence temporary file"};
   }
   const auto target = std::filesystem::symlink_status(path);
   if (target.type() != std::filesystem::file_type::not_found && !std::filesystem::is_regular_file(target)) {
      throw std::runtime_error{"invalid PubSub evidence target"};
   }
   auto output = std::ofstream{temporary};
   output << text << '\n';
   output.close();
   if (!output) { throw std::runtime_error{"cannot write PubSub evidence"}; }
   std::filesystem::rename(temporary, path);
}

namespace {
std::string outcome(pubsub::validation_result result) {
   switch (result) {
   case pubsub::validation_result::accept: return "accept";
   case pubsub::validation_result::reject: return "reject";
   case pubsub::validation_result::ignore: return "ignore";
   case pubsub::validation_result::retry: return "retry";
   }
   throw std::runtime_error{"invalid native validation result"};
}
} // namespace

forge_pubsub_fixture::forge_pubsub_fixture(std::string token, std::string actor)
    : _token{std::move(token)}, _actor{std::move(actor)} {
   if (_token.size() != 32 || !std::ranges::all_of(_token, [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
       }) || (_actor != "victim" && _actor != "offender" && _actor != "replacement" && _actor != "sink")) {
      throw std::runtime_error{"invalid PubSub actor/token"};
   }
}

void forge_pubsub_fixture::record_locked(std::string_view kind, std::string_view source,
                                         forge::mutable_variant_object fields) {
   if (_overflow || !_capture_error.empty()) { return; }
   if (_events.size() >= 2048) { _overflow = true; return; }
   fields("sequence", static_cast<std::uint64_t>(_events.size() + 1))
       ("mono_ns", static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
           std::chrono::steady_clock::now().time_since_epoch()).count()))
       ("kind", std::string{kind})("source", std::string{source});
   auto event = forge::variant{std::move(fields)};
   const auto encoded = forge::codec::json::write_value(event, {.max_bytes = event_limit});
   if (!encoded.ok()) {
      _overflow = true;
      _capture_error = "cannot encode bounded PubSub event";
      return;
   }
   const auto bytes = encoded.text.size() + (_events.empty() ? 0u : 1u);
   if (bytes > trace_budget - _trace_bytes) { _overflow = true; return; }
   _trace_bytes += bytes;
   _events.push_back(std::move(event));
}

void forge_pubsub_fixture::record(std::string_view kind, std::string_view source,
                                  forge::mutable_variant_object fields) {
   const auto lock = std::scoped_lock{_mutex};
   record_locked(kind, source, std::move(fields));
}

bool forge_pubsub_fixture::cached_owner_locked(std::uint64_t session_id, const p2p::peer_id& peer) const {
   const auto owner = _connections.find(session_id);
   if (owner == _connections.end()) { return false; }
   if (owner->second.peer != peer) { throw std::runtime_error{"PubSub cached native owner peer mismatch"}; }
   return true;
}

void forge_pubsub_fixture::trace(const pubsub::trace_event& value) {
   try {
      if (!_node) { throw std::runtime_error{"PubSub trace before node ownership"}; }
      {
         const auto lock = std::scoped_lock{_mutex};
         if (_overflow || !_capture_error.empty()) { return; }
      }
      if (value.kind == pubsub::trace_kind::rpc_read || value.kind == pubsub::trace_kind::rpc_write) {
         if (value.framed_rpc.empty() || value.framed_rpc.size() > 16 * 1024 + 3 ||
             value.session_id == 0 || value.stream_id < 0) {
            throw std::runtime_error{"missing/oversized native PubSub stream receipt"};
         }
         auto cached = false;
         {
            const auto lock = std::scoped_lock{_mutex};
            cached = cached_owner_locked(value.session_id, value.peer);
         }
         auto observed = std::optional<native_owner>{};
         if (!cached) {
            // Query native facts only for the first receipt, outside the fixture lock.
            const auto native = _node->diagnostics();
            const auto owner = std::ranges::find(native.sessions, value.session_id, &p2p::diagnostics::session::id);
            if (owner != native.sessions.end()) {
               if (owner->remote_peer != value.peer || !owner->remote_endpoint ||
                   owner->authentication == p2p::peer_authentication::unverified) {
                  throw std::runtime_error{"PubSub stream lacks its native authenticated session"};
               }
               const auto quic = owner->authentication == p2p::peer_authentication::quic_tls;
               const auto noise = owner->authentication == p2p::peer_authentication::noise;
               if (!quic && !noise && owner->authentication != p2p::peer_authentication::libp2p_tls) {
                  throw std::runtime_error{"unknown PubSub native authentication"};
               }
               observed = native_owner{.peer = owner->remote_peer, .remote_address = owner->remote_endpoint->to_string(),
                   .transport = quic ? "quic" : "tcp", .security = noise ? "/noise" : "/tls/1.0.0",
                   .muxer = quic ? "quic" : owner->muxer.value};
               if (observed->muxer.empty()) { throw std::runtime_error{"missing PubSub native muxer"}; }
            }
         }
         const auto peer = value.peer.to_string();
         const auto connection = std::to_string(value.session_id);
         const auto stream = std::to_string(value.stream_id);
         const auto protocol = value.protocol.value;
         auto receipt = forge::mutable_variant_object{};
         const auto direction = value.kind == pubsub::trace_kind::rpc_read ? "read" : "write";
         receipt("framed_hex", forge::codec::hex::encode(value.framed_rpc))
             (direction, forge::mutable_variant_object{}("framed_bytes", static_cast<std::uint64_t>(value.framed_rpc.size()))
                 ("framed_sha256", forge::crypto::digest::sha256::hash(value.framed_rpc).str())
                 ("frames", 1u)("complete_frames", true)("invalid_or_over_limit", false));
         const auto lock = std::scoped_lock{_mutex};
         auto owner = _connections.find(value.session_id);
         if (owner == _connections.end()) {
            if (!observed) { throw std::runtime_error{"PubSub first receipt lacks its native authenticated session"}; }
            if (_connections.size() >= 16) { _overflow = true; return; }
            auto facts = forge::mutable_variant_object{}("peer_id", peer)("connection_id", connection)
                ("authenticated", true)("remote_address", observed->remote_address)
                ("transport", observed->transport)("security", observed->security)("muxer", observed->muxer)
                ("authentication_basis", "native_session_upgrade_output");
            if (!_fingerprint.empty()) { facts("pnet_verified", true)("pnet_fingerprint", _fingerprint); }
            record_locked("connection", "forge.node.authenticated_session", std::move(facts));
            _connections.emplace(value.session_id, std::move(*observed));
         }
         // Cached, verified facts survive active-session retirement during native teardown.
         static_cast<void>(cached_owner_locked(value.session_id, value.peer));
         const auto stream_key = std::tuple{value.session_id, value.stream_id, protocol};
         if (!_streams.contains(stream_key)) {
            if (_streams.size() >= 64) { _overflow = true; return; }
            record_locked("protocol", "forge.pubsub.native_stream", forge::mutable_variant_object{}
                ("peer_id", peer)("connection_id", connection)("stream_id", stream)("protocol", protocol));
            _streams.insert(stream_key);
         }
         record_locked("rpc", "forge.pubsub.native_stream", forge::mutable_variant_object{}
             ("peer_id", peer)("connection_id", connection)("stream_id", stream)("protocol", protocol)
             ("direction", direction)("receipt", std::move(receipt)));
         return;
      }
      auto detail = forge::mutable_variant_object{}("propagation_peer", value.peer.to_string())
          ("author_peer", value.author ? value.author->to_string() : "")("topic", value.subject.value)
          ("message_id", forge::codec::hex::encode(value.message_id))("seqno_hex", forge::codec::hex::encode(value.seqno))
          ("payload_sha256", forge::crypto::digest::sha256::hash(value.data).str())("generation", value.generation);
      if (value.kind == pubsub::trace_kind::validation_committed) {
         if (!value.result) { throw std::runtime_error{"missing committed validation result"}; }
         detail("outcome", outcome(*value.result));
         record("validation", "forge.pubsub.committed_validation", std::move(detail));
      } else {
         record("delivery", "forge.pubsub.committed_validation", std::move(detail));
      }
   } catch (const std::exception& error) {
      const auto lock = std::scoped_lock{_mutex};
      if (_capture_error.empty()) { _capture_error = bounded_error(error.what()); }
   }
}

pubsub::options forge_pubsub_fixture::pubsub_options() const {
   auto params = pubsub::scoring_params{};
   auto topic = pubsub::topic_score_params{};
   topic.invalid_message_deliveries_weight = -100.0;
   params.topics.emplace(pubsub::topic{"forge-pr11:" + _token}, topic);
   params.retain_score = 60s;
   params.limits.max_connected_peers = 16;
   params.limits.max_retained_peers = 16;
   params.limits.max_messages = 128;
   auto options = pubsub::options{};
   options.scoring = std::move(params);
   options.limits.max_rpc_size = 16 * 1024;
   options.limits.max_data_size = 1024;
   options.limits.max_message_size = 2048;
   options.limits.max_peers_per_topic = 16;
   options.limits.max_topics = 1;
   options.limits.mesh_n = 2;
   options.limits.mesh_n_low = 1;
   options.limits.mesh_n_high = 4;
   options.limits.mesh_score_min = 1;
   options.limits.mesh_outbound_min = 0;
   options.limits.heartbeat_interval = 250ms;
   options.limits.prune_backoff = 1s;
   return options;
}

void forge_pubsub_fixture::sample(std::string_view label) {
   const auto native = _node->pubsub_scores();
   auto mesh = forge::variants{};
   auto scores = forge::variants{};
   for (const auto& peer : native.peers) {
      auto invalid = 0.0;
      for (const auto& topic : peer.topics) {
         if (topic.subject.value != "forge-pr11:" + _token) { continue; }
         invalid = topic.invalid_message_deliveries;
         if (topic.in_mesh) { mesh.emplace_back(peer.peer.to_string()); }
      }
      scores.emplace_back(forge::mutable_variant_object{}("peer_id", peer.peer.to_string())
          ("value", peer.value)("invalid_deliveries", invalid));
   }
   std::ranges::sort(mesh, [](const auto& left, const auto& right) { return left.get_string() < right.get_string(); });
   record("snapshot", "forge.pubsub.peer_score_snapshot", forge::mutable_variant_object{}
       ("label", std::string{label})("mesh_peer_ids", std::move(mesh))("peer_scores", std::move(scores)));
}

forge::variant forge_pubsub_fixture::result(bool finalized, bool joined, std::string_view error) const {
   const auto lock = std::scoped_lock{_mutex};
   const auto failure = bounded_error(!error.empty() ? error : std::string_view{_capture_error});
   return forge::variant{forge::mutable_variant_object{}("schema_version", 1u)("implementation", "forge")
       ("actor", _actor)("case_token", _token)("local_peer_id", _peer)("finalized", finalized)
       ("joined", joined)("overflow", _overflow)("error", failure.empty() ? forge::variant{} : forge::variant{failure})
       ("events", _events)};
}

void forge_pubsub_fixture::command(const forge::variant& input, std::uint64_t sequence,
                                   forge::asio::runtime& runtime) {
   {
      const auto lock = std::scoped_lock{_mutex};
      if (_prepared) { throw std::runtime_error{"PubSub command admission closed after prepare_shutdown"}; }
      if (_overflow || !_capture_error.empty()) { throw std::runtime_error{"PubSub sticky capture failure"}; }
   }
   if (!input.is_object()) { throw std::runtime_error{"PubSub control must be an object"}; }
   const auto& object = input.get_object();
   if (!object["sequence"].is_integer() || object["sequence"].as_uint64() != sequence ||
       !object["kind"].is_string()) { throw std::runtime_error{"PubSub command sequence/kind mismatch"}; }
   const auto& kind = object["kind"].get_string();
   if (kind == "prepare_shutdown") {
      prepare_shutdown(input, sequence);
   } else if (kind == "connect" && object.size() == 4 && object["peer_id"].is_string() && object["address"].is_string()) {
      auto address = p2p::parse_endpoint(object["address"].get_string());
      const auto peer = p2p::peer_id::from_string(object["peer_id"].get_string());
      if (!p2p::valid_peer_id(peer) || (address.peer && *address.peer != peer)) {
         throw std::runtime_error{"PubSub target identity mismatch"};
      }
      forge::asio::blocking::run(runtime, _node->async_connect(address, {.expected_peer = peer, .allow_relay = false,
          .timeout = 5s, .allow_hole_punch = false}));
   } else if (kind == "publish" && object.size() == 3 && object["payload"].is_string()) {
      const auto& text = object["payload"].get_string();
      if (text.empty() || text.size() > 1024) { throw std::runtime_error{"PubSub payload outside fixture bound"}; }
      forge::asio::blocking::run(runtime, _node->async_publish({"forge-pr11:" + _token}, {text.begin(), text.end()}));
      record("publish", "forge.node.async_publish", forge::mutable_variant_object{}
          ("command_sequence", sequence)("topic", "forge-pr11:" + _token)
          ("payload_sha256", forge::crypto::digest::sha256::hash(
              std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}).str()));
   } else if (kind == "sample" && object.size() == 3 && object["label"].is_string()) {
      const auto& label = object["label"].get_string();
      if (label.empty() || label.size() > 64) { throw std::runtime_error{"invalid PubSub sample label"}; }
      sample(label);
   } else {
      throw std::runtime_error{"unsupported/ambiguous PubSub command"};
   }
   record("command_done", "forge.fixture.native_operation", forge::mutable_variant_object{}
       ("command_sequence", sequence)("command_kind", kind)("status", "ok"));
}

void forge_pubsub_fixture::prepare_shutdown(const forge::variant& input, std::uint64_t sequence) {
   const auto lock = std::scoped_lock{_mutex};
   if (_prepared || _overflow || !_capture_error.empty()) {
      throw std::runtime_error{"prepare_shutdown requires open admission and no capture errors"};
   }
   const auto& object = input.get_object();
   if (object.size() != 5 || !object["actor"].is_string() || object["actor"].get_string() != _actor ||
       !object["case_token"].is_string() || object["case_token"].get_string() != _token ||
       !object["local_peer_id"].is_string() || object["local_peer_id"].get_string() != _peer || _peer.empty()) {
      throw std::runtime_error{"prepare_shutdown actor/token/identity mismatch"};
   }
   _prepared = true;
   record_locked("shutdown_prepared", "forge.fixture.prepare_shutdown", forge::mutable_variant_object{}
       ("command_sequence", sequence)("actor", _actor)("case_token", _token)("local_peer_id", _peer)
       ("admission_closed", true)("pending_commands", 0u));
   if (_overflow || !_capture_error.empty()) { throw std::runtime_error{"prepare_shutdown acknowledgement capture failed"}; }
}

void forge_pubsub_fixture::configure_stream_security(p2p::node::options& options, std::string_view transport) {
   if (transport == "tcp" || transport == "tcp-pnet-noise") {
      options.stream_security = p2p::node::stream_security::noise;
   }
}

int forge_pubsub_fixture::run(const arguments& input, const support& composition) {
   auto args = input;
   const auto required = std::set<std::string>{"command", "version", "transport", "actor", "case-token",
       "ready-file", "result-file", "stop-file", "control-file", "store-dir"};
   if (!composition.make_options || std::ranges::any_of(required, [&](const auto& flag) {
          return !args.contains(flag) || args.at(flag).empty();
       }) || args.at("command") != "pubsub-live") {
      throw std::runtime_error{"missing/empty PubSub actor options"};
   }
   const auto transport = args.at("transport");
   const auto private_profile = transport == "tcp-pnet-noise";
   auto allowed = required;
   if (private_profile) { allowed.insert("pnet-key-file"); allowed.insert("pnet-fingerprint"); }
   if ((transport != "quic" && transport != "tcp" && !private_profile) ||
       (args.at("version") != "1.0" && args.at("version") != "1.1") || args.size() != allowed.size() ||
       std::ranges::any_of(args, [&](const auto& option) {
          return !allowed.contains(option.first) || option.second.empty() || option.second.starts_with("--");
       })) {
      throw std::runtime_error{"invalid PubSub actor options"};
   }
   auto fixture = forge_pubsub_fixture{args.at("case-token"), args.at("actor")};
   canonical_paths(args, private_profile);
   auto control = control_reader{args.at("control-file")};
   auto options = composition.make_options(args);
   configure_stream_security(options, transport);
   if (options.allow_insecure_test_mode || private_profile != options.private_network.has_value()) {
      throw std::runtime_error{"PubSub requires actual native authentication/private profile composition"};
   }
   if (private_profile) {
      const auto fingerprint = options.private_network->protector->fingerprint();
      fixture._fingerprint = forge::codec::hex::encode(fingerprint.bytes);
      if (fixture._fingerprint != args.at("pnet-fingerprint")) {
         throw std::runtime_error{"private PubSub fingerprint mismatch"};
      }
   }
   options.limits.pubsub = fixture.pubsub_options();
   options.limits.pubsub.preferred = args.at("version") == "1.0" ? pubsub::version::v1_0 : pubsub::version::v1_1;
   options.limits.pubsub.allow_v1_0_fallback = false;
   options.limits.pubsub.tracer = [&](const auto& event) { fixture.trace(event); };
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto node = p2p::node{runtime, std::move(options)};
   fixture._node = &node;
   auto error = std::string{};
   auto joined = false;
   auto apply = [&](const auto& command, std::uint64_t sequence) { fixture.command(command, sequence, runtime); };
   try {
      const auto peer = node.local_peer().to_string();
      if (peer.size() > peer_limit) { throw std::runtime_error{"PubSub local peer exceeds evidence bound"}; }
      fixture._peer = peer;
      const auto endpoint = p2p::parse_endpoint(transport == "quic" ?
          "/ip4/127.0.0.1/udp/0/quic-v1" : "/ip4/127.0.0.1/tcp/0");
      forge::asio::blocking::run(runtime, node.async_listen(endpoint));
      forge::asio::blocking::run(runtime, node.async_start());
      forge::asio::blocking::run(runtime, node.async_subscribe({"forge-pr11:" + fixture._token},
          [&](pubsub::event event) -> boost::asio::awaitable<pubsub::validation_result> {
             const auto text = std::string_view{reinterpret_cast<const char*>(event.value.data.data()), event.value.data.size()};
             if (fixture._actor == "victim" && text.starts_with("reject:" + fixture._token + ':')) {
                co_return pubsub::validation_result::reject;
             }
             if (fixture._actor == "victim" && text.starts_with("ignore:" + fixture._token + ':')) {
                co_return pubsub::validation_result::ignore;
             }
             co_return pubsub::validation_result::accept;
          }));
      const auto listener = node.local_endpoint();
      if (!listener) { throw std::runtime_error{"PubSub fixture listener not published"}; }
      const auto address = ready_address(*listener, node.local_peer());
      write_atomic(args.at("ready-file"), forge::variant{forge::mutable_variant_object{}("schema_version", 1u)
          ("implementation", "forge")("actor", fixture._actor)("case_token", fixture._token)
          ("local_peer_id", fixture._peer)("peer_id", fixture._peer)
          ("listen_addrs", forge::variants{forge::variant{address}})("topic", "forge-pr11:" + fixture._token)
          ("ready", true)("subscription_created", true)});
      auto next_snapshot = std::chrono::steady_clock::now();
      const auto deadline = next_snapshot + 70s;
      while (true) {
         if (stop_requested(args.at("stop-file"))) {
            control.poll(args.at("control-file"), apply, true);
            const auto lock = std::scoped_lock{fixture._mutex};
            if (!fixture._prepared) { throw std::runtime_error{"PubSub stop before prepare_shutdown acknowledgement"}; }
            break;
         }
         const auto now = std::chrono::steady_clock::now();
         if (now >= deadline) { throw std::runtime_error{"PubSub actor stop deadline exceeded"}; }
         control.poll(args.at("control-file"), apply);
         if (now >= next_snapshot) {
            fixture.sample("periodic");
            next_snapshot = now + 250ms;
         }
         write_atomic(args.at("result-file"), fixture.result(false, false, {}));
         std::this_thread::sleep_for(25ms);
      }
   } catch (const std::exception& failure) { error = bounded_error(failure.what()); }
   try {
      forge::asio::blocking::run(runtime, node.async_stop());
      runtime.stop();
      joined = true;
   } catch (const std::exception& failure) {
      if (error.empty()) { error = bounded_error(failure.what()); }
   }
   if (error.empty()) {
      try { control.poll(args.at("control-file"), apply, true); }
      catch (const std::exception& failure) { error = bounded_error(failure.what()); }
   }
   const auto final = fixture.result(true, joined, error);
   write_atomic(args.at("result-file"), final);
   return final["error"].is_null() && !final["overflow"].as_bool() && joined ? 0 : 1;
}

} // namespace forge::test::libp2p_interop
