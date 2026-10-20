#include <boost/asio/awaitable.hpp>

#include <concepts>
#include <cstdint>
#include <filesystem>
#include <utility>

import forge.asio.blocking;
import forge.asio.compute;
import forge.asio.runtime;
import forge.net.tlsnotary.engine;
#if FORGE_TLSNOTARY_TEST_PLUGIN
import forge.plugins.net.tlsnotary.session.api;
import forge.plugins.net.tlsnotary.session.plugin;
#endif

static_assert(std::same_as<decltype(std::declval<forge::net::tlsnotary::engine&>().verify(
                               std::declval<forge::net::tlsnotary::verifier_request>())),
                           boost::asio::awaitable<forge::net::tlsnotary::result>>);

int main(int argc, char** argv) {
   if (argc != 2 || !std::filesystem::is_regular_file(argv[1]))
      return 1;

#if FORGE_TLSNOTARY_TEST_PLUGIN
   const auto descriptor = forge::plugins::net::tlsnotary::session::plugin::descriptor();
   if (descriptor.id.value != "forge.plugins.net.tlsnotary.session")
      return 2;
#endif

   auto runtime = forge::asio::runtime{};
   auto compute = forge::asio::compute::pool{{.worker_threads = 1, .max_pending_tasks = 4}};
   auto options = forge::net::tlsnotary::engine_options{};
   options.worker_path = std::filesystem::absolute(argv[1]);
   options.max_active = 1;
   options.max_queued = 1;
   auto engine = forge::net::tlsnotary::engine{runtime, compute.get_executor(), std::move(options)};
   forge::asio::blocking::run(runtime, engine.shutdown());
   const auto stopped = engine.snapshot();
   forge::asio::blocking::run(runtime, compute.shutdown());
   return stopped.stopped && stopped.active == 0 && stopped.queued == 0 ? 0 : 3;
}
