module;

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#include <forge/exceptions/macros.hpp>

module forge.net.p2p.node;

import forge.crypto.asymmetric;
import forge.net.p2p.envelope;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.rendezvous;

#include "details/certified_peer_record.hxx"

namespace forge::net::p2p::detail::certified_peer_record {
namespace {

constexpr auto domain = std::string_view{"libp2p-peer-record"};

} // namespace

signed_envelope seal(const rendezvous::peer_record& record, const public_key& key,
                     const forge::crypto::asymmetric::private_key& private_key) {
   const auto payload = rendezvous::codec::encode_peer_record(record);
   return signed_envelope::seal(key, private_key, domain, payload_type, payload);
}

rendezvous::peer_record open(const signed_envelope& envelope, std::optional<peer_id> expected_signer) {
   if (std::ranges::equal(envelope.payload_type, payload_type)) {
      envelope.verify(domain, expected_signer);
      auto record = rendezvous::codec::decode_peer_record(envelope.payload);
      if (record.peer != envelope.signer()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_identity, "Identify signed peer record peer id mismatch");
      }
      return record;
   }
   if (envelope.payload_type == rendezvous::codec::peer_record_payload_type()) {
      return rendezvous::codec::open_peer_record(envelope, std::move(expected_signer));
   }
   FORGE_THROW_EXCEPTION(exceptions::codec_error, "Identify signed peer record has unsupported payload type");
}

} // namespace forge::net::p2p::detail::certified_peer_record
