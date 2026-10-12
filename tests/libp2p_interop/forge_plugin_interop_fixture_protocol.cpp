#include "forge_plugin_interop_fixture.hxx"

import forge.multiformats.varint;

namespace forge::test::libp2p_interop {
namespace {
namespace p2p = forge::net::p2p;
namespace np = forge::plugins::net::p2p::node;
using object = forge::mutable_variant_object;
using namespace std::chrono_literals;
constexpr auto maximum_payload = std::size_t{256 * 1024};

std::string authentication(p2p::peer_authentication value) {
   switch (value) {
   case p2p::peer_authentication::noise: return "noise";
   case p2p::peer_authentication::libp2p_tls: return "libp2p_tls";
   case p2p::peer_authentication::quic_tls: return "quic_tls";
   default: throw std::runtime_error{"stream is not authenticated"};
   }
}
} // namespace

std::vector<std::uint8_t> forge_plugin_interop_fixture::frame(std::span<const std::uint8_t> bytes) {
   auto out = forge::multiformats::varint_encode(bytes.size());
   out.insert(out.end(), bytes.begin(), bytes.end());
   return out;
}

boost::asio::awaitable<std::vector<std::uint8_t>> forge_plugin_interop_fixture::read_frame(p2p::stream& stream) {
   // The unchanged donor echo protocol uses canonical unsigned-varint framing,
   // not transport::frame's opt-in framing. Bound the one expected response.
   auto buffered = std::vector<std::uint8_t>{};
   auto size = std::size_t{};
   auto prefix = std::size_t{};
   auto complete = false;
   while (!complete || buffered.size() < prefix + size) {
      auto next = co_await stream.async_read();
      if (next.empty() || next.size() > maximum_payload + 4 - buffered.size()) {
         throw std::runtime_error{"truncated or oversized echo frame"};
      }
      buffered.insert(buffered.end(), next.begin(), next.end());
      while (!complete && prefix < buffered.size()) {
         if (prefix == 3) { throw std::runtime_error{"oversized echo prefix"}; }
         const auto byte = buffered[prefix++];
         if (!(byte & 128U)) {
            const auto decoded = forge::multiformats::varint_decode(
                std::span<const std::uint8_t>{buffered}.first(prefix));
            if (decoded.value == 0 || decoded.value > maximum_payload) {
               throw std::runtime_error{"echo frame size outside fixture bound"};
            }
            size = static_cast<std::size_t>(decoded.value);
            complete = true;
         }
      }
   }
   if (buffered.size() != prefix + size) { throw std::runtime_error{"concatenated echo response"}; }
   co_return std::vector<std::uint8_t>{buffered.begin() + static_cast<std::ptrdiff_t>(prefix), buffered.end()};
}

forge::variant forge_plugin_interop_fixture::payload_receipt(std::span<const std::uint8_t> bytes) {
   return object{}("bytes", static_cast<std::uint64_t>(bytes.size()))
       ("sha256", forge::crypto::digest::sha256::hash(bytes).str())
       ("contains_zero", std::find(bytes.begin(), bytes.end(), 0) != bytes.end());
}

boost::asio::awaitable<forge::variant> forge_plugin_interop_fixture::session_receipt(
    p2p::peer_id peer, std::uint64_t id, p2p::peer_authentication stream_authentication) {
   const auto executor = co_await boost::asio::this_coro::executor;
   auto timer = boost::asio::steady_timer{executor};
   const auto deadline = std::chrono::steady_clock::now() + 4s;
   while (std::chrono::steady_clock::now() < deadline) {
      const auto state = api<np::diagnostics_api>()->snapshot();
      for (const auto& session : state.sessions) {
         if (session.id != id || session.remote_peer != peer || session.closed ||
             session.identify_state != p2p::identify::state::identified) { continue; }
         if (!session.local_endpoint || !session.remote_endpoint || session.path != p2p::path::kind::direct ||
             session.authentication != stream_authentication) {
            throw std::runtime_error{"actual stream and retained direct session disagree"};
         }
         const auto quic = session.authentication == p2p::peer_authentication::quic_tls;
         if (!quic && session.muxer.value != "/yamux/1.0.0") { throw std::runtime_error{"TCP did not negotiate Yamux"}; }
         co_return object{}("source", "node.protocol-api.stream-and-node.diagnostics-api.session")
             ("session_id", session.id)("local_peer_id", _local_peer)("remote_peer_id", peer.to_string())
             ("identify_state", "identified")("authentication", authentication(session.authentication))
             ("stream_authentication", authentication(stream_authentication))
             ("muxer", session.muxer.value)("local_address", session.local_endpoint->to_string())
             ("remote_address", session.remote_endpoint->to_string())("path", "direct")
             ("direction", session.direction == p2p::diagnostics::session_direction::inbound ? "inbound" : "outbound");
      }
      timer.expires_after(5ms);
      co_await timer.async_wait(boost::asio::use_awaitable);
   }
   throw std::runtime_error{"actual session did not complete Identify inside owned deadline"};
}

boost::asio::awaitable<void> forge_plugin_interop_fixture::receive(p2p::node::incoming_protocol_stream incoming) {
   auto phase = std::string_view{"protocol.inbound_echo.read_frame"};
   try {
      const auto auth = incoming.stream.authentication();
      const auto bytes = co_await read_frame(incoming.stream);
      // Capture before responding: the donor cannot exit and remove this session
      // between its successful echo and our independent Identify observation.
      phase = "protocol.inbound_echo.session_receipt";
      const auto session = co_await session_receipt(incoming.session.remote_peer, incoming.session.id, auth);
      phase = "protocol.inbound_echo.response_write";
      co_await incoming.stream.async_write(frame(bytes));
      phase = "protocol.inbound_echo.stream_close";
      co_await incoming.stream.async_close();
      phase = "protocol.inbound_echo.publish_exchange";
      auto lock = std::lock_guard{_mutex};
      if (!_exchange.is_null()) { throw std::runtime_error{"unexpected second echo exchange"}; }
      _exchange = object{}("source", "official-node.protocol-handler")("protocol", incoming.protocol.value)
          ("session", session)("request", payload_receipt(bytes))("response", payload_receipt(bytes))
          ("response_write_completed", true)("stream_closed", true);
   } catch (...) {
      incoming.stream.request_cancel();
      auto lock = std::lock_guard{_mutex};
      _errors.emplace_back(current_failure(phase));
      throw;
   }
}

boost::asio::awaitable<void> forge_plugin_interop_fixture::exchange() {
   const auto peer = p2p::peer_id::from_string(argument("peer-id"));
   set_phase("protocol.outbound_echo.connect");
   const auto session = co_await api<np::api>()->connect(forge::multiformats::multiaddr::parse(argument("addr")),
       {.expected_peer = peer, .allow_relay = false, .timeout = 6s, .direct_attempt_timeout = 2s,
        .allow_hole_punch = false});
   if (session.remote_peer != peer) { throw std::runtime_error{"connect returned a different authenticated peer"}; }
   set_phase("protocol.outbound_echo.open");
   auto stream = co_await api<np::protocol_api>()->open(peer, {"/forge/interop/relay-echo/1"},
       {.allow_relay = false, .timeout = 2s, .allow_hole_punch = false});
   try {
      set_phase("protocol.outbound_echo.session_receipt");
      const auto facts = co_await session_receipt(peer, session.id, stream.authentication());
      auto bytes = std::vector<std::uint8_t>{};
      if (argument("scenario") == "echo_large") {
         bytes.resize(192 * 1024);
         for (auto index = std::size_t{}; index < bytes.size(); ++index) { bytes[index] = static_cast<std::uint8_t>(index % 251); }
      } else {
         const auto payload = argument("payload");
         bytes.assign(payload.begin(), payload.end());
      }
      if (bytes.empty() || bytes.size() > maximum_payload) { throw std::runtime_error{"echo challenge outside fixture bound"}; }
      set_phase("protocol.outbound_echo.request_write");
      co_await stream.async_write(frame(bytes));
      set_phase("protocol.outbound_echo.response_read");
      const auto response = co_await read_frame(stream);
      if (response != bytes) { throw std::runtime_error{"binary echo challenge mismatch"}; }
      set_phase("protocol.outbound_echo.stream_close");
      co_await stream.async_close();
      auto lock = std::lock_guard{_mutex};
      _exchange = object{}("source", "official-node.protocol-api.open")("protocol", "/forge/interop/relay-echo/1")
          ("session", facts)("request", payload_receipt(bytes))("response", payload_receipt(response))
          ("response_write_completed", true)("stream_closed", true)("expected_peer_id", peer.to_string())
          ("dial_address", argument("addr"));
   } catch (...) { stream.request_cancel(); throw; }
}

} // namespace forge::test::libp2p_interop
