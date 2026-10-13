module;

#include <functional>
#include <memory>
#include <utility>

module forge.plugins.net.p2p.node.protocol_registration;

#include "details/protocol_registration_state.hxx"

namespace forge::plugins::net::p2p::node {

protocol_registration::protocol_registration() = default;
protocol_registration::~protocol_registration() noexcept { close(); }
protocol_registration::protocol_registration(protocol_registration&& other) noexcept
    : state_{std::move(other.state_)} {}

protocol_registration& protocol_registration::operator=(protocol_registration&& other) noexcept {
   if (this != &other) {
      auto previous = protocol_registration{std::move(*this)};
      state_ = std::move(other.state_);
      // The new owner is visible before a reentrant old callback is destroyed.
   }
   return *this;
}

bool protocol_registration::active() const noexcept {
   const auto state = state_;
   return state && state->active();
}
void protocol_registration::close() noexcept {
   const auto state = state_;
   if (state) { state->close(); }
}

protocol_registration detail::protocol_registration_access::make(
    std::weak_ptr<void> owner, std::weak_ptr<void> generation, std::function<void()> close,
    std::function<bool()> active) {
   auto token = protocol_registration{};
   token.state_ = std::make_shared<protocol_registration_state>(protocol_registration_state{
       .owner = std::move(owner), .generation = std::move(generation),
       .close = std::move(close), .active = std::move(active)});
   return token;
}

bool detail::protocol_registration_access::matches(const protocol_registration& token,
    const std::shared_ptr<void>& owner, const std::shared_ptr<void>& generation) noexcept {
   const auto state = token.state_;
   return state && !state->owner.owner_before(owner) && !owner.owner_before(state->owner) &&
          !state->generation.owner_before(generation) && !generation.owner_before(state->generation);
}
}
