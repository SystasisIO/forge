#include <type_traits>

import forge.swarm.api.peer;

int main() {
   namespace api = forge::api::core;
   using peer = forge::swarm::api::peer;
   static_assert(!std::is_abstract_v<api::proxy<peer>>);
   const auto descriptor = peer::describe();
   if (descriptor.methods.size() != 2U)
      return 1;
   if (descriptor.methods[0].kind != api::method_kind::server_stream ||
       descriptor.methods[1].kind != api::method_kind::bidirectional_stream)
      return 2;
   if (!descriptor.methods[0].output_decoder || !descriptor.methods[1].input_decoder ||
       !descriptor.methods[1].output_decoder)
      return 3;
   namespace wire = forge::swarm::protocol;
   descriptor.methods[1].input_decoder(wire::encode_message(wire::peer_message{wire::choke{true}}), {});
   return 0;
}
