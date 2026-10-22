#include <atomic>
#include <boost/scope/scope_exit.hpp>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "details/engine_load_budget.hxx"

import forge.asio.blocking;
import forge.vm.wasm.optimizing.compiler_session;

namespace {
namespace oc = forge::vm::wasm::optimizing;
using budget = forge::vm::wasm::interpret::detail::engine_load_budget;
using namespace std::chrono_literals;
void require(bool value, const char* text) {
   if (!value)
      throw std::runtime_error(text);
}
std::vector<std::uint8_t> read(const char* path) {
   std::ifstream input(path, std::ios::binary);
   require(bool(input), "Cannot read cleanup fixture");
   return {std::istreambuf_iterator<char>{input}, {}};
}
template <typename Error, typename Work> void rejects(Work work) {
   auto rejected = false;
   try {
      work();
   } catch (const Error&) {
      rejected = true;
   }
   require(rejected, "Expected typed cleanup refusal");
}
oc::session_options options(const char* helper, const std::filesystem::path& cache, std::uint64_t address) {
   oc::session_options value;
   value.helper = helper;
   value.cache = cache;
   value.worker_count = 1;
   value.max_pending_jobs = 2;
   value.max_tickets = 4;
   value.max_imports = 8;
   value.max_configuration_bytes = 16384;
   value.max_request_bytes = 4 * 1024 * 1024;
   value.max_response_bytes = 16 * 1024 * 1024;
   value.max_inflight_bytes = 512ULL * 1024 * 1024;
   value.max_resident_bytes = 64 * 1024 * 1024;
   value.max_disk_bytes = 32 * 1024 * 1024;
   value.max_disk_entries = 8;
   value.helper_timeout = 120s;
   value.helper_limits = {120, address, 16 * 1024 * 1024};
   return value;
}
auto submit(oc::compiler_session& session, const oc::compile_request& request) {
   auto admitted = session.enqueue(oc::identify(request), oc::compile_request{request}, forge::asio::task::priority{});
   require(admitted.completion.has_value(), "Cleanup request was not admitted");
   return admitted;
}
auto wait(forge::asio::runtime& runtime, oc::compiler_session::enqueue_result& admitted) {
   return forge::asio::blocking::run(runtime,
                                     std::move(*admitted.completion).wait(std::chrono::steady_clock::now() + 150s, {}));
}

void direct(const std::vector<std::uint8_t>& encoded, const oc::compile_request& request) {
   std::atomic_bool hold = true;
   std::atomic<std::uint64_t> attempts = 0;
   std::atomic_bool refunded = false;
   const budget::fault_scope faults{{.protection = 1, .hold_unmapping = &hold, .unmapping_attempts = &attempts}};
   auto charge = std::shared_ptr<void>{new int{}, [&](void* pointer) {
                                          delete static_cast<int*>(pointer);
                                          refunded = true;
                                       }};
   auto code = std::optional{
       oc::artifact::decode(encoded, oc::identify(request), 16 * 1024 * 1024, oc::artifact::origin::isolated_compiler)};
   const auto drain = boost::scope::scope_exit{[&] {
      hold = false;
      if (code)
         static_cast<void>(code->retry_cleanup());
   }};
   rejects<oc::exceptions::compile_limit>(
       [&] { code->prepare(oc::artifact::standalone_preparation_policy, {}, charge); });
   charge.reset();
   require(!code->is_prepared() && attempts >= 2 && !refunded, "Failed native rollback lost its physical owner/charge");
   require(!code->retry_cleanup() && !refunded, "Persistent cleanup failure falsely drained");
   hold = false;
   require(code->retry_cleanup() && !refunded, "Explicit cleanup retry failed or refunded live artifact storage");
   code.reset();
   require(refunded, "Explicit retry/final artifact drop did not release charge");
}

void final_pin(const std::vector<std::uint8_t>& encoded, const oc::compile_request& request) {
   using preparation = forge::vm::wasm::interpret::prepared_module::preparation;
   std::atomic_bool hold = false;
   std::atomic<std::uint64_t> attempts = 0;
   std::atomic_bool refunded = false;
   const budget::fault_scope faults{{.hold_unmapping = &hold, .unmapping_attempts = &attempts}};
   const auto drain = boost::scope::scope_exit{[&] {
      hold = false;
      static_cast<void>(preparation::retry_pending_cleanup(1));
   }};
   auto charge = std::shared_ptr<void>{new int{}, [&](void* pointer) {
                                          delete static_cast<int*>(pointer);
                                          refunded = true;
                                       }};
   auto code = std::optional{
       oc::artifact::decode(encoded, oc::identify(request), 16 * 1024 * 1024, oc::artifact::origin::isolated_compiler)};
   code->prepare(oc::artifact::standalone_preparation_policy, {}, charge);
   // Preparation may release temporary mappings; inject only final-owner cleanup.
   attempts = 0;
   hold = true;
   auto module = code->runtime_module();
   auto weak = std::weak_ptr{module};
   charge.reset();
   code.reset();
   module.reset();
   weak.reset();
   require(attempts >= 2 && !refunded, "Final module-drop cleanup lost its durable owner");
   const auto pending = preparation::retry_pending_cleanup(1);
   require(pending.attempted == 1 && pending.remaining != 0 && !refunded,
           "Bounded maintenance falsely drained a held native mapping");
   hold = false;
   const auto recovered = preparation::retry_pending_cleanup(1);
   require(recovered.attempted == 1 && recovered.remaining == 0 && refunded,
           "Explicit maintenance failed after final artifact/module/weak pin release");
}

void session_failure(forge::asio::runtime& runtime, forge::asio::compute::pool& compute,
                     const oc::session_options& configuration, const oc::compile_request& request) {
   // Process-scoped private injection is restricted to this dedicated serial
   // subprocess. The real existing compute worker creates the preparation.
   std::atomic_bool hold = true;
   std::atomic<std::uint64_t> attempts = 0;
   const budget::fault_scope faults{{.protection = 1, .hold_unmapping = &hold, .unmapping_attempts = &attempts}};
   auto session = oc::compiler_session{runtime, compute.get_executor(), configuration};
   const auto base = session.usage();
   auto failure = std::exception_ptr{};
   try {
      forge::asio::blocking::run(runtime, session.initialize(oc::target_profile::native(), {}));
      auto admitted = submit(session, request);
      rejects<oc::exceptions::shutdown_incomplete>([&] { static_cast<void>(wait(runtime, admitted)); });
      const auto retained = session.usage();
      require(attempts >= 2 && retained.resident_bytes > base.resident_bytes &&
                  retained.inflight_bytes > base.inflight_bytes,
              "Worker failure dropped unreachable cleanup or refunded its reservations");
      require(!session.ready(oc::identify(request), request.configuration), "Failed cleanup published ready code");
      rejects<oc::exceptions::shutdown_incomplete>(
          [&] { forge::asio::blocking::run(runtime, session.shutdown(std::chrono::steady_clock::now() + 2s)); });
      require(session.usage().resident_bytes == retained.resident_bytes &&
                  session.usage().inflight_bytes > base.inflight_bytes,
              "Incomplete shutdown released physical cleanup reservation");
   } catch (...) {
      failure = std::current_exception();
   }
   hold = false;
   forge::asio::blocking::run(runtime, session.shutdown(std::chrono::steady_clock::now() + 2s));
   const auto drained = session.usage();
   require(drained.resident_bytes == base.resident_bytes && drained.inflight_bytes == base.inflight_bytes,
           "Explicit worker cleanup retry did not restore both account baselines");
   if (failure)
      std::rethrow_exception(failure);
}
} // namespace

int main(int argc, char** argv) {
   try {
      if (argc != 6)
         return 2;
      std::uint64_t address = 0;
      const auto text = std::string{argv[5]};
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), address);
      require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && address,
              "Explicit finite helper address limit is required");
      const auto request = oc::compile_request::decode(read(argv[2]), 4 * 1024 * 1024);
      direct(read(argv[3]), request);
      final_pin(read(argv[3]), request);
      auto pattern = (std::filesystem::path{argv[4]} / "session-cleanup-XXXXXX").string();
      require(::mkdtemp(pattern.data()), "Cannot create cleanup test cache");
      const auto configuration = options(argv[1], pattern, address);
      auto runtime = forge::asio::runtime{};
      auto compute = forge::asio::compute::pool{forge::asio::compute::pool::options{
          .worker_threads = 2, .max_pending_tasks = 8, .max_waiting_submissions = 8}};
      auto failure = std::exception_ptr{};
      try {
         session_failure(runtime, compute, configuration, request);
         require(!std::filesystem::exists(configuration.cache / (oc::identify(request).str() + ".foc")),
                 "Failed native cleanup published a disk artifact");
         auto session = oc::compiler_session{runtime, compute.get_executor(), configuration};
         auto recovery_error = std::exception_ptr{};
         try {
            forge::asio::blocking::run(runtime, session.initialize(oc::target_profile::native(), {}));
            auto admitted = submit(session, request);
            require(wait(runtime, admitted).code.is_prepared(),
                    "Explicit cleanup recovery poisoned future real-helper preparation");
         } catch (...) {
            recovery_error = std::current_exception();
         }
         forge::asio::blocking::run(runtime, session.shutdown(std::chrono::steady_clock::now() + 2s));
         if (recovery_error)
            std::rethrow_exception(recovery_error);
      } catch (...) {
         failure = std::current_exception();
      }
      forge::asio::blocking::run(runtime, compute.shutdown());
      if (failure)
         std::rethrow_exception(failure);
      std::cout
          << "PASS real artifact/worker repeated-unmap refusal, retained cleanup, explicit retry/refund and recovery\n";
      return 0;
   } catch (const std::exception& error) {
      std::cerr << "FAIL " << error.what() << '\n';
      return 1;
   } catch (...) {
      std::cerr << "FAIL unexpected typed error\n";
      return 1;
   }
}
