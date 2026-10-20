module;

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <memory>

export module forge.net.p2p.provider_registration;

import forge.net.p2p.dht;
import forge.net.p2p.protocol;

export namespace forge::net::p2p {

class provider_registration;

namespace detail {
struct provider_registration_access;
}

class provider_registration {
 public:
   provider_registration() = default;
   ~provider_registration();

   provider_registration(const provider_registration&) = delete;
   provider_registration& operator=(const provider_registration&) = delete;

   provider_registration(provider_registration&&) noexcept;
   provider_registration& operator=(provider_registration&&) noexcept;

   [[nodiscard]] bool active() const noexcept;
   [[nodiscard]] const protocol_id& profile() const;
   [[nodiscard]] const dht::key& key() const;
   // Stop renewal without dropping the async_withdraw() ownership fence.
   void request_stop() noexcept;
   boost::asio::awaitable<void> async_withdraw();

 private:
   struct impl;
   using active_callback = std::function<bool()>;
   using request_stop_callback = std::function<void()>;
   using withdraw_callback = std::function<boost::asio::awaitable<void>()>;

   provider_registration(protocol_id profile, dht::key key, active_callback active, request_stop_callback request_stop,
                         withdraw_callback withdraw);

   friend struct detail::provider_registration_access;

   std::shared_ptr<impl> impl_;
};

namespace detail {

struct provider_registration_access {
   [[nodiscard]] static provider_registration make(protocol_id profile, dht::key key,
                                                   provider_registration::active_callback active,
                                                   provider_registration::request_stop_callback request_stop,
                                                   provider_registration::withdraw_callback withdraw);
};

} // namespace detail

} // namespace forge::net::p2p
