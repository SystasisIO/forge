#include "forge_plugin_interop_fixture.hxx"

import forge.config.core.component;

namespace forge::test::libp2p_interop {
namespace {
namespace np = forge::plugins::net::p2p::node;
namespace p2p = forge::net::p2p;
namespace ps = p2p::pubsub;
using object = forge::mutable_variant_object;
using value = forge::config::core::value;
using namespace std::chrono_literals;
constexpr std::size_t control_limit = 1024 * 1024;
constexpr std::size_t line_limit = 16 * 1024;
constexpr std::size_t event_limit = 64 * 1024;
constexpr std::size_t capture_limit = 4 * 1024 * 1024;

std::string text(const forge::variant& command, std::string_view field) {
   const auto& member = command.get_object()[std::string{field}];
   if (!member.is_string() || member.get_string().empty()) { throw std::runtime_error{"missing PubSub string"}; }
   return member.get_string();
}

std::vector<std::uint8_t> command_payload(const forge::variant& command) {
   const auto kind = text(command, "kind");
   const auto input = text(command, kind == "publish" ? "payload" : "payload_hex");
   if (kind == "publish") {
      if (input.size() > 1024 || input.find('\0') != std::string::npos) {
         throw std::runtime_error{"invalid PubSub text payload"};
      }
      return {input.begin(), input.end()};
   }
   if (input.size() > 2048 || input.size() % 2 != 0) { throw std::runtime_error{"invalid bounded PubSub hex"}; }
   return forge::codec::hex::decode(input);
}

void check_command(const forge::variant& command, std::uint64_t sequence) {
   if (!command.is_object() || !command["sequence"].is_integer() ||
       command["sequence"].as_uint64() != sequence) { throw std::runtime_error{"PubSub sequence mismatch"}; }
   const auto kind = text(command, "kind");
   const auto size = command.get_object().size();
   if (kind == "connect" && size == 4) {
      const auto peer = p2p::peer_id::from_string(text(command, "peer_id"));
      const auto endpoint = p2p::parse_endpoint(text(command, "address"));
      if (!p2p::valid_peer_id(peer) || (endpoint.peer && *endpoint.peer != peer) ||
          !endpoint.is_direct_tcp()) {
         throw std::runtime_error{"invalid PubSub authenticated TCP target"};
      }
   } else if ((kind == "publish" || kind == "publish_binary" || kind == "publish_oversized") && size == 3) {
      static_cast<void>(command_payload(command));
   } else if (kind == "sample" && size == 3) {
      const auto label = text(command, "label");
      if (label.size() > 64 || label == "periodic") { throw std::runtime_error{"invalid PubSub sample label"}; }
   } else if (kind == "prepare_shutdown" && size == 5) {
      for (const auto field : {"actor", "case_token", "local_peer_id"}) { static_cast<void>(text(command, field)); }
   } else { throw std::runtime_error{"unsupported PubSub control shape"}; }
}

forge::variants controls(std::string_view previous, std::string_view next, std::size_t& offset,
                         std::uint64_t sequence, bool prepared, bool stopping) {
   if (next.size() > control_limit || !next.starts_with(previous) || (prepared && next != previous)) {
      throw std::runtime_error{"PubSub control rewrite/truncation/closed admission"};
   }
   auto output = forge::variants{};
   auto cursor = offset;
   while (cursor < next.size()) {
      const auto newline = next.find('\n', cursor);
      if (newline == std::string_view::npos) {
         if (stopping || next.size() - cursor > line_limit) { throw std::runtime_error{"incomplete PubSub control"}; }
         break;
      }
      const auto line = next.substr(cursor, newline - cursor);
      if (line.empty() || line.size() > line_limit || ++sequence > 64) { throw std::runtime_error{"PubSub control bound"}; }
      const auto parsed = forge::codec::json::read_value(line, {.max_depth = 8});
      if (!parsed.ok()) { throw std::runtime_error{"invalid PubSub control JSON"}; }
      check_command(parsed.value, sequence);
      if (text(parsed.value, "kind") == "prepare_shutdown" && newline + 1 != next.size()) {
         throw std::runtime_error{"PubSub prepare with queued/incomplete control"};
      }
      output.emplace_back(parsed.value);
      cursor = newline + 1;
   }
   offset = cursor;
   return output;
}

std::string read_controls(const std::filesystem::path& path, bool allow_missing) {
   const auto status = std::filesystem::symlink_status(path);
   if (allow_missing && status.type() == std::filesystem::file_type::not_found) { return {}; }
   if (!std::filesystem::is_regular_file(status)) { throw std::runtime_error{"invalid PubSub control file"}; }
   const auto size = std::filesystem::file_size(path);
   if (size > control_limit) { throw std::runtime_error{"PubSub control file bound"}; }
   auto input = std::ifstream{path, std::ios::binary};
   if (!input) { throw std::runtime_error{"cannot open PubSub control file"}; }
   // A concurrently appended tail is consumed on the next poll; every read/allocation remains bounded.
   auto bytes = std::string(static_cast<std::size_t>(size) + 1, '\0');
   input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
   bytes.resize(static_cast<std::size_t>(input.gcount()));
   if (input.bad() || bytes.size() > control_limit) { throw std::runtime_error{"PubSub control read bound"}; }
   return bytes;
}

void check_probe(std::string_view kind, std::size_t bytes, bool typed) {
   const auto bound = typed ? 96u : 128u;
   if (bytes == 0 || (kind == "publish_oversized" ? bytes != bound + 1 : bytes > bound)) {
      throw std::runtime_error{"PubSub probe outside exact policy boundary"};
   }
}

void check_prepare(const forge::variant& command, std::string_view actor, std::string_view token,
                    std::string_view local_peer) {
   if (text(command, "actor") != actor || text(command, "case_token") != token ||
       text(command, "local_peer_id") != local_peer || local_peer.empty()) {
      throw std::runtime_error{"PubSub prepare identity mismatch"};
   }
}

forge::variant data_facts(std::span<const std::uint8_t> data) {
   return object{}("bytes", static_cast<std::uint64_t>(data.size()))
       ("payload_sha256", forge::crypto::digest::sha256::hash(data).str())
       ("contains_zero", std::find(data.begin(), data.end(), 0) != data.end());
}

value declarative_pubsub(bool competing) {
   const auto yaml = competing ?
       "pubsub:\n  enabled: true\n  preferred-version: v1-0\n  signature-policy: strict-sign\n"
       "  sign-publishes: true\n  max-message-size: 128\n  flood-publish: false\n" :
       "pubsub:\n  enabled: true\n  preferred-version: v1-1\n  signature-policy: strict-sign\n"
       "  sign-publishes: true\n  max-message-size: 128\n  flood-publish: true\n";
   const auto parsed = forge::codec::yaml::read_document(yaml, {.source_name = "configured-pubsub-profile", .max_depth = 8});
   const auto* profile = parsed.value.try_get("pubsub");
   if (!parsed.ok() || !profile || !profile->is_object()) { throw std::runtime_error{"PubSub YAML profile parse failed"}; }
   return *profile;
}

std::string outcome(ps::validation_result result) {
   switch (result) {
   case ps::validation_result::accept: return "accept";
   case ps::validation_result::reject: return "reject";
   case ps::validation_result::ignore: return "ignore";
   case ps::validation_result::retry: return "retry";
   }
   throw std::runtime_error{"unknown PubSub result"};
}

std::string identify_state_name(p2p::identify::state state) {
   switch (state) {
   case p2p::identify::state::unknown: return "unknown";
   case p2p::identify::state::identifying: return "identifying";
   case p2p::identify::state::identified: return "identified";
   case p2p::identify::state::failed: return "failed";
   }
   throw std::runtime_error{"unknown native Identify state"};
}

boost::asio::awaitable<void> drain_handlers(std::shared_ptr<np::pubsub_api> api) {
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   const auto deadline = std::chrono::steady_clock::now() + 4s;
   while (api->snapshot().active_handlers != 0) {
      if (std::chrono::steady_clock::now() >= deadline) { throw std::runtime_error{"PubSub callback drain deadline"}; }
      timer.expires_after(5ms);
      co_await timer.async_wait(boost::asio::use_awaitable);
   }
}
} // namespace

void forge_plugin_interop_fixture::pubsub_state::record(
    std::string_view kind, std::string_view source, object fields) noexcept {
   try {
      auto lock = std::lock_guard{mutex};
      if (overflow || capture_error) { return; }
      if (events.size() == 2048) { overflow = true; return; }
      fields.set("sequence", static_cast<std::uint64_t>(events.size() + 1))
          .set("mono_ns", static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count()))
          .set("kind", std::string{kind}).set("source", std::string{source});
      const auto encoded = forge::codec::json::write_value(fields, {.max_bytes = event_limit});
      if (!encoded.ok() || encoded.text.size() > capture_limit - capture_bytes) { overflow = true; return; }
      capture_bytes += encoded.text.size();
      events.emplace_back(std::move(fields));
   } catch (...) {
      auto lock = std::lock_guard{mutex};
      capture_error = true;
      capture_failure = current_failure("pubsub.event.capture");
   }
}

bool forge_plugin_interop_fixture::pubsub_state::failed() {
   auto lock = std::lock_guard{mutex};
   return overflow || capture_error;
}

boost::asio::awaitable<ps::validation_result> forge_plugin_interop_fixture::pubsub_state::deliver(
    std::shared_ptr<pubsub_state> state, np::message message) {
   try {
      const auto bound = state->typed ? 96u : 128u;
      if (message.data.size() > bound || message.seqno.size() > 64 ||
          message.subject.value != state->topic || !message.author) {
         throw std::runtime_error{"invalid consumer delivery observation"};
      }
      // Callback admission is not a native validation commit. Trace/counters supply that independent evidence.
      state->record("subscription_delivery", "forge.node.pubsub_api.subscribe",
          object{data_facts(message.data)}("propagation_peer", message.source.to_string())
              ("author_peer", message.author->to_string())("topic", message.subject.value)
              ("seqno_hex", forge::codec::hex::encode(message.seqno))("outcome", "accept")
              ("native_validation_commit", false)("at_payload_limit", message.data.size() == bound)
              ("payload_hex", forge::codec::hex::encode(message.data)));
   } catch (...) {
      auto lock = std::lock_guard{state->mutex};
      state->capture_error = true;
      state->capture_failure = current_failure("pubsub.subscription.delivery");
   }
   co_return state->failed() ? ps::validation_result::reject : ps::validation_result::accept;
}

void forge_plugin_interop_fixture::pubsub_state::trace(const ps::trace_event& event) noexcept {
   try {
      if (event.kind == ps::trace_kind::rpc_read || event.kind == ps::trace_kind::rpc_write) {
         if (event.framed_rpc.empty() || event.framed_rpc.size() > 16 * 1024 || event.session_id == 0 ||
             event.stream_id < 0 || event.protocol.value != "/meshsub/1.2.0") {
            throw std::runtime_error{"invalid/mismatched native v1.2 RPC observation"};
         }
         // rpc_read precedes control application. Check only wire structure with the native codec, not mesh commit.
         auto wire = ps::options{};
         wire.preferred = ps::version::v1_2;
         wire.limits.max_data_size = 96;
         static_cast<void>(ps::codec::decode(event.framed_rpc, wire));
         const auto key = std::tuple{event.session_id, event.stream_id, event.protocol.value};
         auto first = false;
         {
            auto lock = std::lock_guard{mutex};
            if (!streams.contains(key) && streams.size() >= 64) { overflow = true; return; }
            first = streams.insert(key).second;
         }
         auto fields = object{}("peer_id", event.peer.to_string())("connection_id", std::to_string(event.session_id))
             ("stream_id", std::to_string(event.stream_id))("protocol", event.protocol.value);
         if (first) { record("protocol", "forge.pubsub.native_stream", fields); }
         const auto direction = event.kind == ps::trace_kind::rpc_read ? "read" : "write";
         fields("direction", direction)("receipt", object{}("framed_hex", forge::codec::hex::encode(event.framed_rpc))
             (direction, object{}("framed_bytes", static_cast<std::uint64_t>(event.framed_rpc.size()))
                 ("framed_sha256", forge::crypto::digest::sha256::hash(event.framed_rpc).str())
                 ("frames", 1u)("complete_frames", true)("invalid_or_over_limit", false)));
         record("rpc", "forge.pubsub.native_stream", std::move(fields));
      } else {
         if (event.data.size() > 96 || event.message_id.size() > 256 || event.seqno.size() > 64 ||
             event.subject.value.size() > 255) { throw std::runtime_error{"PubSub delivery capture bound"}; }
         auto fields = object{data_facts(event.data)}("propagation_peer", event.peer.to_string())
             ("author_peer", event.author ? event.author->to_string() : "")("topic", event.subject.value)
             ("message_id", forge::codec::hex::encode(event.message_id))("seqno_hex", forge::codec::hex::encode(event.seqno))
             ("generation", event.generation);
         if (event.kind == ps::trace_kind::validation_committed) {
            if (!event.result) { throw std::runtime_error{"missing native validation result"}; }
            fields("outcome", outcome(*event.result));
         }
         record(event.kind == ps::trace_kind::validation_committed ? "validation" : "delivery",
                "forge.pubsub.committed_validation", std::move(fields));
      }
   } catch (...) {
      auto lock = std::lock_guard{mutex};
      capture_error = true;
      capture_failure = current_failure("pubsub.native.trace");
   }
}

void forge_plugin_interop_fixture::configure_pubsub_document(forge::config::core::document& config) {
   for (const auto key : {"version", "actor", "case-token", "control-file", "ready-file", "pubsub-policy"}) {
      if (argument(key).empty()) { throw std::runtime_error{"missing PubSub fixture argument"}; }
   }
   const auto actor = argument("actor");
   const auto token = argument("case-token");
   const auto typed = argument("pubsub-policy") == "typed-override";
   if ((!typed && argument("pubsub-policy") != "yaml-only") || argument("version") != (typed ? "1.2" : "1.1") ||
       (actor != "victim" && actor != "offender" && actor != "replacement" && actor != "sink") ||
       token.size() != 32 || !std::ranges::all_of(token, [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
       }) || argument("transport") != "tcp") { throw std::runtime_error{"unsupported PubSub profile"}; }
   for (const auto key : {"addr", "peer-id", "payload", "dns-server", "pnet-key-file", "pnet-fingerprint",
                          "pnet-control", "pnet-correlation", "features"}) {
      if (_args.contains(key)) { throw std::runtime_error{"irrelevant PubSub fixture argument"}; }
   }
   auto paths = std::set<std::filesystem::path>{};
   for (const auto key : {"ready-file", "result-file", "control-file", "stop-file", "store-dir"}) {
      const auto original = std::filesystem::absolute(argument(key));
      if (std::filesystem::is_symlink(std::filesystem::symlink_status(original))) {
         throw std::runtime_error{"PubSub artifact leaf symlink"};
      }
      const auto path = std::filesystem::weakly_canonical(original);
      if (!paths.insert(path).second || (std::string_view{key} != "control-file" &&
          std::string_view{key} != "store-dir" && std::filesystem::exists(path))) {
         throw std::runtime_error{"PubSub artifact paths must be distinct and fresh"};
      }
      if (std::string_view{key} != "store-dir" && !std::filesystem::is_directory(path.parent_path())) {
         throw std::runtime_error{"PubSub artifact parent must exist"};
      }
      _args[key] = path.string();
   }
   for (const auto key : {"ready-file", "result-file"}) {
      auto temporary = std::filesystem::absolute(argument(key)).lexically_normal();
      temporary += ".tmp";
      if (!paths.insert(temporary).second || std::filesystem::exists(temporary) ||
          std::filesystem::is_symlink(std::filesystem::symlink_status(temporary))) {
         throw std::runtime_error{"PubSub temporary artifact collision"};
      }
   }
   if (std::filesystem::exists(argument("store-dir")) && !std::filesystem::is_directory(argument("store-dir"))) {
      throw std::runtime_error{"PubSub store path must be a directory"};
   }
   if (!read_controls(argument("control-file"), true).empty()) { throw std::runtime_error{"PubSub control must start fresh"}; }
   _result_safe = true;
   _pubsub = std::make_shared<pubsub_state>();
   _pubsub->actor = actor;
   _pubsub->token = token;
   _pubsub->topic = "forge-pr11:" + token;
   _pubsub->version = argument("version");
   _pubsub->typed = typed;
   _pubsub->control_seen = std::filesystem::exists(argument("control-file"));
   config.set("plugins.net.p2p.node.pubsub", declarative_pubsub(false));
   config.set("plugins.net.p2p.node.pubsub.allowed-topics", value::array_type{value{_pubsub->topic}});
}

void forge_plugin_interop_fixture::initialize_pubsub(const forge::config::core::document& initial) {
   if (!_pubsub->typed) { return; }
   const auto state = _pubsub;
   auto options = ps::options{};
   options.preferred = ps::version::v1_2;
   options.flood_publish = true;
   options.limits.max_data_size = 96;
   options.scoring.emplace();
   options.scoring->app_specific_weight = 2.;
   options.scoring->app_specific_score = [state](const p2p::peer_id&) { ++state->score_calls; return 3.; };
   options.tracer = [state](const ps::trace_event& event) { state->trace(event); };
   api<np::configuration_api>()->set_pubsub_options(options);
   auto competing = initial;
   competing.set("plugins.net.p2p.node.pubsub", declarative_pubsub(true));
   competing.set("plugins.net.p2p.node.pubsub.allowed-topics", value::array_type{value{state->topic}});
   run(_node_plugin->configure({competing, "plugins.net.p2p.node"}), "pubsub.competing_node_configure");
   // Only the official node component was reconfigured; Secrets and ObjectDB remain shell-owned and unchanged.
   // This fixture's crypto subtree contains only the Secrets entries; omit the
   // entire subtree so the redacted receipt matches the initial public document.
   static_cast<void>(competing.erase("plugins.crypto"));
   const auto encoded = forge::codec::json::write_document(competing);
   if (!encoded.ok()) { throw std::runtime_error{"PubSub redacted configure encoding failed"}; }
   const auto decoded = forge::codec::json::read_value(encoded.text);
   if (!decoded.ok()) { throw std::runtime_error{"PubSub redacted configure conversion failed"}; }
   _settings = object{_settings}("node_reconfigure", object{}("source", "official-node-plugin.configure")
       ("component", "plugins.net.p2p.node")
       ("document", decoded.value))("typed_contribution", object{}("source", "node.configuration_api.set_pubsub_options")
           ("full_native_snapshot", true)("preferred", "v1-2")("max_data_size", 96u)
           ("signatures", "strict-sign")("flood_publish", true)("tracer_installed", true)
           ("application_score_callback_installed", true)("application_sample", 3.)("application_weight", 2.));
}

void forge_plugin_interop_fixture::start_pubsub() {
   const auto state = _pubsub;
   state->subscription = run(api<np::pubsub_api>()->subscribe({state->topic},
       [state](np::message message) { return pubsub_state::deliver(state, std::move(message)); }), "pubsub.subscribe");
   if (state->subscription->id == 0) { throw std::runtime_error{"PubSub subscribe returned no registration identity"}; }
   if (state->typed) {
      try {
         api<np::configuration_api>()->set_pubsub_options(ps::options{});
         throw std::runtime_error{"running PubSub contribution unexpectedly accepted"};
      } catch (const np::exceptions::route_conflict&) {
         state->record("configuration_frozen", "forge.node.configuration_api", object{current_failure("pubsub.post-start.configure")});
      }
   }
   sample_pubsub("startup");
}

forge::variant forge_plugin_interop_fixture::pubsub_session_facts(
    const p2p::diagnostics::session& session, const p2p::node::session_info& connected, std::string_view local_peer) {
   const auto local = p2p::peer_id::from_string(local_peer);
   if (connected.id == 0 || !p2p::valid_peer_id(connected.remote_peer) || !p2p::valid_peer_id(local) ||
       session.id != connected.id || session.remote_peer != connected.remote_peer || session.closed ||
       connected.path != p2p::path::kind::direct || session.path != connected.path ||
       session.authentication != p2p::peer_authentication::noise || session.muxer != connected.muxer ||
       session.muxer.value != "/yamux/1.0.0" || session.direction != p2p::diagnostics::session_direction::outbound ||
       session.relay_peer || session.circuit_endpoint || session.carrier_session_id ||
       !session.local_endpoint || !session.remote_endpoint) {
      throw std::runtime_error{"PubSub connect and retained authenticated session disagree"};
   }
   const auto& local_endpoint = *session.local_endpoint;
   const auto& remote_endpoint = *session.remote_endpoint;
   if (!local_endpoint.is_direct_tcp() || !remote_endpoint.is_direct_tcp() ||
       local_endpoint.transport.port == 0 || remote_endpoint.transport.port == 0 ||
       (local_endpoint.peer && *local_endpoint.peer != local) ||
       (remote_endpoint.peer && *remote_endpoint.peer != connected.remote_peer)) {
      throw std::runtime_error{"PubSub session lacks matching actual TCP socket facts"};
   }
   return object{}("source", "node.api.connect-and-node.diagnostics-api.session")
       ("session_id", session.id)("local_peer_id", std::string{local_peer})
       ("remote_peer_id", session.remote_peer.to_string())("closed", session.closed)
       ("identify_state", identify_state_name(session.identify_state))
       ("identify_observed", session.identify_state == p2p::identify::state::identified)("identify_required", false)
       ("authentication", "noise")("authentication_basis", "native_session_upgrade_output")
       ("muxer", session.muxer.value)("local_address", local_endpoint.to_string())
       ("remote_address", remote_endpoint.to_string())("path", "direct")("direction", "outbound");
}

boost::asio::awaitable<forge::variant> forge_plugin_interop_fixture::pubsub_session_receipt(
    p2p::node::session_info connected) {
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   const auto deadline = std::chrono::steady_clock::now() + 4s;
   while (std::chrono::steady_clock::now() < deadline) {
      const auto snapshot = api<np::diagnostics_api>()->snapshot();
      for (const auto& session : snapshot.sessions) {
         if (session.id != connected.id) { continue; }
         // Pinned Rust pubsub-live owns only GossipSub, not Identify. Keep native
         // automatic Identify enabled, but capture its actual state without waiting for it.
         co_return pubsub_session_facts(session, connected, _local_peer);
      }
      timer.expires_after(5ms);
      co_await timer.async_wait(boost::asio::use_awaitable);
   }
   throw std::runtime_error{"PubSub retained session was not captured inside owned deadline"};
}

void forge_plugin_interop_fixture::sample_pubsub(std::string_view label) {
   const auto state = _pubsub;
   const auto snapshot = api<np::pubsub_api>()->snapshot();
   const auto diagnostics = api<np::diagnostics_api>();
   const auto native = diagnostics->snapshot();
   const auto scores = diagnostics->pubsub_scores();
   for (const auto& session : native.sessions) {
      if (state->connections.contains(session.id)) { continue; }
      if (session.authentication != p2p::peer_authentication::noise || !session.remote_endpoint ||
          !session.local_endpoint || session.muxer.value != "/yamux/1.0.0") {
         throw std::runtime_error{"PubSub connection lacks real Noise/Yamux socket facts"};
      }
      if (state->connections.size() >= 16) { throw std::runtime_error{"PubSub connection capture bound"}; }
      state->record("connection", "forge.node.diagnostics_api", object{}("peer_id", session.remote_peer.to_string())
          ("connection_id", std::to_string(session.id))("authenticated", true)
          ("remote_address", session.remote_endpoint->to_string())("local_address", session.local_endpoint->to_string())
          ("transport", "tcp")("security", "/noise")("muxer", session.muxer.value)
          ("authentication_basis", "native_session_upgrade_output"));
      state->connections.insert(session.id);
   }
   auto mesh = forge::variants{};
   auto peers = forge::variants{};
   if (scores.peers.size() > 16) { throw std::runtime_error{"PubSub score capture bound"}; }
   for (const auto& peer : scores.peers) {
      auto invalid = 0.;
      for (const auto& topic : peer.topics) {
         if (topic.subject.value != state->topic) { continue; }
         invalid = topic.invalid_message_deliveries;
         if (topic.in_mesh) { mesh.emplace_back(peer.peer.to_string()); }
      }
      peers.emplace_back(object{}("peer_id", peer.peer.to_string())("value", peer.value)
          ("invalid_deliveries", invalid)("app_specific_sample", peer.app_specific_sample)
          ("app_specific_score", peer.app_specific_score));
   }
   state->record("snapshot", "forge.node.pubsub_api-and-diagnostics_api", object{}("label", std::string{label})
       ("mesh_peer_ids", mesh)("peer_scores", peers)("application_score_calls", state->score_calls.load())
       ("subscriptions", static_cast<std::uint64_t>(snapshot.subscriptions))
       ("active_handlers", static_cast<std::uint64_t>(snapshot.active_handlers))
       ("messages_published", snapshot.messages_published)("messages_delivered", snapshot.messages_delivered)
       ("messages_accepted", snapshot.messages_accepted)("handler_failures", snapshot.handler_failures)
       ("core", object{}("peers", static_cast<std::uint64_t>(snapshot.core.peers))
           ("mesh_edges", static_cast<std::uint64_t>(snapshot.core.mesh_edges))
           ("messages_received", snapshot.core.messages_received)("messages_delivered", snapshot.core.messages_delivered)
           ("trace_failures", snapshot.core.trace_failures)("application_score_failures", snapshot.core.application_score_failures)));
   if (snapshot.handler_failures || snapshot.core.trace_failures || snapshot.core.application_score_failures || state->failed()) {
      throw std::runtime_error{"PubSub callback/capture failure"};
   }
}

void forge_plugin_interop_fixture::prepare_pubsub() {
   if (_pubsub->drained) { return; }
   if (_pubsub->subscription) {
      run(api<np::pubsub_api>()->unsubscribe(*_pubsub->subscription), "pubsub.unsubscribe");
      _pubsub->subscription.reset();
   }
   run(drain_handlers(api<np::pubsub_api>()), "pubsub.callback_drain");
   sample_pubsub("drained");
   _pubsub->drained = true;
}

void forge_plugin_interop_fixture::pubsub_command(const forge::variant& command, std::uint64_t sequence) {
   const auto state = _pubsub;
   const auto kind = text(command, "kind");
   if (state->prepared || state->failed()) { throw std::runtime_error{"PubSub command admission closed/failed"}; }
   if (kind == "connect") {
      const auto peer = p2p::peer_id::from_string(text(command, "peer_id"));
      const auto session = run(api<np::api>()->connect(p2p::parse_endpoint(text(command, "address")),
          {.expected_peer = peer, .allow_relay = false, .timeout = 4s, .allow_hole_punch = false}), "pubsub.connect");
      if (session.remote_peer != peer || session.id == 0 || session.muxer.value != "/yamux/1.0.0") {
         throw std::runtime_error{"PubSub authenticated target mismatch"};
      }
      const auto facts = run(pubsub_session_receipt(session), "pubsub.connect_session");
      state->record("session", "node.api.connect-and-node.diagnostics-api.session",
          object{facts}("command_sequence", sequence));
      sample_pubsub("connected");
   } else if (kind == "publish" || kind == "publish_binary" || kind == "publish_oversized") {
      const auto data = command_payload(command);
      const auto bound = state->typed ? 96u : 128u;
      check_probe(kind, data.size(), state->typed);
      if (kind == "publish_oversized") {
         auto rejected = false;
         const auto before = api<np::pubsub_api>()->snapshot();
         try { static_cast<void>(run(api<np::pubsub_api>()->publish({state->topic}, data), "pubsub.oversized_publish")); }
         catch (const np::exceptions::message_too_large&) {
            if (state->typed) { throw; }
            rejected = true;
            state->record("publish_rejected", "forge.node.pubsub_api.publish", object{current_failure("pubsub.oversized_publish")}
                ("command_sequence", sequence)("payload", data_facts(data))("expected_limit", bound));
         } catch (const p2p::exceptions::backpressure_rejected&) {
            if (!state->typed) { throw; }
            rejected = true;
            state->record("publish_rejected", "forge.node.pubsub_api.publish", object{current_failure("pubsub.oversized_publish")}
                ("command_sequence", sequence)("payload", data_facts(data))("expected_limit", bound));
         }
         if (!rejected) { throw std::runtime_error{"PubSub oversized publish unexpectedly succeeded"}; }
         const auto after = api<np::pubsub_api>()->snapshot();
         if (after.messages_published != before.messages_published ||
             after.core.messages_published != before.core.messages_published) {
            throw std::runtime_error{"PubSub rejected payload incremented published counter"};
         }
         state->record("publish_rejection_counters", "forge.node.pubsub_api.snapshot", object{}("command_sequence", sequence)
             ("messages_published_before", before.messages_published)("messages_published_after", after.messages_published)
             ("core_published_before", before.core.messages_published)("core_published_after", after.core.messages_published));
      } else {
         const auto message = run(api<np::pubsub_api>()->publish({state->topic}, data), "pubsub.publish");
         if (message.data != data || message.source.to_string() != _local_peer || !message.author ||
             message.author->to_string() != _local_peer || message.subject.value != state->topic) {
            throw std::runtime_error{"PubSub publish receipt differs from actual request"};
         }
         state->record("publish", "forge.node.pubsub_api.publish", object{data_facts(message.data)}
             ("command_sequence", sequence)("topic", message.subject.value)("author_peer", message.author->to_string())
             ("seqno_hex", forge::codec::hex::encode(message.seqno))("at_payload_limit", message.data.size() == bound)
             ("payload_hex", forge::codec::hex::encode(message.data)));
      }
   } else if (kind == "sample") { sample_pubsub(text(command, "label")); }
   else if (kind == "prepare_shutdown") {
      check_prepare(command, state->actor, state->token, _local_peer);
      prepare_pubsub();
      state->prepared = true;
      state->record("shutdown_prepared", "forge.fixture.official_api_drain", object{}("command_sequence", sequence)
          ("actor", state->actor)("case_token", state->token)("local_peer_id", _local_peer)
          ("admission_closed", true)("pending_commands", 0u)("active_handlers", 0u));
   }
   state->record("command_done", "forge.fixture.official_api_operation", object{}("command_sequence", sequence)
       ("command_kind", kind)("status", "ok"));
   if (state->failed()) { throw std::runtime_error{"PubSub command receipt capture failed"}; }
   set_phase("pubsub.control");
}

bool forge_plugin_interop_fixture::poll_pubsub_control(bool stopping) {
   const auto next = read_controls(argument("control-file"), !_pubsub->control_seen);
   _pubsub->control_seen = _pubsub->control_seen || std::filesystem::exists(argument("control-file"));
   const auto batch = controls(_pubsub->prefix, next, _pubsub->offset, _pubsub->commands, _pubsub->prepared, stopping);
   _pubsub->prefix = next;
   for (const auto& command : batch) {
      if (std::chrono::steady_clock::now() >= _pubsub->deadline) { throw std::runtime_error{"PubSub control phase deadline"}; }
      pubsub_command(command, _pubsub->commands + 1);
      ++_pubsub->commands;
   }
   if (stopping && !_pubsub->prepared) { throw std::runtime_error{"PubSub stop before prepare acknowledgement"}; }
   auto sampled = false;
   if (!stopping && std::chrono::steady_clock::now() >= _pubsub->next_sample) {
      sample_pubsub("periodic");
      _pubsub->next_sample = std::chrono::steady_clock::now() + 250ms;
      sampled = true;
   }
   return sampled || !batch.empty();
}

bool forge_plugin_interop_fixture::pubsub_failed() const { return _pubsub && _pubsub->failed(); }

void forge_plugin_interop_fixture::pubsub_ready(object& ready) const {
   _pubsub->deadline = std::chrono::steady_clock::now() + 45s;
   ready("schema_version", 1u)("actor", _pubsub->actor)("case_token", _pubsub->token)("local_peer_id", _local_peer)
       ("topic", _pubsub->topic)("version", _pubsub->version)("pubsub_policy", argument("pubsub-policy"))
       ("ready", true)("subscription_created", _pubsub->subscription.has_value());
}

forge::variant forge_plugin_interop_fixture::pubsub_receipt(bool final) const {
   if (!_pubsub) { return {}; }
   auto lock = std::lock_guard{_pubsub->mutex};
   return object{}("actor", _pubsub->actor)("case_token", _pubsub->token)("topic", _pubsub->topic)
       ("version", _pubsub->version)("pubsub_policy", argument("pubsub-policy"))("events", _pubsub->events)
       ("overflow", _pubsub->overflow)("capture_error", _pubsub->capture_error)
       ("capture_failure", _pubsub->capture_failure)
       ("error", _pubsub->overflow || _pubsub->capture_error ? forge::variant{"bounded_pubsub_capture_failed"} : forge::variant{})
       ("finalized", final)("joined", _plugins_joined && _scheduler_joined)
       ("command_admission_closed", _pubsub->prepared)("subscription_drained", _pubsub->drained)
       ("commands_completed", _pubsub->commands)("application_score_calls", _pubsub->score_calls.load())
       ("native_tracer_installed", _pubsub->typed)
       ("declarative_source", "forge.codec.yaml.read_document")
       ("evidence_boundary", _pubsub->typed ? "actual native trace plus consumer callbacks" : "consumer callbacks and actual counters; no native commit trace");
}

int forge_plugin_interop_fixture::pubsub_self_test() {
   auto count = 0u;
   const auto reject = [&count](auto action) {
      auto failed = false;
      try { action(); } catch (...) { failed = true; }
      if (!failed) { throw std::runtime_error{"PubSub self-test accepted invalid control"}; }
      ++count;
   };
   auto offset = std::size_t{};
   const auto sample = std::string{"{\"sequence\":1,\"kind\":\"sample\",\"label\":\"proof\"}\n"};
   const auto batch = controls({}, sample, offset, 0, false, false);
   if (batch.size() != 1 || offset != sample.size()) { throw std::runtime_error{"PubSub complete control parse failed"}; }
   ++count;
   auto split_cursor = std::size_t{};
   const auto partial = std::string_view{sample}.substr(0, 20);
   if (!controls({}, partial, split_cursor, 0, false, false).empty() || split_cursor != 0 ||
       controls(partial, sample, split_cursor, 0, false, false).size() != 1) {
      throw std::runtime_error{"PubSub split append parse failed"};
   }
   ++count;
   for (const auto malformed : {
       "{\"sequence\":2,\"kind\":\"sample\",\"label\":\"x\"}\n",
       "{\"sequence\":1,\"kind\":\"sample\",\"label\":\"x\",\"extra\":true}\n",
       "{\"sequence\":1,\"kind\":\"sample\",\"kind\":\"publish\",\"label\":\"x\"}\n",
       "{\"sequence\":1,\"kind\":\"sample\",\"label\":\"periodic\"}\n",
       "{\"sequence\":1,\"kind\":\"publish_binary\",\"payload_hex\":\"0x\"}\n",
       "{\"sequence\":1,\"kind\":\"unknown\"}\n", "null\n", "\n"}) {
      reject([&] { auto cursor = std::size_t{}; static_cast<void>(controls({}, malformed, cursor, 0, false, false)); });
   }
   reject([&] { static_cast<void>(controls(sample, {}, offset, 1, false, false)); });
   reject([&] { static_cast<void>(controls(sample, sample + sample, offset, 1, true, false)); });
   reject([&] { auto cursor = std::size_t{}; static_cast<void>(controls({}, "{", cursor, 0, false, true)); });
   reject([&] { auto cursor = std::size_t{}; static_cast<void>(controls({}, sample, cursor, 64, false, false)); });
   reject([&] { auto cursor = std::size_t{}; static_cast<void>(controls({}, std::string(line_limit + 1, 'x'), cursor, 0, false, false)); });
   const auto prepare = std::string{"{\"sequence\":1,\"kind\":\"prepare_shutdown\",\"actor\":\"victim\",\"case_token\":\"x\",\"local_peer_id\":\"x\"}\n"};
   reject([&] { auto cursor = std::size_t{}; static_cast<void>(controls({}, prepare + "{", cursor, 0, false, false)); });
   const auto parsed_prepare = forge::codec::json::read_value(prepare);
   if (!parsed_prepare.ok()) { throw std::runtime_error{"PubSub prepare self-test input failed"}; }
   check_prepare(parsed_prepare.value, "victim", "x", "x");
   ++count;
   reject([&] { check_prepare(parsed_prepare.value, "sink", "x", "x"); });
   reject([&] { check_prepare(parsed_prepare.value, "victim", "foreign", "x"); });
   reject([&] { check_prepare(parsed_prepare.value, "victim", "x", "foreign"); });
   for (const auto typed : {false, true}) {
      const auto bound = typed ? 96u : 128u;
      check_probe("publish_binary", bound, typed);
      check_probe("publish_oversized", bound + 1, typed);
      ++count;
      reject([&] { check_probe("publish_binary", bound + 1, typed); });
      reject([&] { check_probe("publish_oversized", bound, typed); });
      reject([&] { check_probe("publish_oversized", bound + 2, typed); });
   }
   if (!declarative_pubsub(false).is_object() || !declarative_pubsub(true).is_object()) {
      throw std::runtime_error{"PubSub YAML self-test failed"};
   }
   ++count;
   const auto local_peer = std::string{"12D3KooWBda3fFGfHQnbNMo9XYqmKkMrw6ChD64LHwzw1ue3Ysri"};
   const auto remote_peer = p2p::peer_id::from_string("12D3KooWEEpgMh1ErSouV5AiUaJPUuFzatJopeMGk3JgdtFpiLjx");
   const auto connected = p2p::node::session_info{
       .remote_peer = remote_peer, .muxer = {"/yamux/1.0.0"}, .id = 7};
   auto session = p2p::diagnostics::session{};
   session.id = connected.id;
   session.remote_peer = connected.remote_peer;
   session.muxer = connected.muxer;
   session.local_endpoint = p2p::parse_endpoint("/ip4/127.0.0.1/tcp/41001");
   session.remote_endpoint = p2p::parse_endpoint("/ip4/127.0.0.1/tcp/41002");
   session.authentication = p2p::peer_authentication::noise;
   for (const auto& [identify_state, name] : {
       std::pair{p2p::identify::state::unknown, "unknown"},
       std::pair{p2p::identify::state::identifying, "identifying"},
       std::pair{p2p::identify::state::identified, "identified"},
       std::pair{p2p::identify::state::failed, "failed"}}) {
      session.identify_state = identify_state;
      const auto facts = pubsub_session_facts(session, connected, local_peer);
      if (facts["identify_state"].get_string() != name || facts["identify_required"].as_bool() ||
          facts["identify_observed"].as_bool() != (identify_state == p2p::identify::state::identified) ||
          facts["authentication"].get_string() != "noise" || facts["closed"].as_bool() ||
          facts.get_object().contains("stream_authentication")) {
         throw std::runtime_error{"PubSub session receipt invented Identify or stream evidence"};
      }
      session.identify_state = p2p::identify::state::unknown;
      if (facts["identify_state"].get_string() != name) {
         throw std::runtime_error{"PubSub session observation was not immutable"};
      }
      ++count;
   }
   const auto invalid_session = [&reject, &session, &connected, &local_peer](auto mutate) {
      reject([&] {
         auto invalid = session;
         mutate(invalid);
         static_cast<void>(pubsub_session_facts(invalid, connected, local_peer));
      });
   };
   invalid_session([](auto& invalid) { ++invalid.id; });
   invalid_session([&](auto& invalid) { invalid.remote_peer = p2p::peer_id::from_string(local_peer); });
   invalid_session([](auto& invalid) { invalid.closed = true; });
   for (const auto auth : {p2p::peer_authentication::unverified, p2p::peer_authentication::libp2p_tls,
                          p2p::peer_authentication::quic_tls}) {
      invalid_session([&](auto& invalid) { invalid.authentication = auth; });
   }
   invalid_session([](auto& invalid) { invalid.muxer = {"/other-muxer/1"}; });
   invalid_session([](auto& invalid) { invalid.path = p2p::path::kind::relay; });
   invalid_session([&](auto& invalid) { invalid.relay_peer = remote_peer; });
   invalid_session([](auto& invalid) { invalid.direction = p2p::diagnostics::session_direction::inbound; });
   invalid_session([](auto& invalid) { invalid.local_endpoint.reset(); });
   invalid_session([](auto& invalid) { invalid.remote_endpoint.reset(); });
   invalid_session([](auto& invalid) { invalid.local_endpoint->transport.port = 0; });
   invalid_session([](auto& invalid) { invalid.remote_endpoint->transport.port = 0; });
   invalid_session([](auto& invalid) { invalid.remote_endpoint = p2p::parse_endpoint("/ip4/127.0.0.1/udp/41002/quic-v1"); });
   invalid_session([&](auto& invalid) { invalid.remote_endpoint->peer = p2p::peer_id::from_string(local_peer); });
   invalid_session([&](auto& invalid) { invalid.local_endpoint->peer = remote_peer; });
   invalid_session([](auto& invalid) { invalid.identify_state = static_cast<p2p::identify::state>(255); });
   reject([&] {
      auto invalid = connected;
      invalid.id = 0;
      static_cast<void>(pubsub_session_facts(session, invalid, local_peer));
   });
   auto state = pubsub_state{};
   auto bytes = std::vector<std::uint8_t>{0, 255, 1};
   state.record("subscription_delivery", "self-test", object{data_facts(bytes)}("payload_hex", forge::codec::hex::encode(bytes)));
   bytes.assign(3, 42);
   if (state.events.front()["payload_hex"].get_string() != "00ff01") { throw std::runtime_error{"PubSub observation was not immutable"}; }
   ++count;
   const auto session_source = std::string{"node.api.connect-and-node.diagnostics-api.session"};
   state.record("session", session_source, object{pubsub_session_facts(session, connected, local_peer)}
       ("command_sequence", 1u));
   auto source_count = std::size_t{};
   for (const auto& field : state.events.back().get_object()) {
      if (field.key() == "source") { ++source_count; }
   }
   if (source_count != 1 || state.events.back()["source"].get_string() != session_source) {
      throw std::runtime_error{"PubSub event envelope duplicated its source"};
   }
   ++count;
   state.capture_bytes = capture_limit;
   state.record("snapshot", "self-test", {});
   if (!state.failed()) { throw std::runtime_error{"PubSub overflow was not sticky"}; }
   ++count;
   std::cout << "configured PubSub fixture self-test: " << count << " checks\n";
   return 0;
}

} // namespace forge::test::libp2p_interop
