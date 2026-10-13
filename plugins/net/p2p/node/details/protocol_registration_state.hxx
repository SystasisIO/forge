#pragma once

#include <functional>
#include <memory>

namespace forge::plugins::net::p2p::node::detail {
struct protocol_registration_state {
   std::weak_ptr<void> owner;
   std::weak_ptr<void> generation;
   std::function<void()> close;
   std::function<bool()> active;
};
}
