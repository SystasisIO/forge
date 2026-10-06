module;

#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>

module forge.net.p2p.node;

import forge.crypto.asymmetric;
import forge.crypto.asymmetric.x25519;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.negotiation;
import forge.net.p2p.stream;
import forge.multiformats.exceptions;
import forge.multiformats.varint;

#include "details/noise_handshake.hxx"
#include "details/noise_symmetric_state.hxx"
#include "details/secure_io.hxx"
#include "details/cancellation_latch.hxx"
#include "details/identity_signature.hxx"
#include "details/libp2p_identity_material.hxx"
#include "details/protobuf.hxx"

namespace forge::net::p2p::detail {

namespace {

[[nodiscard]] x25519_key make_x25519_key() {
   auto key = forge::crypto::asymmetric::x25519::private_key::generate();
   return x25519_key{.key = key, .public_key = key.get_public_key().serialize()};
}

[[nodiscard]] std::vector<std::uint8_t> x25519_dh(const forge::crypto::asymmetric::x25519::private_key& private_key,
                                                  std::span<const std::uint8_t, 32> remote_public) {
   auto public_key = forge::crypto::asymmetric::x25519::public_key_data{};
   std::copy(remote_public.begin(), remote_public.end(), public_key.begin());
   const auto secret = private_key.get_shared_secret(forge::crypto::asymmetric::x25519::public_key{public_key});
   return {secret.begin(), secret.end()};
}

[[nodiscard]] std::array<std::uint8_t, 32> checked_x25519_public(std::span<const std::uint8_t> bytes) {
   if (bytes.size() != 32) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "Noise X25519 public key must be 32 bytes");
   }
   auto out = std::array<std::uint8_t, 32>{};
   std::copy(bytes.begin(), bytes.end(), out.begin());
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> noise_signature_payload(std::span<const std::uint8_t> static_key) {
   auto out = std::vector<std::uint8_t>{};
   constexpr auto prefix = std::string_view{"noise-libp2p-static-key:"};
   out.insert(out.end(), prefix.begin(), prefix.end());
   out.insert(out.end(), static_key.begin(), static_key.end());
   return out;
}

[[nodiscard]] detail::noise_handshake_payload make_noise_payload(const libp2p_identity_material& identity,
                                                                 std::span<const std::uint8_t> static_key) {
   return detail::noise_handshake_payload{
       .identity_key = identity.public_key,
       .identity_signature =
           sign_identity(require_libp2p_identity_private_key(identity), noise_signature_payload(static_key)),
       .stream_muxers = {"/yamux/1.0.0"},
   };
}

} // namespace

constexpr auto max_noise_muxers = std::size_t{100};

std::vector<std::uint8_t> encode_noise_payload(const noise_handshake_payload& value) {
   auto out = std::vector<std::uint8_t>{};
   detail::append_bytes(out, 1, value.identity_key);
   detail::append_bytes(out, 2, value.identity_signature);
   auto extensions = std::vector<std::uint8_t>{};
   for (const auto& muxer : value.stream_muxers) {
      detail::append_string(extensions, 2, muxer);
   }
   if (!extensions.empty()) {
      detail::append_bytes(out, 4, extensions);
   }
   return out;
}

noise_handshake_payload decode_noise_payload(std::span<const std::uint8_t> bytes) {
   auto out = noise_handshake_payload{};
   auto discard_muxers = false;
   auto in = detail::reader{bytes};
   while (!in.done()) {
      const auto [field, type] = in.key();
      if (type != detail::wire_type::length_delimited) {
         if (field == 1 || field == 2 || field == 4) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "Noise handshake field has an invalid wire type");
         }
         in.skip(type);
         continue;
      }
      switch (field) {
      case 1:
         out.identity_key = in.bytes();
         break;
      case 2:
         out.identity_signature = in.bytes();
         break;
      case 4: {
         auto ext_bytes = in.bytes();
         auto ext = detail::reader{ext_bytes};
         while (!ext.done()) {
            const auto [ext_field, ext_type] = ext.key();
            if (ext_field == 2) {
               if (ext_type != detail::wire_type::length_delimited) {
                  FORGE_THROW_EXCEPTION(exceptions::codec_error, "Noise muxer field has an invalid wire type");
               }
               if (discard_muxers || out.stream_muxers.size() == max_noise_muxers) {
                  // Go transportEarlyDataHandler.Received discards the entire
                  // offer over 100 protocols, including any earlier match.
                  discard_muxers = true;
                  out.stream_muxers.clear();
                  ext.skip(ext_type);
               } else {
                  out.stream_muxers.push_back(ext.string());
               }
            } else {
               ext.skip(ext_type);
            }
         }
         break;
      }
      default:
         in.skip(type);
         break;
      }
   }
   return out;
}

verified_noise_payload verify_noise_payload(const noise_handshake_payload& payload,
                                            std::span<const std::uint8_t> static_key,
                                            const std::optional<peer_id>& expected_peer) {
   if (payload.identity_key.empty() || payload.identity_signature.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "Noise handshake payload is missing identity proof");
   }
   const auto key = decode_public_key(payload.identity_key);
   const auto peer = make_peer_id(key);
   if (expected_peer && peer != *expected_peer) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "Noise identity peer id mismatch");
   }
   if (!verify_identity_signature(key, noise_signature_payload(static_key), payload.identity_signature)) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "Noise identity signature is invalid");
   }
   // With only Yamux, initiator-preference ordering has one possible match.
   // Empty/discarded offers use legacy fallback. For nonempty no-overlap,
   // enforce the pinned spec's MUST-fail rule (Go instead tries multistream).
   if (payload.stream_muxers.empty()) {
      return verified_noise_payload{.peer = peer};
   }
   for (const auto& muxer : payload.stream_muxers) {
      if (muxer == "/yamux/1.0.0") {
         return verified_noise_payload{.peer = peer, .muxer = protocol_id{.value = muxer}};
      }
   }
   FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "Noise peer offered no supported stream muxer");
}

boost::asio::awaitable<noise_result> noise_initiator(forge::net::p2p::stream stream,
                                                     const libp2p_identity_material& identity,
                                                     std::optional<peer_id> expected_peer,
                                                     const std::shared_ptr<cancellation_latch>& cancel_current) {
   auto io = std::make_shared<secure_io>(std::move(stream));
   if (cancel_current) {
      cancel_current->arm([io] { io->cancel(); });
   }
   auto symmetric = noise_symmetric_state{};
   auto ephemeral = make_x25519_key();
   auto local_static = make_x25519_key();

   symmetric.mix_hash(ephemeral.public_key);
   (void)symmetric.encrypt_and_hash(std::span<const std::uint8_t>{});
   co_await io->write_plain_frame(ephemeral.public_key);

   auto message2 = co_await io->read_plain_frame();
   if (message2.size() < 96) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "Noise responder message is truncated");
   }
   const auto responder_ephemeral = checked_x25519_public(std::span<const std::uint8_t>{message2}.subspan(0, 32));
   symmetric.mix_hash(responder_ephemeral);
   symmetric.mix_key(x25519_dh(ephemeral.key, responder_ephemeral));
   const auto responder_static_cipher = std::span<const std::uint8_t>{message2}.subspan(32, 48);
   const auto responder_static_plain = symmetric.decrypt_and_hash(responder_static_cipher);
   const auto responder_static = checked_x25519_public(responder_static_plain);
   symmetric.mix_key(x25519_dh(ephemeral.key, responder_static));
   const auto responder_payload = symmetric.decrypt_and_hash(std::span<const std::uint8_t>{message2}.subspan(80));
   auto decoded_responder_payload = detail::decode_noise_payload(responder_payload);
   const auto verified_responder =
       detail::verify_noise_payload(decoded_responder_payload, responder_static, expected_peer);

   auto message3 = symmetric.encrypt_and_hash(local_static.public_key);
   symmetric.mix_key(x25519_dh(local_static.key, responder_ephemeral));
   const auto payload = detail::encode_noise_payload(make_noise_payload(identity, local_static.public_key));
   auto encrypted_payload = symmetric.encrypt_and_hash(payload);
   message3.insert(message3.end(), encrypted_payload.begin(), encrypted_payload.end());
   co_await io->write_plain_frame(message3);

   auto states = symmetric.split();
   io->set_cipher_states(std::move(states[1]), std::move(states[0]));
   co_return noise_result{
       .peer = verified_responder.peer, .secure = std::move(io), .muxer = verified_responder.muxer};
}

boost::asio::awaitable<noise_result> noise_responder(forge::net::p2p::stream stream,
                                                     const libp2p_identity_material& identity,
                                                     std::optional<peer_id> expected_peer,
                                                     const std::shared_ptr<cancellation_latch>& cancel_current) {
   auto io = std::make_shared<secure_io>(std::move(stream));
   if (cancel_current) {
      cancel_current->arm([io] { io->cancel(); });
   }
   auto symmetric = noise_symmetric_state{};
   auto initiator_ephemeral = checked_x25519_public(co_await io->read_plain_frame());
   symmetric.mix_hash(initiator_ephemeral);
   (void)symmetric.decrypt_and_hash(std::span<const std::uint8_t>{});

   auto ephemeral = make_x25519_key();
   auto local_static = make_x25519_key();
   auto message2 = std::vector<std::uint8_t>{ephemeral.public_key.begin(), ephemeral.public_key.end()};
   symmetric.mix_hash(ephemeral.public_key);
   symmetric.mix_key(x25519_dh(ephemeral.key, initiator_ephemeral));
   auto encrypted_static = symmetric.encrypt_and_hash(local_static.public_key);
   message2.insert(message2.end(), encrypted_static.begin(), encrypted_static.end());
   symmetric.mix_key(x25519_dh(local_static.key, initiator_ephemeral));
   const auto payload = detail::encode_noise_payload(make_noise_payload(identity, local_static.public_key));
   auto encrypted_payload = symmetric.encrypt_and_hash(payload);
   message2.insert(message2.end(), encrypted_payload.begin(), encrypted_payload.end());
   co_await io->write_plain_frame(message2);

   const auto message3 = co_await io->read_plain_frame();
   if (message3.size() < 64) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "Noise initiator message is truncated");
   }
   const auto initiator_static_plain =
       symmetric.decrypt_and_hash(std::span<const std::uint8_t>{message3}.subspan(0, 48));
   const auto initiator_static = checked_x25519_public(initiator_static_plain);
   symmetric.mix_key(x25519_dh(ephemeral.key, initiator_static));
   const auto initiator_payload = symmetric.decrypt_and_hash(std::span<const std::uint8_t>{message3}.subspan(48));
   auto decoded_initiator_payload = detail::decode_noise_payload(initiator_payload);
   const auto verified_initiator =
       detail::verify_noise_payload(decoded_initiator_payload, initiator_static, expected_peer);

   auto states = symmetric.split();
   io->set_cipher_states(std::move(states[0]), std::move(states[1]));
   co_return noise_result{
       .peer = verified_initiator.peer, .secure = std::move(io), .muxer = verified_initiator.muxer};
}

} // namespace forge::net::p2p::detail
