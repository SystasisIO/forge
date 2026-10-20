#include "fixture.hxx"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <iostream>
#include <thread>

import forge.app.application_builder;
import forge.app.application_shell;
import forge.config.core.document;
import forge.config.core.value;
import forge.plugins.net.tlsnotary.session.plugin;
import forge.plugins.net.tlsnotary.session.api;
import forge.plugins.net.tlsnotary.session.exceptions;

using namespace tlsn_test;
namespace plugin = forge::plugins::net::tlsnotary::session;
int main(int argc, char** argv) {
   try {
      const auto worker = std::string{argc > 1 ? argv[1] : FORGE_TLSNOTARY_WORKER_PATH};
      auto builder = forge::app::application_builder{};
      builder.name("tlsnotary-session-test")
          .runtime({.worker_threads = 4})
          .compute({.worker_threads = 2, .max_pending_tasks = 8})
          .plugin(plugin::plugin::descriptor());
      auto shell = std::move(builder).build();
      auto document = forge::config::core::document{};
      document.set("plugins.net.tlsnotary.session.worker-path", forge::config::core::value{worker});
      document.set("plugins.net.tlsnotary.session.max-active", forge::config::core::value{std::uint64_t{1}});
      document.set("plugins.net.tlsnotary.session.max-queued", forge::config::core::value{std::uint64_t{0}});
      shell->configure(document);
      forge::asio::blocking::run(shell->runtime(), shell->startup());
      auto api = shell->apis().get<plugin::api>(plugin::api::ref()).shared();
      check(!api->snapshot().stopped, "plugin did not start");
      auto identity = make_identity();
      auto streams = pair(shell->runtime());
      auto pending_request =
          tlsn::verifier_request{.peer = std::move(streams.first),
                                 .source = {.server_name = "tlsn.local", .trust_roots_der = {identity.root}},
                                 .options = options(17)};
      auto running =
          asio::co_spawn(shell->runtime().context(), api->verify(std::move(pending_request)), asio::use_future);
      auto until = std::chrono::steady_clock::now() + 3s;
      while (api->snapshot().active != 1 && std::chrono::steady_clock::now() < until)
         std::this_thread::sleep_for(1ms);
      check(api->snapshot().active == 1, "plugin session not admitted");
      auto more = pair(shell->runtime());
      auto extra = tlsn::verifier_request{.peer = std::move(more.first),
                                          .source = {.server_name = "tlsn.local", .trust_roots_der = {identity.root}},
                                          .options = options(18)};
      auto rejected = asio::co_spawn(shell->runtime().context(), api->verify(std::move(extra)), asio::use_future);
      auto overload = false;
      try {
         static_cast<void>(rejected.get());
      } catch (const tlsn::exceptions::overloaded&) {
         overload = true;
      }

      check(overload, "plugin did not enforce active quota");
      check(api->cancel(17), "plugin cancellation missed session");
      auto cancelled = false;
      try {
         static_cast<void>(running.get());
      } catch (const tlsn::exceptions::cancelled&) {
         cancelled = true;
      }

      check(cancelled, "plugin session cancellation not typed");
      auto direct = tlsn::engine{shell->runtime(), shell->compute(), {.worker_path = worker, .max_active = 1}};
      for (auto plugin_prover : {false, true}) {
         auto https = https_fixture{shell->runtime(), identity};
         auto service = asio::co_spawn(shell->runtime().context(), https.serve(), asio::use_future);
         auto peers = pair(shell->runtime());
         auto proof = tlsn::prover_request{.peer = std::move(peers.first),
                                           .server = https.connect(shell->runtime()),
                                           .source = {.server_name = "tlsn.local", .trust_roots_der = {identity.root}},
                                           .request_bytes = request(),
                                           .options = options(51)};
         auto verification =
             tlsn::verifier_request{.peer = std::move(peers.second),
                                    .source = {.server_name = "tlsn.local", .trust_roots_der = {identity.root}},
                                    .options = options(52)};
         auto verified = asio::co_spawn(shell->runtime().context(),
                                        plugin_prover ? direct.verify(std::move(verification))
                                                      : api->verify(std::move(verification)),
                                        asio::use_future);
         auto proved = asio::co_spawn(shell->runtime().context(),
                                      plugin_prover ? api->prove(std::move(proof)) : direct.prove(std::move(proof)),
                                      asio::use_future);
         const auto authenticated = verified.get();
         const auto metadata = proved.get();
         check(authenticated.sent.size() == 1 && authenticated.received.size() == 1 && metadata.sent.empty(),
               "plugin MPC authenticated ranges missing");
         const auto body =
             std::string{authenticated.received.front().bytes.begin(), authenticated.received.front().bytes.end()};
         check(body.find("7318921") != std::string::npos && body.find("secret-sentinel") == std::string::npos,
               "plugin MPC authenticated response mismatch");
         static_cast<void>(service.get());
      }

      forge::asio::blocking::run(shell->runtime(), direct.shutdown());
      shell->request_stop();
      forge::asio::blocking::run(shell->runtime(), shell->shutdown());
      auto stopped = false;
      try {
         static_cast<void>(api->snapshot());
      } catch (const plugin::exceptions::not_ready&) {
         stopped = true;
      }

      check(stopped, "retained API admitted after stop");
      std::cout << "TLSNotary App plugin startup, overload, cancel and shutdown PASS\n";
      return 0;
   } catch (const std::exception& error) {
      std::cerr << "TLSNotary plugin test failed: " << error.what() << '\n';
      return 1;
   }
}
