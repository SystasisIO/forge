#include "evidence.hxx"

#include <concepts>
#include <cstdint>
#include <filesystem>
#include <utility>

import forge.asio.blocking;
import forge.asio.compute;
import forge.asio.runtime;
import forge.net.tlsnotary.engine;
#if FORGE_TLSNOTARY_TEST_PLUGIN
import forge.app.application_builder;
import forge.app.application_shell;
import forge.config.core.document;
import forge.config.core.value;
import forge.plugins.net.tlsnotary.session.api;
import forge.plugins.net.tlsnotary.session.plugin;
#endif

static_assert(std::same_as<decltype(std::declval<forge::net::tlsnotary::engine&>().verify(
                               std::declval<forge::net::tlsnotary::verifier_request>())),
                           boost::asio::awaitable<forge::net::tlsnotary::result>>);
static_assert(std::same_as<decltype(std::declval<forge::net::tlsnotary::engine&>().check(
                               std::declval<forge::net::tlsnotary::check_request>())),
                           boost::asio::awaitable<void>>);
#if FORGE_TLSNOTARY_TEST_PLUGIN
static_assert(std::same_as<decltype(std::declval<forge::plugins::net::tlsnotary::session::api&>().check(
                               std::declval<forge::net::tlsnotary::check_request>())),
                           boost::asio::awaitable<void>>);
#endif

int main(int argc, char** argv) {
   try {
      if (argc == 4 && std::string_view{argv[1]} == "--retained") {
         tlsn_test::retained_evidence_test(argv[2], argv[3]);
         return 0;
      }
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
      if (!stopped.stopped || stopped.active != 0 || stopped.queued != 0)
         return 3;
      auto name = (std::filesystem::current_path() / "evidence-installed-XXXXXX").string();
      tlsn_test::check(::mkdtemp(name.data()) != nullptr, "installed evidence output creation failed");
      const auto output = std::filesystem::path{name};
      const auto worker = std::filesystem::absolute(argv[1]);
      tlsn_test::evidence_test(worker, output);
      tlsn_test::restart_evidence_test(argv[0], worker, output);
#if FORGE_TLSNOTARY_TEST_PLUGIN
      auto builder = forge::app::application_builder{};
      builder.name("installed-tlsnotary-evidence")
          .runtime({.worker_threads = 4})
          .compute({.worker_threads = 2, .max_pending_tasks = 4})
          .plugin(forge::plugins::net::tlsnotary::session::plugin::descriptor());
      auto shell = std::move(builder).build();
      auto document = forge::config::core::document{};
      document.set("plugins.net.tlsnotary.session.worker-path", forge::config::core::value{worker.string()});
      shell->configure(document);
      forge::asio::blocking::run(shell->runtime(), shell->startup());
      const auto api = shell->apis().get<forge::plugins::net::tlsnotary::session::api>(
          forge::plugins::net::tlsnotary::session::api::ref()).shared();
      const auto retained = tlsn_test::read_record<tlsn_test::signed_evidence>(output / "evidence.raw");
      const auto trust = tlsn_test::read_record<tlsn_test::evidence_trust>(output / "trust.raw");
      tlsn_test::check(tlsn_test::authorize(retained, trust), "installed plugin evidence authorization rejected");
      const auto openings = tlsn_test::read_record<std::vector<forge::net::tlsnotary::opening>>(
          output / "openings.private.raw");
      auto check_request = forge::net::tlsnotary::check_request{
          .authenticated = retained.statement.authenticated, .opening = openings.back()};
      check_request.options.id = 501;
      forge::asio::blocking::run(shell->runtime(), api->check(std::move(check_request)));
      shell->request_stop();
      forge::asio::blocking::run(shell->runtime(), shell->shutdown());
#endif
      std::cout << "Installed TLSNotary MPC, canonical evidence, authorized openings and restart PASS\n";
      return 0;
   } catch (const std::exception& error) {
      std::cerr << "Installed TLSNotary consumer failed: " << error.what() << '\n';
      return 1;
   }
}
