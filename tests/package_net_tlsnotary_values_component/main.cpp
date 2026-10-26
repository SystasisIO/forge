#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

import forge.net.tlsnotary.types;
import forge.raw.codec;

int main()
{
   auto verified = forge::net::tlsnotary::result{};
   verified.server_name = "source.test";
   verified.sent_size = 64;
   verified.received_size = 96;
   verified.sent = {{0, {71, 69, 84}}, {40, {13, 10}}};
   verified.received = {{0, {72, 84, 84, 80}}};
   verified.retained = forge::net::tlsnotary::evidence{};
   verified.retained->server_name = verified.server_name;
   verified.retained->sent_size = verified.sent_size;
   verified.retained->received_size = verified.received_size;
   verified.retained->commitments.push_back({forge::net::tlsnotary::direction::received, {{32, 96}}, {}});

   const auto encoded = forge::raw::pack(verified);
   const auto restored = forge::raw::unpack_bounded_exact<forge::net::tlsnotary::result>(encoded);
   if (restored.server_name != verified.server_name || restored.sent != verified.sent ||
       restored.received != verified.received || restored.retained != verified.retained ||
       !restored.openings.empty() || forge::raw::pack(restored) != encoded ||
       encoded != forge::raw::pack(std::tie(verified.server_name, verified.sent_size, verified.received_size,
                                           verified.sent, verified.received, verified.retained, verified.openings)))
   {
      return 1;
   }

   auto prover = forge::net::tlsnotary::result{};
   prover.server_name = verified.server_name;
   prover.retained = verified.retained;
   prover.openings.push_back({0, {0x51, 0x52, 0x53}, {1, 2, 3}});
   const auto sealed_source = forge::raw::pack(prover);
   const auto private_restored = forge::raw::unpack_bounded_exact<forge::net::tlsnotary::result>(sealed_source);
   if (!private_restored.sent.empty() || !private_restored.received.empty() ||
       private_restored.openings != prover.openings || forge::raw::pack(private_restored) != sealed_source)
   {
      return 2;
   }
   return 0;
}
