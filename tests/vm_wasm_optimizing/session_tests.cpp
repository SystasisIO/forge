#include <boost/asio/awaitable.hpp>
#include <algorithm>
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
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

import forge.asio.blocking;
import forge.raw.raw;
import forge.vm.wasm.interpret.capacity_profile;
import forge.vm.wasm.optimizing.compiler_session;

namespace {
namespace oc = forge::vm::wasm::optimizing;
using namespace std::chrono_literals;
void require(bool value, const char* message) {
   if (!value)
      throw std::runtime_error(message);
}
std::vector<std::uint8_t> read(const std::filesystem::path& path) {
   auto input = std::ifstream(path, std::ios::binary);
   require(bool(input), "Cannot read session fixture");
   return {std::istreambuf_iterator<char>(input), {}};
}
void write(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
   auto output = std::ofstream(path, std::ios::binary | std::ios::trunc);
   output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
   require(bool(output), "Cannot write owned cache fixture");
   output.close();
   std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
}
oc::session_options options(const char* helper, const std::filesystem::path& cache, std::uint64_t address_bytes) {
   auto result = oc::session_options{};
   result.helper = helper;
   result.cache = cache;
   result.worker_count = 1;
   result.max_pending_jobs = 4;
   result.max_tickets = 8;
   result.max_imports = 8;
   result.max_configuration_bytes = 16384;
   result.max_request_bytes = 1024 * 1024;
   result.max_response_bytes = 1024 * 1024;
   result.max_inflight_bytes = 512ULL * 1024 * 1024;
   result.max_resident_bytes = 64 * 1024 * 1024;
   result.max_disk_bytes = 32 * 1024 * 1024;
   result.max_disk_entries = 8;
   result.helper_timeout = 120s;
   result.helper_limits = {120, address_bytes, 16 * 1024 * 1024};
   // Additive preparation limits inherit worker shares and current shared headroom.
   return result;
}
auto enqueue(oc::compiler_session& session, const oc::compile_request& request,
             std::source_location caller = std::source_location::current()) {
   auto result = session.enqueue(oc::identify(request), oc::compile_request{request}, forge::asio::task::priority{});
   if (result.status != oc::admission_status::accepted_unverified || !result.completion)
      throw std::runtime_error{"Session rejected bounded request admission at line " + std::to_string(caller.line()) +
                               ", status=" + std::to_string(static_cast<unsigned>(result.status))};
   return result;
}
auto wait(forge::asio::runtime& runtime, oc::compiler_session::enqueue_result& result) {
   return forge::asio::blocking::run(runtime,
                                     std::move(*result.completion).wait(std::chrono::steady_clock::now() + 150s, {}));
}
template <typename Error, typename Work> void rejects(Work work) {
   auto rejected = false;
   try {
      work();
   } catch (const Error&) {
      rejected = true;
   }
   require(rejected, "Expected typed session rejection");
}
template <oc::exceptions::code Expected, typename Work> void rejects_helper(Work work) {
   auto rejected = false;
   try {
      work();
   } catch (const forge::exceptions::runtime_coded_exception<oc::exceptions::code>& error) {
      if (error.value() != Expected || error.code() != oc::exceptions::make_error_code(Expected))
         throw;
      rejected = true;
   }
   require(rejected, "Expected exact typed helper rejection");
}

void helper_rejection_control() {
   using failure = forge::exceptions::runtime_coded_exception<oc::exceptions::code>;
   rejects_helper<oc::exceptions::code::compile_limit>(
       [] { throw failure{oc::exceptions::code::compile_limit, "matching helper control"}; });
   auto wrong_escaped = false;
   try {
      rejects_helper<oc::exceptions::code::compile_limit>(
          [] { throw failure{oc::exceptions::code::queue_full, "wrong helper control"}; });
   } catch (const failure& error) {
      wrong_escaped = error.value() == oc::exceptions::code::queue_full;
   }
   require(wrong_escaped, "Helper rejection check swallowed a different typed failure");
   rejects<std::runtime_error>([] { rejects_helper<oc::exceptions::code::compile_limit>([] {}); });
   rejects<oc::exceptions::compile_limit>([] {
      rejects_helper<oc::exceptions::code::compile_limit>([] { throw oc::exceptions::compile_limit{"local control"}; });
   });
}

std::uint64_t decode_workspace(const oc::session_options& configuration) {
   const auto element_bytes = std::max(
       {sizeof(oc::import_signature), sizeof(oc::image_section), sizeof(oc::image_relocation),
        sizeof(forge::vm::wasm::interpret::host_function), sizeof(oc::compiled_function), sizeof(oc::compiled_export),
        sizeof(oc::global_initial_value), sizeof(oc::memory_initial_segment), sizeof(std::uint32_t)});
   return forge::raw::max_array_elements * (element_bytes + 4 * sizeof(std::uint64_t)) +
          configuration.max_response_bytes * 8 + configuration.max_request_bytes * 3 +
          configuration.max_configuration_bytes * 4;
}

template <std::size_t Count>
boost::asio::awaitable<std::array<oc::compiler_session::enqueue_result, Count>>
enqueue_queued(oc::compiler_session& session, const oc::session_options& configuration,
               std::array<const oc::compile_request*, Count> requests, bool cancel_last = false) {
   // One runtime thread and no suspension: scheduler-posted verification cannot
   // run until every precharged record and the usage snapshot are present.
   const auto before = session.usage().inflight_bytes;
   const auto workspace = decode_workspace(configuration);
   auto results = std::array<oc::compiler_session::enqueue_result, Count>{};
   auto previous = before;
   auto records = std::uint64_t{};
   for (std::size_t i = 0; i < Count; ++i) {
      results[i] = enqueue(session, *requests[i]);
      const auto current = session.usage().inflight_bytes;
      require(current > previous && current - previous > workspace,
              "Queued admission did not retain its decode/request/ticket precharge");
      records += current - previous;
      previous = current;
   }
   require(previous == before + records && previous < configuration.max_inflight_bytes,
           "Accepted queued records left no preparation headroom");
   std::cout << "SESSION queued_jobs=" << Count << " decode_workspace_per_job=" << workspace
             << " base_inflight=" << before << " admitted_record_bytes=" << records
             << " preparation_headroom=" << configuration.max_inflight_bytes - previous << '\n';
   if (cancel_last)
      require(results.back().completion->cancel(), "Consumer cancellation was not accepted");
   co_return std::move(results);
}

boost::asio::awaitable<std::array<oc::compiler_session::enqueue_result, 3>>
saturate_queued(oc::compiler_session& session, const oc::session_options& configuration,
                const oc::compile_request& request) {
   const auto before = session.usage().inflight_bytes;
   auto admitted = std::array<oc::compiler_session::enqueue_result, 3>{};
   for (auto& result : admitted)
      result = enqueue(session, request);
   const auto charged = session.usage().inflight_bytes;
   const auto workspace = decode_workspace(configuration);
   require(workspace * 4 > configuration.max_inflight_bytes,
           "512 MiB saturation control no longer exceeds the proved four-record bound");
   auto fourth = session.enqueue(oc::identify(request), oc::compile_request{request}, forge::asio::task::priority{});
   require(fourth.status == oc::admission_status::saturated && !fourth.completion &&
               session.usage().inflight_bytes == charged,
           "Over-budget queued admission succeeded or retained a rejected reservation");
   std::cout << "SESSION saturated_budget=" << configuration.max_inflight_bytes
             << " three_record_bytes=" << charged - before << " four_decode_workspaces=" << workspace * 4 << '\n';
   session.request_stop();
   co_return std::move(admitted);
}

void admission_saturation(forge::asio::runtime& runtime, forge::asio::compute::pool& compute,
                          oc::session_options configuration, const oc::compile_request& request) {
   auto session = oc::compiler_session{runtime, compute.get_executor(), configuration};
   const auto base = session.usage();
   auto failure = std::exception_ptr{};
   try {
      forge::asio::blocking::run(runtime, session.initialize(oc::target_profile::native(), {}));
      auto admitted = forge::asio::blocking::run(runtime, saturate_queued(session, configuration, request));
      for (auto& result : admitted)
         rejects<oc::exceptions::cancelled>([&] { static_cast<void>(wait(runtime, result)); });
   } catch (...) {
      failure = std::current_exception();
   }
   forge::asio::blocking::run(runtime, session.shutdown(std::chrono::steady_clock::now() + 2s));
   const auto drained = session.usage();
   require(drained.resident_bytes == base.resident_bytes && drained.inflight_bytes == base.inflight_bytes,
           "Saturated admission/stop did not return exact reservations");
   if (failure)
      std::rethrow_exception(failure);
}

void exercise(forge::asio::runtime& runtime, forge::asio::compute::pool& compute, oc::session_options configuration,
              const oc::compile_request& request, const oc::compile_request& multi_result,
              oc::compiler_session::completion_status expected) {
   auto failure = std::exception_ptr{};
   auto session = oc::compiler_session{runtime, compute.get_executor(), configuration};
   const auto base = session.usage();
   const auto identity = oc::identify(request);
   auto module = std::shared_ptr<forge::vm::wasm::interpret::prepared_module>{};
   auto weak = std::weak_ptr<forge::vm::wasm::interpret::prepared_module>{};
   try {
      require(!session.ready(identity, request.configuration), "Cold session published code");
      forge::asio::blocking::run(runtime, session.initialize(oc::target_profile::native(), {}));
      auto queued = forge::asio::blocking::run(
          runtime, enqueue_queued(session, configuration,
                                  std::array<const oc::compile_request*, 3>{&request, &request, &request}, true));
      auto first = std::move(queued[0]);
      auto parallel = std::move(queued[1]);
      auto cancelled = std::move(queued[2]);
      rejects<oc::exceptions::cancelled>([&] { static_cast<void>(wait(runtime, cancelled)); });
      auto completion = std::optional{wait(runtime, first)};
      require(completion->status == expected && completion->code.is_prepared(),
              "Cold/restart completion status mismatch");
      auto duplicate = std::optional{wait(runtime, parallel)};
      require(duplicate->code.native_code().data() == completion->code.native_code().data(),
              "Parallel consumers did not share one prepared artifact");
      duplicate.reset();
      auto ready = session.ready(identity, request.configuration);
      require(ready && ready->native_code().data() == completion->code.native_code().data(),
              "Ready lookup changed mapping");
      ready.reset();
      auto warm = enqueue(session, request);
      auto warm_result = std::optional{wait(runtime, warm)};
      require(warm_result->status == oc::compiler_session::completion_status::already_ready,
              "Warm request performed another preparation");
      warm_result.reset();
      module = completion->code.runtime_module();
      weak = module;
      completion.reset();

      auto oversized_pages = request;
      oversized_pages.configuration.execution.max_pages = forge::vm::wasm::interpret::capacity_profile::max_pages + 1;
      for (const auto* invalid : std::array<const oc::compile_request*, 2>{&multi_result, &oversized_pages}) {
         std::cout << "SESSION helper_rejection=" << (invalid == &multi_result ? "multiple-results" : "oversized-pages")
                   << '\n';
         auto ticket = enqueue(session, *invalid);
         rejects_helper<oc::exceptions::code::compile_limit>([&] { static_cast<void>(wait(runtime, ticket)); });
      }
      auto wrong = request;
      wrong.wasm.push_back(0);
      auto invalid = session.enqueue(identity, std::move(wrong), forge::asio::task::priority{});
      require(invalid.completion.has_value(), "Wrong hint did not reach verification");
      rejects<oc::exceptions::invalid_module>([&] { static_cast<void>(wait(runtime, invalid)); });
      auto reuse = enqueue(session, request);
      require(wait(runtime, reuse).status == oc::compiler_session::completion_status::already_ready,
              "Typed rejection poisoned ready reuse");
   } catch (...) {
      failure = std::current_exception();
   }
   forge::asio::blocking::run(runtime, session.shutdown(std::chrono::steady_clock::now() + 2s));
   require(!session.ready(identity, request.configuration), "Stopped session retained ready admission");
   if (failure)
      std::rethrow_exception(failure);
   require(module && !module->native_code().empty(), "Shutdown invalidated external runtime module pin");
   const auto retained = session.usage();
   require(retained.resident_bytes > base.resident_bytes && retained.inflight_bytes == base.inflight_bytes,
           "Shutdown lost retained charge or kept preparation workspace");
   module.reset();
   require(weak.expired() && session.usage().resident_bytes == retained.resident_bytes,
           "Weak control-block pin refunded physical storage early");
   weak.reset();
   require(session.usage().resident_bytes == base.resident_bytes,
           "Final physical owner did not refund exact resident charge");
}

void eviction(forge::asio::runtime& runtime, forge::asio::compute::pool& compute, oc::session_options configuration,
              const oc::compile_request& request) {
   configuration.max_tickets = 1;
   auto session = oc::compiler_session{runtime, compute.get_executor(), configuration};
   auto failure = std::exception_ptr{};
   try {
      forge::asio::blocking::run(runtime, session.initialize(oc::target_profile::native(), {}));
      auto first = enqueue(session, request);
      auto result = std::optional{wait(runtime, first)};
      const auto first_bytes = result->code.byte_size();
      auto module = result->code.runtime_module();
      auto weak = std::weak_ptr{module};
      result.reset();
      auto other = request;
      other.wasm.insert(other.wasm.end(), {0, 2, 1, 'b'}); // Valid, identity-distinct custom section.
      auto blocked = enqueue(session, other);
      rejects<oc::exceptions::queue_full>([&] { static_cast<void>(wait(runtime, blocked)); });
      module.reset();
      auto admitted = enqueue(session, other);
      auto second = std::optional{wait(runtime, admitted)};
      require(second->code.is_prepared(), "Releasing module pin did not permit eviction/reuse");
      require(weak.expired(), "Second preparation did not evict the old strong artifact owner");
      const auto with_weak = session.usage().resident_bytes;
      require(with_weak <= configuration.max_resident_bytes,
              "Weak-pin replacement exceeded the unchanged resident budget");
      weak.reset();
      const auto without_weak = session.usage().resident_bytes;
      require(without_weak < with_weak && with_weak - without_weak >= first_bytes,
              "Evicted weak-pinned artifact did not retain its full durable charge until final weak release");
      std::cout << "SESSION weak_pin_replacement old_artifact_bytes=" << first_bytes
                << " resident_with_weak=" << with_weak << " resident_without_weak=" << without_weak << '\n';
      second.reset();
      auto reload = enqueue(session, request);
      require(wait(runtime, reload).status == oc::compiler_session::completion_status::loaded_from_disk,
              "Evicted artifact was not reloaded from the existing disk cache");
   } catch (...) {
      failure = std::current_exception();
   }
   forge::asio::blocking::run(runtime, session.shutdown(std::chrono::steady_clock::now() + 2s));
   if (failure)
      std::rethrow_exception(failure);
}

void quota(forge::asio::runtime& runtime, forge::asio::compute::pool& compute, oc::session_options configuration,
           const oc::compile_request& request, const std::vector<std::uint8_t>& encoded) {
   const auto measured_test_cap = [&] {
      auto code = oc::artifact::decode(encoded, oc::identify(request), configuration.max_response_bytes,
                                       oc::artifact::origin::isolated_compiler);
      const auto base = code.byte_size();
      code.prepare();
      return code.byte_size() - base;
   }();
   configuration.max_preparation_resident_bytes = measured_test_cap - 1;
   auto session = oc::compiler_session{runtime, compute.get_executor(), configuration};
   const auto base = session.usage();
   auto failure = std::exception_ptr{};
   try {
      forge::asio::blocking::run(runtime, session.initialize(oc::target_profile::native(), {}));
      for (unsigned attempt = 0; attempt != 2; ++attempt) {
         auto ticket = enqueue(session, request);
         rejects<oc::exceptions::compile_limit>([&] { static_cast<void>(wait(runtime, ticket)); });
         require(!session.ready(oc::identify(request), request.configuration), "Quota failure published ready code");
      }
      auto small = request;
      small.wasm = {0, 0x61, 0x73, 0x6d, 1,   0,   0,   0,   1, 5, 1,  0x60, 0, 1, 0x7f, 3,    2,  1,   0,
                    7, 8,    1,    4,    'p', 'i', 'n', 'g', 0, 0, 10, 6,    1, 4, 0,    0x41, 42, 0x0b};
      auto recovery = enqueue(session, small);
      require(wait(runtime, recovery).code.is_prepared(),
              "Tight per-load quota rejection poisoned smaller-request recovery");
   } catch (...) {
      failure = std::current_exception();
   }
   forge::asio::blocking::run(runtime, session.shutdown(std::chrono::steady_clock::now() + 2s));
   require(session.usage().resident_bytes == base.resident_bytes &&
               session.usage().inflight_bytes == base.inflight_bytes,
           "Quota failure/retry leaked a reservation");
   if (failure)
      std::rethrow_exception(failure);
}

void distinct_parallel(forge::asio::runtime& runtime, forge::asio::compute::pool& compute,
                       oc::session_options configuration, const oc::compile_request& request) {
   configuration.worker_count = 2;
   std::shared_ptr<forge::vm::wasm::interpret::prepared_module> modules[2];
   {
      auto session = oc::compiler_session{runtime, compute.get_executor(), configuration};
      auto failure = std::exception_ptr{};
      try {
         forge::asio::blocking::run(runtime, session.initialize(oc::target_profile::native(), {}));
         auto a = request;
         auto b = request;
         a.wasm.insert(a.wasm.end(), {0, 2, 1, 'p'});
         b.wasm.insert(b.wasm.end(), {0, 2, 1, 'q'});
         auto queued = forge::asio::blocking::run(
             runtime, enqueue_queued(session, configuration, std::array<const oc::compile_request*, 2>{&a, &b}));
         auto first = std::move(queued[0]);
         auto second = std::move(queued[1]);
         auto one = std::optional{wait(runtime, first)};
         modules[0] = one->code.runtime_module();
         one.reset(); // External module pin stays live during the other load.
         auto two = std::optional{wait(runtime, second)};
         modules[1] = two->code.runtime_module();
         two.reset();
         require(modules[0]->native_code().data() != modules[1]->native_code().data(),
                 "Distinct concurrent requests unexpectedly aliased native images");
         require(session.ready(oc::identify(a), a.configuration).has_value() &&
                     session.ready(oc::identify(b), b.configuration).has_value(),
                 "Concurrent preparation evicted a pinned successful request");
      } catch (...) {
         failure = std::current_exception();
      }
      forge::asio::blocking::run(runtime, session.shutdown(std::chrono::steady_clock::now() + 2s));
      if (failure)
         std::rethrow_exception(failure);
   }
   for (const auto& module : modules)
      require(module && !module->native_code().empty(), "Session destruction invalidated external module storage");
}

void stopped_job(forge::asio::runtime& runtime, forge::asio::compute::pool& compute, oc::session_options configuration,
                 const oc::compile_request& request) {
   auto session = oc::compiler_session{runtime, compute.get_executor(), configuration};
   const auto base = session.usage();
   auto failure = std::exception_ptr{};
   try {
      forge::asio::blocking::run(runtime, session.initialize(oc::target_profile::native(), {}));
      auto pending = request;
      pending.wasm.insert(pending.wasm.end(), {0, 2, 1, 'c'});
      auto ticket = enqueue(session, pending);
      session.request_stop();
      rejects<oc::exceptions::cancelled>([&] { static_cast<void>(wait(runtime, ticket)); });
      require(!session.ready(oc::identify(pending), pending.configuration), "Stopped job published ready code");
   } catch (...) {
      failure = std::current_exception();
   }
   forge::asio::blocking::run(runtime, session.shutdown(std::chrono::steady_clock::now() + 2s));
   require(session.usage().resident_bytes == base.resident_bytes &&
               session.usage().inflight_bytes == base.inflight_bytes,
           "Stopped job retained peak/resident reservation after drain");
   if (failure)
      std::rethrow_exception(failure);
}

void many_tickets(forge::asio::runtime& runtime, forge::asio::compute::pool& compute, oc::session_options configuration,
                  const oc::compile_request& request) {
   configuration.max_tickets = 1024;
   auto session = oc::compiler_session{runtime, compute.get_executor(), configuration};
   const auto base = session.usage();
   auto failure = std::exception_ptr{};
   auto actual = std::uint64_t{};
   try {
      forge::asio::blocking::run(runtime, session.initialize(oc::target_profile::native(), {}));
      auto admitted = enqueue(session, request);
      auto result = wait(runtime, admitted);
      actual = result.code.byte_size();
      require(result.code.is_prepared() && actual > configuration.max_resident_bytes / configuration.max_tickets,
              "Large ticket-count fixture did not exceed the retired per-ticket ceiling");
      require(session.ready(oc::identify(request), request.configuration).has_value(),
              "Large ticket count refused ordinary prepared-session reuse");
   } catch (...) {
      failure = std::current_exception();
   }
   forge::asio::blocking::run(runtime, session.shutdown(std::chrono::steady_clock::now() + 2s));
   const auto drained = session.usage();
   require(drained.resident_bytes == base.resident_bytes && drained.inflight_bytes == base.inflight_bytes,
           "Large ticket-count session retained an allocation/reservation after drain");
   if (failure)
      std::rethrow_exception(failure);
   std::cout << "PASS default worker share with max_tickets=1024; actual_artifact_bytes=" << actual << '\n';
}
} // namespace

#if defined(FORGE_WAMR_SESSION_LATE_SPAWN_TEST)
int session_late_spawn_cleanup(int argc, char** argv);
int session_workspace_refusal(int argc, char** argv);
int session_deadline_context(int argc, char** argv);
int session_second_socketpair_refusal(int argc, char** argv);
#endif

int main(int argc, char** argv) {
#if defined(FORGE_WAMR_SESSION_LATE_SPAWN_TEST)
   if (argc > 1 && std::string_view{argv[1]} == "late-spawn-cleanup")
      return session_late_spawn_cleanup(argc, argv);
   if (argc > 1 && std::string_view{argv[1]} == "workspace-refusal")
      return session_workspace_refusal(argc, argv);
   if (argc > 1 && std::string_view{argv[1]} == "deadline-context")
      return session_deadline_context(argc, argv);
   if (argc > 1 && std::string_view{argv[1]} == "second-socketpair-refusal")
      return session_second_socketpair_refusal(argc, argv);
#endif
   try {
      if (argc != 7)
         return 2;
      helper_rejection_control();
      auto address_bytes = std::uint64_t{};
      const auto text = std::string{argv[5]};
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), address_bytes);
      require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && address_bytes,
              "An explicit finite diagnostic helper address-space limit is required");
      const auto encoded_request = read(argv[2]);
      const auto encoded_artifact = read(argv[3]);
      constexpr auto message_cap = 1024 * 1024;
      require(encoded_request.size() <= message_cap && encoded_artifact.size() <= message_cap,
              "Fresh fixture exceeds the consumer-shaped 1 MiB request/response caps");
      const auto request = oc::compile_request::decode(encoded_request, message_cap);
      std::cout << "SESSION fixture_request_bytes=" << encoded_request.size()
                << " fixture_artifact_bytes=" << encoded_artifact.size() << " message_cap=" << message_cap << '\n';
      auto multi_result = request;
      multi_result.wasm = read(argv[6]);
      const auto identity = oc::identify(request);
      auto pattern = (std::filesystem::path{argv[4]} / "session-bounded-XXXXXX").string();
      require(::mkdtemp(pattern.data()) != nullptr, "Cannot create owned test cache");
      const auto cache = std::filesystem::path{pattern};
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
      auto compute = forge::asio::compute::pool{forge::asio::compute::pool::options{
          .worker_threads = 2, .max_pending_tasks = 8, .max_waiting_submissions = 8}};
      auto failure = std::exception_ptr{};
      try {
         const auto configuration = options(argv[1], cache, address_bytes);
         admission_saturation(runtime, compute, configuration, request);
         exercise(runtime, compute, configuration, request, multi_result,
                  oc::compiler_session::completion_status::compiled);
         const auto cached = cache / (identity.str() + ".foc");
         require(std::filesystem::exists(cached), "Prepared artifact did not reach disk cache");
         exercise(runtime, compute, configuration, request, multi_result,
                  oc::compiler_session::completion_status::loaded_from_disk);
         many_tickets(runtime, compute, configuration, request);
         quota(runtime, compute, configuration, request, encoded_artifact);
         stopped_job(runtime, compute, configuration, request);
         distinct_parallel(runtime, compute, configuration, request);
         auto corrupt = read(cached);
         corrupt.front() ^= 0x80;
         write(cached, corrupt);
         const auto orphan = cache / "preserved-orphan";
         const auto opaque = std::vector<std::uint8_t>{'o', 'w', 'n', 'e', 'r'};
         write(orphan, opaque);
         exercise(runtime, compute, configuration, request, multi_result,
                  oc::compiler_session::completion_status::compiled);
         require(read(cached) != corrupt && read(orphan) == opaque,
                 "Restart failed replacement or deleted opaque owner bytes");
         eviction(runtime, compute, configuration, request);
         for (const auto& entry : std::filesystem::directory_iterator{cache}) {
            const auto name = entry.path().filename().string();
            require(name == "owner.lock" || name == "preserved-orphan" || entry.path().extension() == ".foc",
                    "Session leaked a helper/cache temporary");
         }
      } catch (...) {
         failure = std::current_exception();
      }
      forge::asio::blocking::run(runtime, compute.shutdown());
      if (failure)
         std::rethrow_exception(failure);
      std::cout << "PASS real-helper cold/warm/reload/eviction, parallel/cancel/reuse, quota/tamper/restart, "
                   "shutdown/weak refund; cache="
                << cache << '\n';
      return 0;
   } catch (const std::exception& error) {
      std::cerr << "FAIL " << error.what() << '\n';
      return 1;
   } catch (...) {
      std::cerr << "FAIL unexpected typed error\n";
      return 1;
   }
}
