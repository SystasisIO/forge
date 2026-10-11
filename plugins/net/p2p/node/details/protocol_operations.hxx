#pragma once

#include <functional>
#include <memory>

namespace forge::plugins::net::p2p::node::detail {

struct protocol_operations {
   std::function<void(forge::net::p2p::protocol_id, forge::net::p2p::node::protocol_handler)> install;
   std::function<bool(const forge::net::p2p::protocol_id&)> remove;
};

[[nodiscard]] std::shared_ptr<const protocol_operations>
make_protocol_operations(std::shared_ptr<forge::net::p2p::node> node);

}
