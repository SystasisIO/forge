#include "evidence.hxx"

#include <thread>

namespace {
using namespace tlsn_test;

tlsn::check_request checker(std::uint64_t id) {
   auto request = tlsn::check_request{
       .authenticated = {.server_name = "tlsn.local", .sent_size = 8, .received_size = 8,
                          .commitments = {{.direction = tlsn::direction::received, .ranges = {{0, 1}}}}},
       .opening = {.plaintext = {'x'}}};
   request.options.id = id;
   request.options.timeout = 10s;
   return request;
}

template <typename Work> void rejects_code(tlsn::exceptions::code expected, Work work) {
   auto rejected = false;
   try {
      work();
   } catch (const forge::exceptions::context_error& error) {
      rejected = std::string_view{error.code().category().name()} == "forge.net.tlsnotary" &&
                 error.code().value() == static_cast<int>(expected);
   }
   check(rejected, "opening checker returned a different failure category");
}

void checker_admission_tests(const std::filesystem::path& output) {
   auto runtime = forge::asio::runtime{{.worker_threads = 4}};
   auto compute = forge::asio::compute::pool{{.worker_threads = 2, .max_pending_tasks = 4}};
   const auto fault = output / "checker-faults";
   std::filesystem::create_directory(fault);
   for (const auto* name : {"active", "pre-cancel", "deadline", "lazy", "stopped", "stream"})
      std::filesystem::create_directory(fault / name);
   for (const auto* name : {"active", "pre-cancel", "deadline", "lazy", "stopped"})
      std::filesystem::create_symlink(FORGE_TLSNOTARY_FAULT_WORKER, fault / name / "hang");
   std::filesystem::create_symlink(FORGE_TLSNOTARY_FAULT_WORKER, fault / "stream" / "read");
   {
      auto owner = tlsn::engine{runtime, compute.get_executor(),
                                {.worker_path = fault / "active" / "hang", .max_active = 1, .max_queued = 0}};
      const auto cleanup = engine_cleanup{runtime, owner};
      auto running = asio::co_spawn(runtime.context(), owner.check(checker(601)), asio::use_future);
      const auto until = std::chrono::steady_clock::now() + 3s;
      while (!std::filesystem::exists(fault / "active" / "hang.started") && std::chrono::steady_clock::now() < until)
         std::this_thread::sleep_for(1ms);
      check(std::filesystem::exists(fault / "active" / "hang.started"), "opening checker worker did not start");
      rejects_code(tlsn::exceptions::code::invalid_request,
                   [&] { forge::asio::blocking::run(runtime, owner.check(checker(601))); });
      rejects_code(tlsn::exceptions::code::overloaded,
                   [&] { forge::asio::blocking::run(runtime, owner.check(checker(602))); });
      check(owner.snapshot().accepted == 1, "duplicate or overload entered opening checker admission");
      check(owner.cancel(601), "opening checker cancellation missed active owner");
      rejects_code(tlsn::exceptions::code::cancelled, [&] { running.get(); });
      forge::asio::blocking::run(runtime, owner.shutdown());
   }
   {
      auto owner = tlsn::engine{runtime, compute.get_executor(), {.worker_path = fault / "pre-cancel" / "hang"}};
      const auto cleanup = engine_cleanup{runtime, owner};
      auto cancelled = checker(603);
      auto stop = std::stop_source{};
      stop.request_stop();
      cancelled.options.stop = stop.get_token();
      rejects_code(tlsn::exceptions::code::cancelled,
                   [&] { forge::asio::blocking::run(runtime, owner.check(std::move(cancelled))); });
      check(!std::filesystem::exists(fault / "pre-cancel" / "hang.started"), "pre-cancelled checker spawned a worker");
      forge::asio::blocking::run(runtime, owner.shutdown());
   }
   {
      auto owner = tlsn::engine{runtime, compute.get_executor(), {.worker_path = fault / "deadline" / "hang"}};
      const auto cleanup = engine_cleanup{runtime, owner};
      auto expired = checker(607);
      expired.options.timeout = 500ms;
      rejects_code(tlsn::exceptions::code::deadline_exceeded,
                   [&] { forge::asio::blocking::run(runtime, owner.check(std::move(expired))); });
      check(std::filesystem::exists(fault / "deadline" / "hang.started"), "checker deadline did not exercise a live worker");
      check(owner.snapshot().active == 0 && owner.snapshot().queued == 0, "expired checker retained admission");
      forge::asio::blocking::run(runtime, owner.shutdown());
   }
   {
      auto owner = tlsn::engine{runtime, compute.get_executor(), {.worker_path = fault / "lazy" / "hang"}};
      auto lazy = owner.check(checker(604));
      check(owner.snapshot().accepted == 0, "unpolled opening checker entered admission");
      forge::asio::blocking::run(runtime, owner.shutdown());
      rejects_code(tlsn::exceptions::code::stopped,
                   [&] { forge::asio::blocking::run(runtime, std::move(lazy)); });
      check(!std::filesystem::exists(fault / "lazy" / "hang.started"), "lazy checker spawned after shutdown");
   }
   {
      auto owner = tlsn::engine{runtime, compute.get_executor(), {.worker_path = fault / "stopped" / "hang"}};
      owner.request_stop();
      rejects_code(tlsn::exceptions::code::stopped,
                   [&] { static_cast<void>(owner.check(checker(605))); });
      check(!std::filesystem::exists(fault / "stopped" / "hang.started"), "stopped checker spawned a worker");
      forge::asio::blocking::run(runtime, owner.shutdown());
   }
   {
      auto owner = tlsn::engine{runtime, compute.get_executor(), {.worker_path = fault / "stream" / "read"}};
      const auto cleanup = engine_cleanup{runtime, owner};
      rejects_code(tlsn::exceptions::code::protocol_error,
                   [&] { forge::asio::blocking::run(runtime, owner.check(checker(606))); });
      check(owner.snapshot().active == 0 && owner.snapshot().queued == 0,
            "opening checker transport refusal retained resources");
      forge::asio::blocking::run(runtime, owner.shutdown());
   }
   forge::asio::blocking::run(runtime, compute.shutdown());
   auto status = int{};
   check(::waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, "opening checker retained a child process");
}
} // namespace

int main(int argc, char** argv) {
   try {
      if (argc == 4 && std::string_view{argv[1]} == "--retained") {
         tlsn_test::retained_evidence_test(argv[2], argv[3]);
         return 0;
      }
      const auto worker = std::filesystem::path{argc > 1 ? argv[1] : FORGE_TLSNOTARY_WORKER_PATH};
      auto name = std::string{FORGE_TLSNOTARY_TEST_OUTPUT "/evidence-XXXXXX"};
      tlsn_test::check(::mkdtemp(name.data()) != nullptr, "evidence test output creation failed");
      const auto output = std::filesystem::path{name};
      checker_admission_tests(output);
      tlsn_test::evidence_test(worker, output);
      tlsn_test::restart_evidence_test(argv[0], worker, output);
      tlsn_test::commitment_policy_test(worker);
      std::cout << "TLSNotary authenticated commitments, authorized openings and retained evidence PASS\n";
      return 0;
   } catch (const std::exception& error) {
      std::cerr << "TLSNotary evidence test failed: " << error.what() << '\n';
      return 1;
   }
}
