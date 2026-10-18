#include <boost/asio/awaitable.hpp>
#include <array>
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
#include <string_view>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>
#include "details/compiler_session_probe.hxx"

import forge.asio.blocking;
import forge.vm.wasm.optimizing.compiler_session;

namespace {
namespace oc = forge::vm::wasm::optimizing;
using probe = oc::detail::compiler_session_probe;
using namespace std::chrono_literals;
static_assert(std::is_trivially_copyable_v<probe::snapshot>);

void require(bool value, const char* message) {
   if (!value)
      throw std::runtime_error(message);
}
std::vector<std::uint8_t> read(const char* path) {
   auto input = std::ifstream{path, std::ios::binary};
   require(bool(input), "Cannot read probe request fixture");
   return {std::istreambuf_iterator<char>{input}, {}};
}
oc::session_options options(const char* helper, const std::filesystem::path& cache, std::uint64_t address,
                            std::size_t workers) {
   auto value = oc::session_options{};
   value.helper = helper;
   value.cache = cache;
   value.worker_count = workers;
   value.max_pending_jobs = 4;
   value.max_tickets = 8;
   value.max_imports = 8;
   value.max_configuration_bytes = 16384;
   value.max_request_bytes = 1024 * 1024;
   value.max_response_bytes = 1024 * 1024;
   value.max_inflight_bytes = 512ULL * 1024 * 1024;
   value.max_resident_bytes = 64 * 1024 * 1024;
   value.max_disk_bytes = 32 * 1024 * 1024;
   value.max_disk_entries = 8;
   value.helper_timeout = 120s;
   value.helper_limits = {120, address, 16 * 1024 * 1024};
   return value;
}
void initialize(forge::asio::runtime& runtime, oc::compiler_session& session) {
   require(forge::asio::blocking::run_for(runtime, session.initialize(oc::target_profile::native(), {}), 150s),
           "Real helper initialization exceeded its test deadline");
}
auto wait(forge::asio::runtime& runtime, oc::compiler_session::enqueue_result& result) {
   return forge::asio::blocking::run(runtime,
                                     std::move(*result.completion).wait(std::chrono::steady_clock::now() + 150s, {}));
}
template <std::size_t Count> struct queued_requests {
   std::array<oc::compiler_session::enqueue_result, Count> values;
   oc::compiler_session::memory_usage usage;
};
template <std::size_t Count>
boost::asio::awaitable<queued_requests<Count>> enqueue_queued(oc::compiler_session& session,
                                                              std::array<const oc::compile_request*, Count> requests) {
   auto result = queued_requests<Count>{};
   // The sole Asio runtime thread cannot run scheduler-posted verification
   // until all records and this baseline have been captured without suspension.
   for (std::size_t i = 0; i < Count; ++i) {
      result.values[i] =
          session.enqueue(oc::identify(*requests[i]), oc::compile_request{*requests[i]}, forge::asio::task::priority{});
      require(result.values[i].status == oc::admission_status::accepted_unverified && result.values[i].completion,
              "Probe request was not admitted");
   }
   result.usage = session.usage();
   co_return std::move(result);
}
void paused(const probe::observation& value, std::size_t expected) {
   require(value.arrived == expected && value.paused == expected && !value.released && !value.timed_out &&
               !value.overflow,
           "Expected preparation boundary did not remain paused before its deadline");
}
void baseline(oc::compiler_session& session, oc::compiler_session::memory_usage expected) {
   const auto actual = session.usage();
   require(actual.resident_bytes == expected.resident_bytes && actual.inflight_bytes == expected.inflight_bytes,
           "Drained session did not restore exact accounting baselines");
}
void compute_fence(forge::asio::runtime& runtime, forge::asio::compute::pool& compute) {
   // Used only by the one-worker late-cancel case: the next work item cannot
   // start before the preceding post-work cancellation/metrics update finishes.
   require(forge::asio::blocking::run_for(
               runtime,
               compute.get_executor().execute(forge::asio::compute::task_options{.name = "probe-drain-fence"}, [] {}),
               10s),
           "One-worker compute drain fence exceeded its test deadline");
}
void recovery(forge::asio::runtime& runtime, forge::asio::compute::pool& compute,
              const oc::session_options& configuration, const oc::compile_request& request) {
   auto session = oc::compiler_session{runtime, compute.get_executor(), configuration};
   const auto base = session.usage();
   auto failure = std::exception_ptr{};
   try {
      initialize(runtime, session);
      auto queued = forge::asio::blocking::run(
          runtime, enqueue_queued(session, std::array<const oc::compile_request*, 1>{&request}));
      require(wait(runtime, queued.values[0]).code.is_prepared(),
              "Late cancellation poisoned a fresh real-helper session on the same compute pool");
   } catch (...) {
      failure = std::current_exception();
   }
   forge::asio::blocking::run(runtime, session.shutdown(std::chrono::steady_clock::now() + 2s));
   compute_fence(runtime, compute);
   baseline(session, base);
   if (failure)
      std::rethrow_exception(failure);
}

void late(forge::asio::runtime& runtime, forge::asio::compute::pool& compute, const oc::session_options& configuration,
          const oc::compile_request& request) {
   auto session = oc::compiler_session{runtime, compute.get_executor(), configuration};
   const auto base = session.usage();
   auto observer = std::optional<probe>{};
   auto failure = std::exception_ptr{};
   auto metrics = forge::asio::compute::metrics{};
   auto snapshot = probe::snapshot{};
   try {
      initialize(runtime, session);
      observer.emplace(probe::point::prepared_before_return, std::chrono::steady_clock::now() + 180s);
      auto queued = forge::asio::blocking::run(
          runtime, enqueue_queued(session, std::array<const oc::compile_request*, 1>{&request}));
      const auto observed = observer->wait(1);
      paused(observed, 1);
      snapshot = observed.snapshots[0];
      require(snapshot.session && snapshot.job && snapshot.prepared && snapshot.resident_bytes &&
                  snapshot.workspace_bytes == 0,
              "Late point was not after successful emplace and reservation refund");
      const auto held = session.usage();
      require(held.resident_bytes == queued.usage.resident_bytes + snapshot.resident_bytes +
                                         snapshot.configuration_bytes + snapshot.token_bytes &&
                  held.inflight_bytes == queued.usage.inflight_bytes,
              "Late point lost actual resident charge or retained preparation workspace");
      require(!session.ready(oc::identify(request), request.configuration),
              "Prepared worker published before returning compute");
      metrics = compute.snapshot();
      require(metrics.running == 1 && metrics.pending == 0 && metrics.waiting == 0,
              "Late-cancel compute metrics were not isolated to the paused operation");
      session.request_stop();
      // Destroy the controller while the worker still pins its observer state.
      // No stop check or exception is supplied by the fixed-point probe.
      observer.reset();
      auto cancelled = false;
      try {
         static_cast<void>(wait(runtime, queued.values[0]));
      } catch (const oc::exceptions::cancelled&) {
         cancelled = true;
      }
      require(cancelled, "Late cancellation produced a successful or untyped ticket result");
   } catch (...) {
      failure = std::current_exception();
   }
   session.request_stop();
   observer.reset(); // Always open before the mandatory drain, including assertion failures.
   forge::asio::blocking::run(runtime, session.shutdown(std::chrono::steady_clock::now() + 2s));
   compute_fence(runtime, compute);
   baseline(session, base);
   if (failure)
      std::rethrow_exception(failure);
   const auto drained = compute.snapshot();
   require(drained.canceled == metrics.canceled + 1 && drained.failed == metrics.failed,
           "Real compute post-work check did not cancel the successfully prepared operation");
   require(!std::filesystem::exists(configuration.cache / (oc::identify(request).str() + ".foc")),
           "Late-cancelled preparation reached disk publication");
   recovery(runtime, compute, configuration, request);
   std::cout << "PASS late prepared-before-compute-return cancellation; actual_resident=" << snapshot.resident_bytes
             << " compute_canceled_delta=1; observer-close/drain/refund/recovery\n";
}

void overlap(forge::asio::runtime& runtime, forge::asio::compute::pool& compute,
             const oc::session_options& configuration, const oc::compile_request& request) {
   auto a = request;
   auto b = request;
   a.wasm.insert(a.wasm.end(), {0, 2, 1, 'p'});
   b.wasm.insert(b.wasm.end(), {0, 2, 1, 'q'});
   require(oc::identify(a) != oc::identify(b), "Overlap requests did not have distinct identities");
   auto session = oc::compiler_session{runtime, compute.get_executor(), configuration};
   const auto base = session.usage();
   auto observer = std::optional<probe>{};
   auto failure = std::exception_ptr{};
   auto snapshots = std::array<probe::snapshot, 2>{};
   auto modules = std::array<std::shared_ptr<forge::vm::wasm::interpret::prepared_module>, 2>{};
   auto actual_bytes = std::uint64_t{};
   try {
      initialize(runtime, session);
      observer.emplace(probe::point::reservations_held, std::chrono::steady_clock::now() + 180s);
      auto queued = forge::asio::blocking::run(
          runtime, enqueue_queued(session, std::array<const oc::compile_request*, 2>{&a, &b}));
      const auto observed = observer->wait(2);
      paused(observed, 2);
      snapshots = observed.snapshots;
      const auto& first = snapshots[0];
      const auto& second = snapshots[1];
      require(first.session && first.session == second.session && first.job && second.job && first.job != second.job &&
                  !first.prepared && !second.prepared && first.resident_bytes && second.resident_bytes &&
                  first.workspace_bytes && second.workspace_bytes,
              "Two distinct preparation reservations were not held together");
      const auto held = session.usage();
      require(held.inflight_bytes == queued.usage.inflight_bytes + first.workspace_bytes + second.workspace_bytes &&
                  held.resident_bytes == queued.usage.resident_bytes + first.resident_bytes + second.resident_bytes +
                                             first.configuration_bytes + second.configuration_bytes +
                                             first.token_bytes + second.token_bytes &&
                  held.inflight_bytes <= configuration.max_inflight_bytes &&
                  held.resident_bytes <= configuration.max_resident_bytes,
              "Simultaneously held reservations did not match the exact aggregate account formula");
      require(!session.ready(oc::identify(a), a.configuration) && !session.ready(oc::identify(b), b.configuration),
              "Reservation pause unexpectedly published ready code");
      std::cout << "SESSION overlap_paused=2 queued_inflight=" << queued.usage.inflight_bytes
                << " workspace_a=" << first.workspace_bytes << " workspace_b=" << second.workspace_bytes
                << " held_inflight=" << held.inflight_bytes << " held_resident=" << held.resident_bytes << '\n';
      observer.reset();
      for (std::size_t i = 0; i < modules.size(); ++i) {
         auto result = wait(runtime, queued.values[i]);
         require(result.code.is_prepared(), "Held reservation did not complete a real native preparation");
         actual_bytes += result.code.byte_size();
         modules[i] = result.code.runtime_module();
      }
      require(modules[0]->native_code().data() != modules[1]->native_code().data(),
              "Distinct preparations unexpectedly shared a native image");
      require(session.ready(oc::identify(a), a.configuration) && session.ready(oc::identify(b), b.configuration),
              "Successful distinct preparations were not both published");
   } catch (...) {
      failure = std::current_exception();
   }
   if (failure)
      session.request_stop();
   observer.reset(); // No test gate can obstruct shutdown on any failure path.
   forge::asio::blocking::run(runtime, session.shutdown(std::chrono::steady_clock::now() + 2s));
   if (!failure) {
      const auto retained = session.usage();
      require(retained.inflight_bytes == base.inflight_bytes &&
                  retained.resident_bytes ==
                      base.resident_bytes + actual_bytes + snapshots[0].token_bytes + snapshots[1].token_bytes,
              "Shutdown lost external module charges or retained preparation workspace");
      for (const auto& module : modules)
         require(module && !module->native_code().empty(), "Shutdown invalidated a distinct external module pin");
   }
   modules = {};
   baseline(session, base);
   if (failure)
      std::rethrow_exception(failure);
   std::cout << "PASS two real preparations with simultaneous resident/workspace reservations, pins and exact refund\n";
}
} // namespace

int main(int argc, char** argv) {
   try {
      if (argc != 6)
         return 2;
      const auto mode = std::string_view{argv[1]};
      require(mode == "late" || mode == "overlap", "Unknown fixed-point test case");
      auto address = std::uint64_t{};
      const auto text = std::string_view{argv[5]};
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), address);
      require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && address,
              "Explicit finite diagnostic helper address limit is required");
      const auto request = oc::compile_request::decode(read(argv[3]), 1024 * 1024);
      auto pattern = (std::filesystem::path{argv[4]} / "session-probe-XXXXXX").string();
      require(::mkdtemp(pattern.data()) != nullptr, "Cannot create private fixed-point test cache");
      const auto workers = mode == "late" ? 1U : 2U;
      const auto configuration = options(argv[2], pattern, address, workers);
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
      auto compute = forge::asio::compute::pool{forge::asio::compute::pool::options{
          .worker_threads = workers, .max_pending_tasks = 8, .max_waiting_submissions = 8}};
      auto failure = std::exception_ptr{};
      try {
         if (mode == "late")
            late(runtime, compute, configuration, request);
         else
            overlap(runtime, compute, configuration, request);
      } catch (...) {
         failure = std::current_exception();
      }
      forge::asio::blocking::run(runtime, compute.shutdown());
      if (failure)
         std::rethrow_exception(failure);
      return 0;
   } catch (const std::exception& error) {
      std::cerr << "FAIL " << error.what() << '\n';
      return 1;
   } catch (...) {
      std::cerr << "FAIL unexpected typed fixed-point error\n";
      return 1;
   }
}
