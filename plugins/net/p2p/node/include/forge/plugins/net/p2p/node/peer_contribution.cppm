module;

#include <functional>

export module forge.plugins.net.p2p.node.peer_contribution;

export namespace forge::plugins::net::p2p::node {

namespace detail { struct peer_contribution_access; }

// One exact local protection or score contribution, not ownership of the peer.
class peer_contribution {
 public:
   peer_contribution();
   ~peer_contribution() noexcept;
   peer_contribution(const peer_contribution&) = delete;
   peer_contribution& operator=(const peer_contribution&) = delete;
   peer_contribution(peer_contribution&&) noexcept;
   peer_contribution& operator=(peer_contribution&&) noexcept;

   [[nodiscard]] bool active() const noexcept;
   void close() noexcept;

 private:
   friend struct detail::peer_contribution_access;
   std::function<void()> close_;
   std::function<bool()> active_;
};

namespace detail {
// Module boundary only; the private registry owns native keys and mutations.
struct peer_contribution_access {
   [[nodiscard]] static peer_contribution make(std::function<void()> close, std::function<bool()> active);
};
}

} // namespace forge::plugins::net::p2p::node
