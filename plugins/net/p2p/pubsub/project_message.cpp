module;

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

module forge.plugins.net.p2p.pubsub.plugin;

import forge.net.p2p.identity;
import forge.net.p2p.pubsub;
import forge.plugins.net.p2p.pubsub.types;

#include "details/project_message.hxx"

namespace forge::plugins::net::p2p::pubsub {

message project_message(const forge::net::p2p::peer_id& source, const forge::net::p2p::pubsub::message& value) {
   return message{
       .source = source,
       .author = value.signature.empty() ? std::nullopt : value.from,
       .subject = value.subject,
       .data = value.data,
       .seqno = value.seqno,
   };
}

} // namespace forge::plugins::net::p2p::pubsub
