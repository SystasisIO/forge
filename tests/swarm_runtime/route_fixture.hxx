#pragma once

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

import forge.swarm.runtime.capabilities;

namespace forge::tests::swarm {

class route final : public forge::swarm::runtime::capabilities::route {
 public:
   using connection = forge::swarm::runtime::capabilities::connection;
   using factory = std::function<boost::asio::awaitable<std::unique_ptr<connection>>(
      std::stop_token, std::chrono::steady_clock::time_point)>;
   route(std::string key, factory connect) : _state{std::make_shared<const state>(std::move(key), std::move(connect))} {}
   std::string_view key() const noexcept override { return _state->key; }
   boost::asio::awaitable<std::unique_ptr<connection>> async_connect(
      std::stop_token stop, std::chrono::steady_clock::time_point deadline) const override {
      return connect_owned(_state, stop, deadline);
   }
 private:
   struct state {
      std::string key;
      factory connect;
   };
   static boost::asio::awaitable<std::unique_ptr<connection>> connect_owned(
      std::shared_ptr<const state> context, std::stop_token stop, std::chrono::steady_clock::time_point deadline) {
      co_return co_await context->connect(stop, deadline);
   }
   std::shared_ptr<const state> _state;
};

} // namespace forge::tests::swarm
