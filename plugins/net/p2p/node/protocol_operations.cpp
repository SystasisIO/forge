module;

#include <functional>
#include <memory>
#include <utility>

module forge.plugins.net.p2p.node.plugin;

import forge.net.p2p.node;
import forge.net.p2p.protocol;

#include "details/protocol_operations.hxx"

namespace forge::plugins::net::p2p::node::detail {

std::shared_ptr<const protocol_operations> make_protocol_operations(std::shared_ptr<forge::net::p2p::node> node) {
   return std::make_shared<const protocol_operations>(protocol_operations{
       .install = [node](auto protocol, auto handler) {
          node->register_protocol_handler(std::move(protocol), std::move(handler));
       },
       .remove = [node = std::move(node)](const auto& protocol) {
          return node->unregister_protocol_handler(protocol);
       },
   });
}

}
