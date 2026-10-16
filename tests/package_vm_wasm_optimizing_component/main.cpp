#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <sys/resource.h>
#include <utility>
#include <vector>

import forge.asio.blocking;
import forge.asio.compute;
import forge.asio.runtime;
import forge.vm.wasm.optimizing.compiler_session;
import forge.vm.wasm.optimizing.executor;

// An ordinary imported consumer TU: no named owning-module declaration,
// interpreter/compiler test object, forced standard-library emission or shim.
namespace {
namespace oc = forge::vm::wasm::optimizing;
namespace vm = forge::vm::wasm::interpret;
using clock_type = std::chrono::steady_clock;

std::vector<std::uint8_t> read_bytes(const char* path) {
   auto input = std::ifstream{path, std::ios::binary | std::ios::ate};
   if (!input)
      throw std::runtime_error{"required reviewed fixture unavailable"};
   const auto size = input.tellg();
   if (size <= 0 || size > 2 * 1024 * 1024)
      throw std::runtime_error{"fixture outside bounded consumer input"};
   auto bytes = std::vector<std::uint8_t>(static_cast<std::size_t>(size));
   input.seekg(0);
   if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
      throw std::runtime_error{"fixture read incomplete"};
   return bytes;
}

oc::session_options options(const std::filesystem::path& helper, const std::filesystem::path& cache,
                            std::uint64_t address_bytes) {
   auto value = oc::session_options{};
   value.helper = helper;
   value.cache = cache;
   value.worker_count = 1;
   value.max_pending_jobs = 4;
   value.max_tickets = 8;
   value.max_imports = 8;
   value.max_configuration_bytes = 16384;
   value.max_request_bytes = 1024U * 1024U;
   value.max_response_bytes = 1024U * 1024U;
   value.max_inflight_bytes = 512ULL * 1024U * 1024U;
   value.max_resident_bytes = 8U * 1024U * 1024U;
   value.max_disk_bytes = 8U * 1024U * 1024U;
   value.max_disk_entries = 8;
   value.helper_timeout = std::chrono::seconds{2};
   value.helper_limits = {10, address_bytes, 16U * 1024U * 1024U};
   return value;
}

void drain(forge::asio::runtime& runtime, oc::compiler_session& session, std::exception_ptr& failure) {
   try {
      forge::asio::blocking::run(runtime, session.shutdown(clock_type::now() + std::chrono::milliseconds{100}));
   } catch (...) {
      if (!failure)
         failure = std::current_exception();
      // A failed cleanup retains the SAME owner/compute/runtime for retry.
      // A second failure is a lifecycle gate failure, never clean success.
      forge::asio::blocking::run(runtime, session.shutdown(clock_type::now() + std::chrono::milliseconds{100}));
   }
}

std::optional<oc::artifact> compile(forge::asio::runtime& runtime, forge::asio::compute::pool& compute,
                                    oc::session_options configuration, const oc::compile_request& request,
                                    bool missing_helper) {
   auto failure = std::exception_ptr{};
   auto code = std::optional<oc::artifact>{};
   {
      auto session = oc::compiler_session{runtime, compute.get_executor(), std::move(configuration)};
      try {
         const auto identity = oc::identify(request);
         if (session.ready(identity, request.configuration))
            throw std::runtime_error{"cold session falsely ready"};
         auto unavailable = false;
         try {
            forge::asio::blocking::run(runtime, session.initialize(oc::target_profile::native(), {}));
         } catch (const oc::exceptions::compiler_unavailable&) {
            if (!missing_helper)
               throw;
            unavailable = true;
         }
         if (missing_helper) {
            auto copy = request;
            const auto rejected = session.enqueue(identity, std::move(copy), forge::asio::task::priority{});
            if (!unavailable || rejected.status != oc::admission_status::not_initialized || rejected.completion ||
                session.ready(identity, request.configuration))
               throw std::runtime_error{"missing installed helper did not fail closed"};
         } else {
            auto copy = request;
            auto admitted = session.enqueue(identity, std::move(copy), forge::asio::task::priority{});
            if (admitted.status != oc::admission_status::accepted_unverified || !admitted.completion)
               throw std::runtime_error{"initialized consumer admission failed"};
            auto completed = forge::asio::blocking::run(
                runtime, std::move(*admitted.completion).wait(clock_type::now() + std::chrono::seconds{5}, {}));
            const auto ready = session.ready(identity, request.configuration);
            if (completed.status != oc::compiler_session::completion_status::compiled ||
                completed.code.identity() != identity || !completed.code.is_prepared() || !ready ||
                ready->identity() != identity || !ready->is_prepared())
               throw std::runtime_error{"installed helper completion not checked/prepared/ready"};
            code.emplace(std::move(completed.code));
         }
      } catch (...) {
         failure = std::current_exception();
      }
      drain(runtime, session, failure); // unconditional, including initialize/handshake/helper failures
   }
   if (failure)
      std::rethrow_exception(failure);
   return code;
}

void execute(forge::asio::runtime& runtime, forge::asio::task::scheduler& scheduler, const oc::artifact& artifact,
             std::stop_token stopped) {
   using registry = vm::registered_host_functions<vm::standalone_function_t>;
   const auto code = oc::executor::bind<registry>(artifact);
   // Explicit physical cap: fixed 4MiB reserve plus local payload.
   auto executor = oc::executor{scheduler, {4 * 1024 * 1024 + 256 * 1024, 64 * 1024}};
   auto thread_prepared = false;
   auto failure = std::exception_ptr{};
   try {
      forge::asio::blocking::run(runtime, executor.start(clock_type::now() + std::chrono::seconds{1}));
      executor.prepare_thread();
      thread_prepared = true;
      auto memory = vm::wasm_allocator{};
      const auto arguments =
          std::array<vm::operand_stack_elem, 2>{vm::i32_const_t{std::uint32_t{7}}, vm::i32_const_t{std::uint32_t{9}}};
      const auto result =
          executor.invoke(code, nullptr, 0, arguments, memory, clock_type::now() + std::chrono::seconds{1});
      if (result.trap != oc::trap_reason::none || result.host_error || !result.value ||
          !result.value->is_a<vm::i32_const_t>() || result.value->to_ui32() != 16)
         throw std::runtime_error{"public executor add result mismatch"};
      const auto cancelled =
          executor.invoke(code, nullptr, 0, arguments, memory, clock_type::now() + std::chrono::seconds{1}, stopped);
      if (cancelled.trap != oc::trap_reason::cancelled || cancelled.host_error || cancelled.value)
         throw std::runtime_error{"public executor already-stopped result mismatch"};
   } catch (...) {
      failure = std::current_exception();
   }
   if (thread_prepared) {
      try {
         executor.release_thread();
      } catch (...) {
         if (!failure)
            failure = std::current_exception();
      }
   }
   try {
      forge::asio::blocking::run(runtime, executor.shutdown());
   } catch (...) {
      if (!failure)
         failure = std::current_exception();
      forge::asio::blocking::run(runtime, executor.shutdown());
   }
   if (failure)
      std::rethrow_exception(failure);
}
} // namespace

int main(int argc, char** argv) {
   const auto core_limit = rlimit{0, 0};
   if (::setrlimit(RLIMIT_CORE, &core_limit) != 0)
      return 65;
   try {
      auto called = std::atomic<unsigned>{0};
      auto stop = std::stop_source{};
      {
         auto callback = std::stop_callback{stop.get_token(), [&]() noexcept { called.fetch_add(1); }};
         if (!stop.request_stop() || called.load() != 1)
            return 1;
      }
      if (stop.request_stop() || called.load() != 1)
         return 2;
      {
         auto callback = std::stop_callback{stop.get_token(), [&]() noexcept { called.fetch_add(1); }};
         if (called.load() != 2)
            return 3;
      }
      std::puts("ordinary std::stop_callback registration/stop/destruction/already-stopped PASS");
      if (argc != 3 && argc != 6)
         return 64; // Required input is a gate failure, never skipped PASS.

      const auto request_bytes = read_bytes(argv[1]);
      const auto artifact_bytes = read_bytes(argv[2]);
      const auto request = oc::compile_request::decode(request_bytes, request_bytes.size());
      auto artifact = oc::artifact::decode(artifact_bytes, oc::identify(request), artifact_bytes.size());
      artifact.prepare();
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
      auto compute = forge::asio::compute::pool{forge::asio::compute::pool::options{
          .worker_threads = 1, .max_pending_tasks = 8, .max_waiting_submissions = 8}};
      auto scheduler = forge::asio::task::scheduler{runtime};
      auto failure = std::exception_ptr{};
      try {
         execute(runtime, scheduler, artifact, stop.get_token()); // preserve original public oracle
         if (argc == 6) {
            const auto text = std::string_view{argv[5]};
            auto address_bytes = std::uint64_t{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), address_bytes);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || address_bytes == 0)
               throw std::runtime_error{"explicit finite helper AS input required"};
            const auto helper = std::filesystem::path{argv[3]};
            const auto parent = std::filesystem::path{argv[4]};
            if (!helper.is_absolute() || !parent.is_absolute() || !std::filesystem::is_directory(parent) ||
                std::filesystem::exists(parent / "compiled") || std::filesystem::exists(parent / "missing"))
               throw std::runtime_error{"fresh owned consumer cache paths required"};
            const auto missing = helper.parent_path() / "forge-wasm-compiler-missing";
            if (std::filesystem::exists(missing))
               throw std::runtime_error{"missing-helper negative path is not absent"};
            auto compiled =
                compile(runtime, compute, options(helper, parent / "compiled", address_bytes), request, false);
            if (!compiled)
               throw std::runtime_error{"installed helper produced no artifact"};
            execute(runtime, scheduler, *compiled, stop.get_token()); // pins survive session/cache shutdown
            static_cast<void>(
                compile(runtime, compute, options(missing, parent / "missing", address_bytes), request, true));
            std::puts("installed relocated helper HELLO/compile/prepared/execute/missing-helper/drain PASS");
         }
      } catch (...) {
         failure = std::current_exception();
      }
      // Every initial/helper/guest failure goes through the SAME owner drains.
      try {
         forge::asio::blocking::run(runtime, scheduler.shutdown());
      } catch (...) {
         if (!failure)
            failure = std::current_exception();
         try {
            forge::asio::blocking::run(runtime, scheduler.shutdown());
         } catch (...) {
            // Still attempt compute drain with ALL injected storage alive.
            // Non-quiescent owner destruction remains a failing lifecycle
            // guard, not permission to report clean shutdown or detach work.
         }
      }
      try {
         forge::asio::blocking::run(runtime, compute.shutdown());
      } catch (...) {
         if (!failure)
            failure = std::current_exception();
         try {
            forge::asio::blocking::run(runtime, compute.shutdown());
         } catch (...) {
         }
      }
      if (failure)
         std::rethrow_exception(failure);
      std::puts("ordinary public executor add=16/already-stopped/drain PASS");
      return 0;
   } catch (const std::exception&) {
      // No request bytes, helper stderr, environment or payload are printed.
      std::fputs("ordinary installed OC consumer gate failed\n", stderr);
      return 90;
   }
}
