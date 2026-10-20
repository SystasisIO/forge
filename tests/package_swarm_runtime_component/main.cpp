#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <memory>
#include <stop_token>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>
#include <type_traits>

import forge.swarm.runtime.engine;
import forge.asio.blocking;
import forge.asio.runtime;

class installed_route final : public forge::swarm::runtime::capabilities::route {
 public:
   std::string_view key() const noexcept override { return "installed-route"; }
   boost::asio::awaitable<std::unique_ptr<forge::swarm::runtime::capabilities::connection>> async_connect(
      std::stop_token, std::chrono::steady_clock::time_point) const override { return connect_owned(); }
 private:
   static boost::asio::awaitable<std::unique_ptr<forge::swarm::runtime::capabilities::connection>> connect_owned() {
      co_return nullptr;
   }
};

int main() {
   namespace swarm = forge::swarm::runtime;
   static_assert(!std::is_copy_constructible_v<swarm::sharing>);
   static_assert(!std::is_copy_constructible_v<swarm::download>);
   static_assert(!std::is_abstract_v<swarm::engine>);
   auto route = std::make_unique<installed_route>();
   auto pending = route->async_connect({}, std::chrono::steady_clock::now());
   route.reset();
   auto grant = swarm::quota{{.bytes = 32U}};
   {
      auto held = grant.reserve(16U);
      if (grant.inspect().available != 16U)
         return 1;
   }
   if (grant.inspect().available != 32U || grant.inspect().held != 0U)
      return 2;
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto cpu = forge::asio::compute::pool{{.worker_threads = 1}};
   auto engine = swarm::engine{scheduler, cpu.get_executor(), std::make_shared<swarm::free_admission>()};
   auto close = [&]() -> boost::asio::awaitable<void> {
      if (co_await std::move(pending)) throw std::logic_error{"unexpected installed route connection"};
      co_await engine.async_close();
      co_await scheduler.shutdown();
   };
   forge::asio::blocking::run(runtime, close());
   return 0;
}
