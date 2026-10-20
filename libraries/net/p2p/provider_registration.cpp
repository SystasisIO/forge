module;

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include <forge/exceptions/macros.hpp>

module forge.net.p2p.provider_registration;

import forge.asio.gate;
import forge.net.p2p.exceptions;

#include "details/provider_registration_impl.hxx"

namespace forge::net::p2p {

provider_registration::provider_registration(protocol_id profile, dht::key key, active_callback active,
                                             request_stop_callback request_stop, withdraw_callback withdraw)
    : impl_(std::make_shared<impl>(std::move(profile), std::move(key), std::move(active), std::move(request_stop),
                                   std::move(withdraw))) {}

provider_registration::~provider_registration() {
   if (impl_) {
      impl_->request_stop();
   }
}

provider_registration::provider_registration(provider_registration&&) noexcept = default;
provider_registration& provider_registration::operator=(provider_registration&& other) noexcept {
   if (this != &other) {
      if (impl_) {
         impl_->request_stop();
      }
      impl_ = std::move(other.impl_);
   }
   return *this;
}

bool provider_registration::active() const noexcept {
   return impl_ && impl_->active();
}

void provider_registration::request_stop() noexcept {
   if (impl_) { impl_->request_stop(); }
}

const protocol_id& provider_registration::profile() const {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "P2P provider registration is empty");
   }
   return impl_->profile;
}

const dht::key& provider_registration::key() const {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "P2P provider registration is empty");
   }
   return impl_->key;
}

boost::asio::awaitable<void> provider_registration::async_withdraw() {
   if (!impl_) {
      return []() -> boost::asio::awaitable<void> { co_return; }();
   }
   return impl::async_withdraw_owned(impl_);
}

provider_registration detail::provider_registration_access::make(
    protocol_id profile, dht::key key, provider_registration::active_callback active,
    provider_registration::request_stop_callback request_stop, provider_registration::withdraw_callback withdraw) {
   return provider_registration{std::move(profile), std::move(key), std::move(active), std::move(request_stop),
                                std::move(withdraw)};
}

} // namespace forge::net::p2p
