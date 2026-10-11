module;

#include <functional>
#include <memory>

export module forge.plugins.net.p2p.node.protocol_registration;

export namespace forge::plugins::net::p2p::node {

namespace detail { struct protocol_registration_access; struct protocol_registration_state; }

class protocol_registration {
 public:
   protocol_registration();
   ~protocol_registration() noexcept;
   protocol_registration(const protocol_registration&) = delete;
   protocol_registration& operator=(const protocol_registration&) = delete;
   protocol_registration(protocol_registration&&) noexcept;
   protocol_registration& operator=(protocol_registration&&) noexcept;

   [[nodiscard]] bool active() const noexcept;
   // Closes admission, not already admitted streams. Repeated close retries an
   // uncertain native unregister; it never releases another generation's name.
   void close() noexcept;

 private:
   friend struct detail::protocol_registration_access;
   std::shared_ptr<detail::protocol_registration_state> state_;
};

namespace detail {
struct protocol_registration_access {
   [[nodiscard]] static protocol_registration make(std::weak_ptr<void> owner, std::weak_ptr<void> generation,
       std::function<void()> close, std::function<bool()> active);
   [[nodiscard]] static bool matches(const protocol_registration& token, const std::shared_ptr<void>& owner,
                                     const std::shared_ptr<void>& generation) noexcept;
};
}
}
