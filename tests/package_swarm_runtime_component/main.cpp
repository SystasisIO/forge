#include <boost/asio/awaitable.hpp>
#include <memory>
#include <type_traits>

import forge.swarm.runtime.engine;
import forge.asio.blocking;
import forge.asio.runtime;

int main() {
   namespace swarm = forge::swarm::runtime;
   static_assert(!std::is_copy_constructible_v<swarm::sharing>);
   static_assert(!std::is_copy_constructible_v<swarm::download>);
   static_assert(!std::is_abstract_v<swarm::engine>);
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
      co_await engine.async_close();
      co_await scheduler.shutdown();
   };
   forge::asio::blocking::run(runtime, close());
   return 0;
}
