module;

#include <functional>
#include <utility>

module forge.plugins.net.p2p.node.peer_contribution;

namespace forge::plugins::net::p2p::node {

peer_contribution::peer_contribution() = default;
peer_contribution::~peer_contribution() noexcept { close(); }
peer_contribution::peer_contribution(peer_contribution&& other) noexcept
    : close_{std::exchange(other.close_, {})}, active_{std::exchange(other.active_, {})} {}

peer_contribution& peer_contribution::operator=(peer_contribution&& other) noexcept {
   if (this != &other) {
      auto previous = peer_contribution{std::move(*this)};
      close_ = std::exchange(other.close_, {});
      active_ = std::exchange(other.active_, {});
   }
   return *this;
}

bool peer_contribution::active() const noexcept { return active_ && active_(); }

void peer_contribution::close() noexcept {
   auto close = std::exchange(close_, {});
   auto active = std::exchange(active_, {});
   if (close) { close(); }
}

peer_contribution detail::peer_contribution_access::make(std::function<void()> close,
                                                        std::function<bool()> active) {
   auto result = peer_contribution{};
   result.close_ = std::move(close);
   result.active_ = std::move(active);
   return result;
}

} // namespace forge::plugins::net::p2p::node
