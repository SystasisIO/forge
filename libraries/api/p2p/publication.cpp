module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <functional>
#include <memory>
#include <stop_token>
#include <utility>

module forge.api.p2p.publication;

import forge.api.core.exceptions;

namespace forge::api::p2p {

publication::publication() = default;

publication::publication(std::shared_ptr<detail::publication_state> state)
    : state_{std::move(state)} {}

publication::~publication() noexcept {
   close();
}

publication::publication(publication&& other) noexcept = default;

publication& publication::operator=(publication&& other) noexcept {
   if (this == &other) {
      return *this;
   }

   auto& result = *this;
   auto replaced = std::exchange(state_, std::move(other.state_));
   detail::close_publication(std::move(replaced));
   return result;
}

bool publication::active() const noexcept {
   const auto state = state_;
   return detail::publication_active(state);
}

std::stop_token publication::stop_token() const noexcept {
   return detail::publication_stop_token(state_);
}

void publication::close() noexcept {
   const auto state = state_;
   detail::close_publication(state);
}

boost::asio::awaitable<void> publication::async_close() {
   return async_close_impl(state_);
}

boost::asio::awaitable<void>
publication::async_close_impl(std::shared_ptr<detail::publication_state> state) {
   return detail::async_close_publication(std::move(state));
}

publication detail::publication_access::make(
   boost::asio::any_io_executor owner_executor,
   std::function<void()> close,
   std::function<boost::asio::awaitable<void>()> drain,
   std::function<bool()> active, std::stop_token native) {
   if (!owner_executor) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                            "P2P publication owner executor is unavailable");
   }
   return publication{make_publication_state(
      std::move(owner_executor), std::move(close), std::move(drain), std::move(active), native)};
}

} // namespace forge::api::p2p
