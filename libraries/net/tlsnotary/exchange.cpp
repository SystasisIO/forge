module;

#include <forge/exceptions/macros.hpp>
#include <cstdint>
#include <span>
#include <string_view>

module forge.net.tlsnotary.exchange;

import forge.net.tlsnotary.exceptions;
import forge.crypto.digest.sha256;
import forge.raw.raw;

namespace forge::net::tlsnotary {
namespace {

void require_binding(bool condition) {
   if (!condition) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "TLSNotary exchange differs from the trusted source policy");
   }
}

} // namespace

void bind_exchange(const result& verified, const forge::crypto::exchange::manifest& offered,
                   const forge::crypto::digest::sha256& expected_context, std::string_view expected_server,
                   byte_range body, std::span<const forge::crypto::core::bytes> ciphertext) {
   forge::crypto::exchange::validate(offered);
   require_binding(expected_context != forge::crypto::digest::sha256{} && offered.context == expected_context &&
                   !expected_server.empty() && expected_server.size() <= 253 &&
                   verified.server_name == expected_server && verified.sent_size > 0 && verified.sent_size <= 16384 &&
                   verified.retained.has_value() && verified.openings.empty() && !verified.sent.empty());
   const auto& evidence = *verified.retained;
   require_binding(
       evidence.format == 1 && evidence.implementation == forge::net::tlsnotary::evidence{}.implementation &&
       evidence.server_name == verified.server_name && evidence.sent_size == verified.sent_size &&
       evidence.received_size == verified.received_size && evidence.commitments.size() == offered.blocks.size());
   require_binding(body.begin > 0 && body.begin <= 16384 && body.end > body.begin &&
                   body.end == verified.received_size &&
                   body.end - body.begin <= forge::crypto::exchange::block_bytes * forge::crypto::exchange::max_blocks);
   require_binding(verified.received.size() == 1 && verified.received.front().begin == 0 &&
                   verified.received.front().bytes.size() == body.begin && ciphertext.size() == offered.blocks.size());
   auto next = body.begin;
   for (auto index = std::size_t{0}; index < offered.blocks.size(); ++index) {
      const auto& source = evidence.commitments[index];
      const auto& block = offered.blocks[index];
      require_binding(source.direction == direction::received && source.ranges.size() == 1 &&
                      source.ranges.front().begin == next && block.size <= body.end - next &&
                      source.ranges.front().end == next + block.size && source.digest == block.source);
      const auto expected_size = ((static_cast<std::size_t>(block.size) + 16U + 31U) / 32U) * 32U;
      require_binding(ciphertext[index].size() == expected_size &&
                      forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{ciphertext[index]}) ==
                          block.ciphertext);
      next += block.size;
   }
   require_binding(next == body.end);
   const auto bytes = forge::raw::pack(evidence);
   require_binding(forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{bytes}) == offered.evidence);
}

} // namespace forge::net::tlsnotary
