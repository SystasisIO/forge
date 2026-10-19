// Standalone test executable: allocation replacements must not be linked into other tests.
// Probe pattern: quic_p2p/control/pubsub_control_allocation.cpp; runtime pattern: asio/compute_tests.cpp.
#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/scope/scope_exit.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <span>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

import forge.asio.blocking;
import forge.asio.compute;
import forge.asio.notification;
import forge.asio.runtime;
import forge.asio.task;
import forge.auth.attestation.document_schema;
import forge.codec.hex;
import forge.codec.json;
import forge.crypto.asymmetric.p256;
import forge.crypto.digest.sha256;
import forge.crypto.jose.codec;
import forge.variant.value;

namespace allocation_probe {
thread_local bool armed = false;
thread_local std::uint64_t calls = 0, bytes = 0;
void* allocate(std::size_t size, std::size_t alignment = 0) {
   void* result = nullptr;
   if (alignment == 0) result = std::malloc(size == 0 ? 1 : size);
   else if (posix_memalign(&result, std::max(alignment, sizeof(void*)), size == 0 ? 1 : size) != 0) result = nullptr;
   if (!result) throw std::bad_alloc{};
   if (armed) { ++calls; bytes += size; }
   return result;
}
struct scope {
   scope() noexcept { calls = bytes = 0; armed = true; }
   ~scope() { armed = false; }
   scope(const scope&) = delete;
   scope& operator=(const scope&) = delete;
};
} // namespace allocation_probe

void* operator new(std::size_t size) { return allocation_probe::allocate(size); }
void* operator new[](std::size_t size) { return allocation_probe::allocate(size); }
void* operator new(std::size_t size, std::align_val_t alignment) { return allocation_probe::allocate(size, std::size_t(alignment)); }
void* operator new[](std::size_t size, std::align_val_t alignment) { return allocation_probe::allocate(size, std::size_t(alignment)); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { try { return allocation_probe::allocate(size); } catch (...) { return nullptr; } }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { try { return allocation_probe::allocate(size); } catch (...) { return nullptr; } }
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept { try { return allocation_probe::allocate(size, std::size_t(alignment)); } catch (...) { return nullptr; } }
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept { try { return allocation_probe::allocate(size, std::size_t(alignment)); } catch (...) { return nullptr; } }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
void operator delete(void* value, std::align_val_t) noexcept { std::free(value); }
void operator delete[](void* value, std::align_val_t) noexcept { std::free(value); }
void operator delete(void* value, std::size_t, std::align_val_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t, std::align_val_t) noexcept { std::free(value); }
void operator delete(void* value, const std::nothrow_t&) noexcept { std::free(value); }
void operator delete[](void* value, const std::nothrow_t&) noexcept { std::free(value); }
void operator delete(void* value, std::align_val_t, const std::nothrow_t&) noexcept { std::free(value); }
void operator delete[](void* value, std::align_val_t, const std::nothrow_t&) noexcept { std::free(value); }

namespace {
namespace att = forge::auth::attestation;
namespace jose = forge::crypto::jose;
namespace json = forge::codec::json;
namespace compute = forge::asio::compute;
namespace task = forge::asio::task;
using clock_type = std::chrono::steady_clock;
using object = forge::mutable_variant_object;
using document = att::document<forge::variant_object>;
constexpr std::size_t warmup_iterations = 16;
struct options { std::size_t iterations = 1000, workers = 2, inflight = 4; bool fail_worker = false; };
struct invalid_arguments {};
struct injected_worker_failure {};
options parse_options(int argc, char** argv) {
   options result;
   unsigned seen = 0;
   for (int i = 1; i < argc; i += 2) {
      if (i + 1 >= argc) throw invalid_arguments{};
      const auto name = std::string_view{argv[i]}, text = std::string_view{argv[i + 1]};
      std::size_t value = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0) throw invalid_arguments{};
      const unsigned bit = name == "--iterations" ? 1 : name == "--workers" ? 2 : name == "--inflight" ? 4 :
                           name == "--fail-worker" ? 8 : 0;
      if (bit == 0 || (seen & bit) != 0) throw invalid_arguments{};
      seen |= bit;
      if (bit == 1) result.iterations = value;
      if (bit == 2) result.workers = value;
      if (bit == 4) result.inflight = value;
      if (bit == 8) {
         if (value != 1) throw invalid_arguments{};
         result.fail_worker = true;
      }
   }
   if (result.iterations > 100'000 || result.workers > 8 || result.inflight > 64) throw invalid_arguments{};
   return result;
}
att::verification_policy profile() {
   return {.issuer = "urn:benchmark:issuer", .audience = "urn:benchmark:resource", .purpose = "fixture.read",
           .schema = "urn:benchmark:grant:v1", .max_lifetime = std::chrono::seconds{300}};
}
jose::trusted_key public_key() {
   const auto point = forge::codec::hex::decode(
       "046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
       "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
   jose::trusted_key key{.kid = "benchmark-key"};
   std::copy(point.begin(), point.end(), key.public_key.begin());
   return key;
}
struct fixture { std::string name, payload, compact; std::size_t ranges, providers; };
fixture make_fixture(std::string name, std::size_t range_count, std::size_t provider_count) {
   forge::variants ranges, providers;
   for (std::size_t i = 0; i < range_count; ++i)
      ranges.emplace_back(object{}("offset", std::to_string(9'007'199'254'740'993ULL + i * 1024))("length", "1024"));
   for (std::size_t i = 0; i < provider_count; ++i) providers.emplace_back("provider-" + std::to_string(i));
   const auto claims = object{}("delegation", object{}("id", "delegation-1")("generation", "18446744073709551615"))
       ("resource", object{}("object", "object-1")("revision", "revision-1")("ranges", ranges)("providers", providers));
   const auto expected = profile();
   const auto value = document{.iss = expected.issuer, .aud = expected.audience, .jti = "g_" + std::string(64, 'a'),
       .iat = 1'800'000'000, .nbf = 1'800'000'000, .exp = 1'800'000'300,
       .att = {.purpose = expected.purpose, .schema = expected.schema, .claims = claims}};
   auto encoded = json::write(value, {.max_bytes = att::max_payload_bytes});
   if (!encoded.ok()) throw std::runtime_error{"fixture encoding failed"};
   auto prepared = jose::prepare(encoded.text, public_key(), {.typ = std::string{att::token_type}});
   const auto disposable = forge::crypto::asymmetric::p256::private_key::regenerate(
       forge::crypto::digest::sha256{std::string(63, '0') + '1'});
   const auto signature = disposable.sign_compact(prepared.signing_digest());
   return {std::move(name), std::move(encoded.text),
           prepared.complete(std::span<const std::uint8_t>{signature}.subspan(1)), range_count, provider_count};
}
std::uint64_t parse_profile(std::string_view payload, const att::verification_policy& expected) {
   const auto parsed = json::read<document>(payload, {.max_depth = att::max_json_depth,
       .unknown_fields = json::unknown_field_policy::error, .described_records = json::described_record_policy::exact,
       .max_bytes = att::max_payload_bytes, .max_nodes = att::max_json_nodes,
       .max_container_size = att::max_container_size, .max_string_bytes = att::max_string_bytes, .reject_nul = true});
   if (!parsed.ok()) throw std::runtime_error{"fixture parse failed"};
   parsed.value.validate(expected, std::chrono::sys_seconds{std::chrono::seconds{1'800'000'100}});
   return parsed.value.jti.size() + parsed.value.att.claims.size();
}
std::uint64_t verify_parse(const fixture& input, const jose::trusted_keys& keys, const att::verification_policy& expected) {
   const auto verified = jose::verifier{}.verify(input.compact, keys);
   return parse_profile(verified.payload(), expected);
}
std::uint64_t elapsed_ns(clock_type::time_point start) {
   const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now() - start).count();
   return value > 0 ? static_cast<std::uint64_t>(value) : 0;
}
struct sample { std::uint64_t ns, calls, bytes, consumed; };
sample measure(auto&& work) {
   const auto start = clock_type::now();
   sample result{};
   {
      allocation_probe::scope count;
      result.consumed = work();
      result.calls = allocation_probe::calls; result.bytes = allocation_probe::bytes;
   }
   result.ns = elapsed_ns(start);
   return result;
}
struct series {
   std::vector<std::uint64_t> times;
   std::uint64_t calls = 0, bytes = 0, consumed = 0, total_ns = 0;
   explicit series(std::size_t capacity) { times.reserve(capacity); }
   void add(sample value) {
      times.push_back(value.ns); calls += value.calls; bytes += value.bytes;
      consumed += value.consumed; total_ns += value.ns;
   }
   forge::variant summary() {
      std::ranges::sort(times);
      const auto n = times.size();
      return object{}("samples", std::uint64_t(n))("latency_ns", object{}("min", times.front())
          ("p50", times[(n * 50 + 99) / 100 - 1])("p95", times[(n * 95 + 99) / 100 - 1])
          ("max", times.back())("mean", double(total_ns) / n))
          ("cpp_new_calls_total", calls)("cpp_new_calls_mean", double(calls) / n)
          ("cpp_requested_bytes_total", bytes)("cpp_requested_bytes_mean", double(bytes) / n)
          ("consumed_size_checksum", consumed);
   }
};
forge::variant sequential(const fixture& input, const jose::trusted_keys& keys,
                          const att::verification_policy& expected, std::size_t count) {
   series parse{count}, verify{count}, combined{count};
   const auto verify_only = [&] {
      const auto proof = jose::verifier{}.verify(input.compact, keys);
      return proof.payload().size() + proof.key_binding().identity.size();
   };
   for (std::size_t i = 0; i < warmup_iterations; ++i) {
      static_cast<void>(parse_profile(input.payload, expected)); static_cast<void>(verify_only());
      static_cast<void>(verify_parse(input, keys, expected));
   }
   for (std::size_t i = 0; i < count; ++i) {
      parse.add(measure([&] { return parse_profile(input.payload, expected); }));
      verify.add(measure(verify_only)); combined.add(measure([&] { return verify_parse(input, keys, expected); }));
   }
   return object{}("shape", input.name)("range_count", std::uint64_t(input.ranges))
       ("provider_count", std::uint64_t(input.providers))("payload_bytes", std::uint64_t(input.payload.size()))
       ("compact_bytes", std::uint64_t(input.compact.size()))("parse_profile", parse.summary())
       ("jose_verify", verify.summary())("verify_parse_profile", combined.summary());
}
struct heartbeat_state {
   std::atomic_bool done{false};
   std::atomic_size_t active_compute{0};
   forge::asio::notification started;
   std::uint64_t ticks = 0, ticks_during_compute = 0, max_lateness_ns = 0;
};
boost::asio::awaitable<void> heartbeat_loop(task::context& context, std::shared_ptr<heartbeat_state> state) {
   boost::asio::steady_timer timer{co_await boost::asio::this_coro::executor};
   while (!state->done.load()) {
      context.throw_if_cancel_requested();
      const auto deadline = clock_type::now() + std::chrono::milliseconds{1};
      timer.expires_at(deadline);
      co_await timer.async_wait(boost::asio::use_awaitable);
      if (state->done.load()) break;
      if (++state->ticks == 1) state->started.notify();
      if (state->active_compute.load() != 0) ++state->ticks_during_compute;
      state->max_lateness_ns = std::max(state->max_lateness_ns, elapsed_ns(deadline));
   }
}
boost::asio::awaitable<forge::variant> load(compute::pool& pool, task::scheduler& scheduler,
    const std::array<fixture, 2>& fixtures, const jose::trusted_keys& keys,
    const att::verification_policy& expected, options settings) {
   std::array<series, 2> samples{series{settings.iterations}, series{settings.iterations}};
   auto beat = std::make_shared<heartbeat_state>();
   // request_stop() cannot retire an already running heartbeat; every exit must signal it.
   const auto stop_heartbeat = boost::scope::scope_exit{[beat]() noexcept { beat->done.store(true); }};
   const auto started_epoch = beat->started.epoch();
   auto heartbeat = scheduler.submit(task::awaitable{.name = "attestation-benchmark-heartbeat",
       .work = [beat](task::context& context) { return heartbeat_loop(context, beat); }});
   if (!heartbeat.accepted()) throw std::runtime_error{"heartbeat admission failed"};
   if (settings.fail_worker) {
      const auto observed = co_await beat->started.async_wait_until(
          started_epoch, clock_type::now() + std::chrono::seconds{1});
      if (observed == started_epoch) throw std::runtime_error{"heartbeat startup timed out"};
   }
   const auto executor = pool.get_executor();
   const auto start = clock_type::now();
   const auto total = settings.iterations * fixtures.size();
   for (std::size_t first = 0; first < total; first += settings.inflight) {
      std::vector<compute::operation<sample>> operations;
      const auto count = std::min(settings.inflight, total - first);
      operations.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
         const auto* input = &fixtures[(first + i) % fixtures.size()];
         const auto inject = settings.fail_worker && first == 0 && i == 0;
         operations.push_back(co_await executor.submit({.name = "attestation-benchmark-verify"}, [input, &keys, &expected, beat, inject] {
            beat->active_compute.fetch_add(1);
            const auto finish = boost::scope::scope_exit{[beat] { beat->active_compute.fetch_sub(1); }};
            if (inject) throw injected_worker_failure{};
            return measure([&] { return verify_parse(*input, keys, expected); });
         }));
      }
      for (std::size_t i = 0; i < count; ++i)
         samples[(first + i) % fixtures.size()].add(co_await std::move(operations[i]).wait());
   }
   const auto wall = elapsed_ns(start);
   beat->done.store(true);
   co_await heartbeat.wait();
   forge::variants shapes;
   for (std::size_t i = 0; i < fixtures.size(); ++i)
      shapes.emplace_back(object{}("shape", fixtures[i].name)("verify_parse_profile", samples[i].summary()));
   co_return object{}("operations", std::uint64_t(total))("wall_ns", wall)
       ("operations_per_second", double(total) * 1e9 / std::max<std::uint64_t>(wall, 1))
       ("worker_operation_samples", shapes)("heartbeat_period_ns", std::uint64_t{1'000'000})
       ("heartbeat_ticks", beat->ticks)("heartbeat_ticks_during_compute", beat->ticks_during_compute)
       ("heartbeat_overlap_observed", beat->ticks_during_compute != 0)
       ("heartbeat_max_lateness_ns", beat->ticks == 0 ? forge::variant{} : forge::variant{beat->max_lateness_ns});
}
forge::variant run(options settings) {
   const std::array<fixture, 2> fixtures{make_fixture("single_range", 1, 1), make_fixture("multi_range", 8, 4)};
   const auto keys = jose::trusted_keys{{"benchmark-domain", "benchmark-profile", 1}, {public_key()},
                                        {.typ = std::string{att::token_type}}};
   const auto expected = profile();
   forge::variants sequential_results;
   if (!settings.fail_worker)
      for (const auto& input : fixtures) sequential_results.push_back(sequential(input, keys, expected, settings.iterations));
   forge::asio::runtime runtime{{.worker_threads = 1}};
   compute::pool pool{{.worker_threads = settings.workers, .max_pending_tasks = settings.inflight,
                       .max_waiting_submissions = 1}};
   task::scheduler scheduler{runtime, {.max_blocking_tasks = 1, .max_awaitable_tasks = 1, .max_pending_tasks = 1}};
   forge::variant concurrent;
   try { concurrent = forge::asio::blocking::run(runtime, load(pool, scheduler, fixtures, keys, expected, settings)); }
   catch (...) {
      pool.request_stop(); scheduler.request_stop();
      forge::asio::blocking::run(runtime, pool.shutdown());
      forge::asio::blocking::run(runtime, scheduler.shutdown());
      throw;
   }
   forge::asio::blocking::run(runtime, pool.shutdown());
   forge::asio::blocking::run(runtime, scheduler.shutdown());
   const auto metrics = pool.snapshot();
   if (metrics.completed != settings.iterations * fixtures.size() || metrics.failed || metrics.canceled ||
       metrics.rejected || metrics.pending || metrics.running || metrics.waiting)
      throw std::runtime_error{"incomplete compute workload"};
   return object{}("schema_version", 1)("status", "MEASURED")
       ("measurement_scope", "Synthetic instrumented fixtures; sequential warmup only; no thresholds or universal claims")
       ("verification_cache", false)
       ("allocation_scope", "Successful replaceable C++ new/new[] including aligned/nothrow, measured calling-thread body only")
       ("backend_malloc_tracked", false)("executor_allocations_tracked", false)
       ("allocation_exclusions", "CJOSE/Jansson/OpenSSL malloc/calloc/realloc, executor, setup, warmup, samples and JSON output")
       ("latency_percentiles", "nearest-rank; nanoseconds include allocation-probe and timer overhead; object destruction included")
       ("iterations_per_shape", std::uint64_t(settings.iterations))
       ("sequential_warmup_per_shape", std::uint64_t(warmup_iterations))("compute_warmup_operations", 0)
       ("io_threads", 1)("compute_workers", std::uint64_t(settings.workers))("max_inflight", std::uint64_t(settings.inflight))
       ("compute_max_pending", std::uint64_t(settings.inflight))("compute_max_waiting", 1)
       ("sequential", sequential_results)("compute_load", concurrent)
       ("compute_metrics", object{}("submitted", metrics.submitted)("completed", metrics.completed)
           ("failed", metrics.failed)("rejected", metrics.rejected)("canceled", metrics.canceled)
           ("queue_ns_total", std::uint64_t(metrics.total_queue_time.count()))
           ("execution_ns_total", std::uint64_t(metrics.total_execution_time.count())));
}
} // namespace

int main(int argc, char** argv) {
   try {
      const auto metrics = json::write_value(run(parse_options(argc, argv)), {.max_bytes = 64 * 1024});
      if (!metrics.ok()) throw std::runtime_error{"metrics encoding failed"};
      std::cout << metrics.text << '\n';
      return 0;
   } catch (const invalid_arguments&) {
      std::cout << R"({"status":"HARNESS_ERROR","reason":"invalid_arguments","cli":"--iterations 1..100000 --workers 1..8 --inflight 1..64 [--fail-worker 1]"})" << '\n';
      return 2;
   } catch (const injected_worker_failure&) {
      std::cout << R"({"status":"HARNESS_ERROR","reason":"injected_worker_failure"})" << '\n';
      return 1;
   } catch (...) {
      std::cout << R"({"status":"HARNESS_ERROR","reason":"benchmark_failed"})" << '\n';
      return 1;
   }
}
