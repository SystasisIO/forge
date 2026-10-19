#include <array>
#include <cstdint>
#include <chrono>

import forge.swarm.core.descriptor;
import forge.swarm.core.identity;
import forge.swarm.core.merkle;
import forge.swarm.core.transfer;
import forge.swarm.core.choker;

int main() {
   namespace core = forge::swarm::core;
   const auto data = std::array<std::uint8_t, 1>{'a'};
   const auto hashes = core::hash_file(data, core::block_size);
   const auto value = core::descriptor{core::descriptor_version::v1, core::block_size, {{{"a"}, 1U, hashes.root, {}}}};
   const auto wire = core::encode_descriptor(value);
   const auto restored = core::decode_descriptor(wire);
   if (restored != value ||
       core::calculate_swarm_id(wire).value.str() != "fce83635f30c1c132dc75136561c023f62db154a65bfc68e655668edd64069ab")
      return 1;
   auto transfer = core::transfer{value, {}, 42};
   const auto peer = transfer.add_peer(std::array<std::uint8_t, 1>{1});
   const auto now = core::transfer_time{std::chrono::milliseconds{1}};
   static_cast<void>(transfer.set_choked(peer, false, now));
   const auto requests = transfer.pick(peer, now, 1);
   if (requests.size() != 1 || requests.front().length != 1)
      return 2;
   const auto received = transfer.receive(peer, requests.front().id, 1, now);
   if (!received.accepted || !received.ready || transfer.verified({0, 0}))
      return 3;
   if (transfer.finish(*received.ready, true) != core::transfer::verification_result::verified ||
       !transfer.inspect().complete)
      return 4;
   auto serving = core::choker{{}, 42};
   const auto candidates = std::array{core::choker::candidate{peer, true, transfer.observations(peer), 0}};
   const auto plan = serving.select(candidates, now, true);
   return plan.unchoked.size() == 1 && plan.unchoked.front() == peer ? 0 : 5;
}
