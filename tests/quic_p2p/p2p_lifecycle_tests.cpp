module;

#include <boost/test/unit_test.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>

#include <atomic>
#include <bit>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <ranges>
#include <regex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include "libp2p_identity_fixture.hxx"

module forge.net.p2p.node;

import :lifecycle_stop_listener;

import forge.asio.blocking;
import forge.asio.gate;
import forge.asio.runtime;
import forge.log.logger;
import forge.multiformats.varint;
import forge.multiformats.multiaddr;
import forge.net.p2p.dht;
import forge.net.p2p.diagnostics;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.identify;
import forge.net.p2p.lifecycle;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.relay;
import forge.net.p2p.topology;

#include "../../libraries/net/p2p/details/autorelay_manager.hxx"
#include "../../libraries/net/p2p/details/bootstrap_service.hxx"
#include "../../libraries/net/p2p/details/dht_provider_registry.hxx"
#include "../../libraries/net/p2p/details/identify_service.hxx"
#include "../../libraries/net/p2p/details/lifecycle_tracker.hxx"
#include "../../libraries/net/p2p/details/topology_manager.hxx"

namespace forge::net::p2p {
namespace {

class p2p_log_capture final : public forge::sink {
 public:
   void log(const forge::log_record& record) override {
      auto inspect = std::function<void()>{};
      {
         const auto lock = std::scoped_lock{_mutex};
         _records.push_back(record);
         inspect = _inspect;
      }
      if (inspect) { inspect(); }
   }
   void inspect(std::function<void()> value) {
      const auto lock = std::scoped_lock{_mutex};
      _inspect = std::move(value);
   }
   std::vector<forge::log_record> records() const {
      const auto lock = std::scoped_lock{_mutex};
      return _records;
   }
   std::size_t count(std::string_view event) const {
      const auto values = records();
      return std::ranges::count_if(values, [&](const auto& record) {
         return std::ranges::any_of(record.fields, [&](const auto& field) {
            return field.key == "event" && field.value == event;
         });
      });
   }

 private:
   mutable std::mutex _mutex;
   std::vector<forge::log_record> _records;
   std::function<void()> _inspect;
};

class p2p_log_routes {
 public:
   p2p_log_routes(std::shared_ptr<p2p_log_capture> capture, forge::log_level level,
                  std::initializer_list<std::string_view> names) {
      for (const auto name : names) {
         auto key = std::string{name};
         _previous.emplace_back(key, forge::logger::get(key));
         auto route = forge::logger{key};
         route.set_log_level(level);
         route.add_sink(capture);
         forge::logger::update(key, route);
      }
   }
   ~p2p_log_routes() {
      for (auto& [name, route] : _previous) { forge::logger::update(name, route); }
   }

 private:
   std::vector<std::pair<std::string, forge::logger>> _previous;
};

[[nodiscard]] node::options make_lifecycle_node_options(std::string name) {
   auto identity = forge::tests::p2p::make_identity_fixture(std::move(name));
   return node::options{
       .certificate_pem = std::move(identity.certificate_pem),
       .private_key_pem = std::move(identity.private_key_pem),
       .peer_state = {.persistence = peer_store::make_memory_persistence()},
   };
}

[[nodiscard]] endpoint unavailable_bootstrap(std::string name) {
   const auto identity = forge::tests::p2p::make_identity_fixture(std::move(name));
   const auto peer = make_peer_id_from_certificate_pem(identity.certificate_pem);
   return parse_endpoint("/ip4/127.0.0.1/tcp/1/p2p/" + peer.to_string());
}

template <typename Predicate> [[nodiscard]] bool eventually(Predicate&& predicate, std::chrono::milliseconds timeout) {
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   while (!predicate()) {
      if (std::chrono::steady_clock::now() >= deadline) {
         return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
   }
   return true;
}

[[nodiscard]] bool supports(const peer_store::record& record, const protocol_id& protocol) {
   return std::ranges::find(record.protocols, protocol) != record.protocols.end();
}

[[nodiscard]] std::vector<std::uint8_t> length_delimited(std::span<const std::uint8_t> payload) {
   auto out = forge::multiformats::varint_encode(payload.size());
   out.insert(out.end(), payload.begin(), payload.end());
   return out;
}

} // namespace

BOOST_AUTO_TEST_CASE(p2p_logging_native_lifecycle_transitions_are_sparse_unlocked_and_disableable) {
   auto capture = std::make_shared<p2p_log_capture>();
   auto routes = p2p_log_routes{capture, forge::log_level::info, {"forge.net.p2p"}};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto options = make_lifecycle_node_options("logging-lifecycle");
   options.lifecycle.bootstrap = {{.address = unavailable_bootstrap("logging-unavailable").to_multiaddr()}};
   options.lifecycle.maintenance_interval = std::chrono::milliseconds{20};
   options.lifecycle.bootstrap_retry_initial_delay = std::chrono::milliseconds{20};
   options.lifecycle.bootstrap_retry_max_delay = std::chrono::milliseconds{40};
   options.lifecycle.connect_timeout = std::chrono::milliseconds{20};
   auto value = node{runtime, std::move(options)};
   auto snapshots = std::atomic_size_t{};
   capture->inspect([&] { static_cast<void>(value.lifecycle_state()); ++snapshots; });
   const auto status = forge::asio::blocking::run(runtime, value.async_start());
   BOOST_TEST(status.degraded);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, value.async_start()), exceptions::invalid_options);
#ifndef FORGE_DISABLE_LOGGING
   BOOST_REQUIRE(eventually([&] { return capture->count("lifecycle.degraded") == 1U; }, std::chrono::seconds{2}));
#endif
   forge::asio::blocking::run(runtime, value.async_set_bootstrap({}));
#ifndef FORGE_DISABLE_LOGGING
   BOOST_REQUIRE(eventually([&] { return capture->count("lifecycle.recovered") == 1U; }, std::chrono::seconds{2}));
#endif
   forge::asio::blocking::run(runtime, value.async_stop());
   forge::asio::blocking::run(runtime, value.async_stop());
   BOOST_TEST(static_cast<int>(value.lifecycle_state().phase) == static_cast<int>(lifecycle_phase::stopped));
   capture->inspect({});
#ifndef FORGE_DISABLE_LOGGING
   BOOST_TEST(capture->count("lifecycle.ready") == 1U);
   BOOST_TEST(capture->count("lifecycle.stopped") == 1U);
   BOOST_TEST(snapshots.load() == 4U);
#else
   BOOST_TEST(capture->records().empty());
#endif
   auto disabled = forge::logger::get("forge.net.p2p");
   disabled.set_enabled(false);
   const auto recorded = capture->records().size();
   auto quiet = node{runtime, make_lifecycle_node_options("logging-disabled")};
   const auto quiet_status = forge::asio::blocking::run(runtime, quiet.async_start());
   BOOST_TEST(!quiet_status.degraded);
   forge::asio::blocking::run(runtime, quiet.async_stop());
   BOOST_TEST(capture->records().size() == recorded);
}

BOOST_AUTO_TEST_CASE(p2p_logging_bootstrap_repeated_failures_are_bounded_and_redacted) {
   auto capture = std::make_shared<p2p_log_capture>();
   auto routes = p2p_log_routes{capture, forge::log_level::debug, {"forge.net.p2p.discovery"}};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto options = lifecycle_options{};
   options.bootstrap = {{.address = unavailable_bootstrap("unsafe-address-sentinel").to_multiaddr()}};
   options.requirement = bootstrap_requirement::require_connection;
   options.startup_budget = std::chrono::milliseconds{120};
   options.bootstrap_retry_initial_delay = std::chrono::milliseconds{10};
   options.bootstrap_retry_max_delay = std::chrono::milliseconds{10};
   options.bootstrap_retry_jitter = 0.0;
   auto attempts = std::atomic_size_t{};
   auto service = std::make_shared<detail::bootstrap_service>(runtime.context().get_executor(), options,
       detail::bootstrap_service::callbacks{
           .connect = [&](bootstrap_peer, std::chrono::milliseconds, std::shared_ptr<cancellation_latch>)
               -> boost::asio::awaitable<peer_id> {
              ++attempts;
              throw std::runtime_error{"unsafe-remote-error-sentinel"};
              co_return peer_id{};
           },
           .connected = [](const bootstrap_peer&, const peer_id&) { return false; },
           .protect = [](const peer_id&) {}, .unprotect = [](const peer_id&) {},
           .prune_peer_state = []() -> boost::asio::awaitable<void> { co_return; },
       });
   capture->inspect([&] { static_cast<void>(service->last_failure()); });
   BOOST_TEST(forge::asio::blocking::run(runtime, service->async_initial_bootstrap()) == 0U);
   BOOST_TEST(service->last_failure() == "unsafe-remote-error-sentinel");
   const auto total = attempts.load();
   BOOST_REQUIRE(total >= 2U);
#ifndef FORGE_DISABLE_LOGGING
   BOOST_TEST(capture->count("bootstrap.connect_failed") == std::bit_width(total));
#else
   BOOST_TEST(capture->records().empty());
#endif
   for (const auto& record : capture->records()) {
      BOOST_TEST(record.level.value == forge::log_level::debug);
      const auto text = forge::format_json_log_record(record);
      BOOST_TEST(text.find("unsafe-remote-error-sentinel") == std::string::npos);
      BOOST_TEST(text.find("/ip4/") == std::string::npos);
   }
   service->request_stop();
   capture->inspect({});
}

BOOST_AUTO_TEST_CASE(p2p_logging_identify_emits_once_per_coalesced_attempt_without_document_content) {
   auto capture = std::make_shared<p2p_log_capture>();
   auto routes = p2p_log_routes{capture, forge::log_level::debug, {"forge.net.p2p.connection"}};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto service = detail::identify_service{runtime.context().get_executor()};
   capture->inspect([&] { static_cast<void>(service.retained()); });
   auto attempts = 0U;
   const auto run = [&]() -> boost::asio::awaitable<identify::document> {
      ++attempts;
      co_return identify::document{.agent_version = "unsafe-agent-sentinel", .protocols = {builtins::ping}};
   };
   const auto first = forge::asio::blocking::run(runtime, service.async_identify(41, run));
   const auto repeated = forge::asio::blocking::run(runtime, service.async_identify(41, run));
   BOOST_TEST(attempts == 1U);
   BOOST_TEST(static_cast<int>(first.state) == static_cast<int>(identify::state::identified));
   BOOST_TEST(static_cast<int>(repeated.state) == static_cast<int>(first.state));
   const auto failed = forge::asio::blocking::run(runtime, service.async_identify(42,
       []() -> boost::asio::awaitable<identify::document> {
          throw std::runtime_error{"unsafe-identify-error-sentinel"};
          co_return identify::document{};
       }));
   BOOST_TEST(failed.error == "unsafe-identify-error-sentinel");
#ifndef FORGE_DISABLE_LOGGING
   BOOST_TEST(capture->count("identify.completed") == 2U);
#else
   BOOST_TEST(capture->records().empty());
#endif
   for (const auto& record : capture->records()) {
      const auto text = forge::format_json_log_record(record);
      BOOST_TEST(text.find("unsafe-agent-sentinel") == std::string::npos);
      BOOST_TEST(text.find("unsafe-identify-error-sentinel") == std::string::npos);
   }
   capture->inspect({});
}

BOOST_AUTO_TEST_CASE(p2p_logging_topology_worker_exit_warns_once_unless_stop_was_requested) {
   for (const auto requested_stop : {false, true}) {
      auto capture = std::make_shared<p2p_log_capture>();
      auto routes = p2p_log_routes{capture, forge::log_level::warn, {"forge.net.p2p.discovery"}};
      auto context = boost::asio::io_context{};
      auto lifecycle = detail::lifecycle_tracker{context.get_executor()};
      auto manager = std::shared_ptr<detail::topology_manager>{};
      auto clock_calls = 0U;
      manager = std::make_shared<detail::topology_manager>(topology::policy{}, detail::topology_manager::callbacks{},
          detail::topology_manager::clocks{.steady_now = [&] {
             if (++clock_calls == 2U) {
                if (requested_stop) {
                   manager->request_stop();
                   throw exceptions::canceled{"unsafe-topology-error-sentinel"};
                }
                throw std::runtime_error{"unsafe-topology-error-sentinel"};
             }
             return std::chrono::steady_clock::now();
          }});
      capture->inspect([&] { static_cast<void>(manager->current()); });
      manager->start(lifecycle);
      context.poll();
      BOOST_TEST(static_cast<int>(manager->current().lifecycle_phase) ==
                 static_cast<int>(detail::topology_manager::phase::stopped));
      auto joined = boost::asio::co_spawn(context, manager->async_join(), boost::asio::use_future);
      context.restart();
      context.poll();
      BOOST_REQUIRE(joined.wait_for(std::chrono::seconds{0}) == std::future_status::ready);
      BOOST_CHECK_NO_THROW(joined.get());
      manager->request_stop();
      manager->request_stop();
      capture->inspect({});
#ifndef FORGE_DISABLE_LOGGING
      BOOST_TEST(capture->count("topology.failed") == (requested_stop ? 0U : 1U));
      BOOST_TEST(capture->records().size() == (requested_stop ? 0U : 1U));
#else
      BOOST_TEST(capture->records().empty());
#endif
      for (const auto& record : capture->records()) {
         BOOST_TEST(record.level.value == forge::log_level::warn);
         BOOST_TEST(forge::format_json_log_record(record).find("unsafe-topology-error-sentinel") == std::string::npos);
      }
      lifecycle.request_stop();
   }
}

BOOST_AUTO_TEST_CASE(p2p_logging_autorelay_samples_crossed_thresholds_for_batched_failures) {
   auto capture = std::make_shared<p2p_log_capture>();
   auto routes = p2p_log_routes{capture, forge::log_level::debug, {"forge.net.p2p.relay"}};
   auto context = boost::asio::io_context{};
   auto lifecycle = detail::lifecycle_tracker{context.get_executor()};
   using manager = detail::autorelay_manager;
   auto state = manager::snapshot{.permitted = true, .generation = 1};
   for (const auto id : {1U, 2U, 3U}) {
      state.candidates.push_back({.peer = make_peer_id({.type = public_key::type::ed25519,
                                                       .data = std::vector<std::uint8_t>(32, id)}),
                                  .session_id = id});
   }
   auto now = manager::time_point{std::chrono::seconds{10'000}};
   auto policy = relay::policy{.target_reservations = 3, .max_candidates_per_refresh = 3,
                               .max_parallel_reservations = 3, .candidate_backoff = std::chrono::hours{1}};
   auto owner = std::make_shared<manager>(context.get_executor(), policy, manager::callbacks{
       .current = [&] { return state; },
       // Synchronous completion handlers queue on one strand before the next reap.
       .reserve = [](manager::candidate, std::uint64_t, std::shared_ptr<cancellation_latch>)
           -> boost::asio::awaitable<relay::reservation::info> {
          throw std::runtime_error{"unsafe-autorelay-error-sentinel"};
       },
       .now = [&] { return now; },
   });
   capture->inspect([&] { static_cast<void>(owner->stats()); });
   owner->start(lifecycle);
   for (auto batch = 1U; batch <= 6U; ++batch) {
      if (batch != 1U) {
         now += std::chrono::hours{1};
         owner->notify();
      }
      context.restart();
      context.poll();
      BOOST_TEST(owner->stats().failures == batch * 3U);
      BOOST_TEST(owner->stats().attempts == batch * 3U);
      BOOST_TEST(owner->stats().pending_reservations == 0U);
   }
   auto logged_failures = std::vector<std::string>{};
   for (const auto& record : capture->records()) {
      const auto event = std::ranges::find(record.fields, std::string{"event"}, &forge::log_field::key);
      if (event != record.fields.end() && event->value == "autorelay.reservation_failed") {
         const auto failures = std::ranges::find(record.fields, std::string{"failures"}, &forge::log_field::key);
         BOOST_REQUIRE(failures != record.fields.end());
         logged_failures.push_back(failures->value);
         BOOST_TEST(record.level.value == forge::log_level::debug);
      }
      BOOST_TEST(forge::format_json_log_record(record).find("unsafe-autorelay-error-sentinel") == std::string::npos);
   }
#ifndef FORGE_DISABLE_LOGGING
   BOOST_CHECK(logged_failures == (std::vector<std::string>{"3", "6", "9", "18"}));
   BOOST_TEST(logged_failures.size() <= std::bit_width(owner->stats().failures));
#else
   BOOST_TEST(capture->records().empty());
#endif
   const auto recorded = capture->records().size();
   auto disabled = forge::logger::get("forge.net.p2p.relay");
   disabled.set_enabled(false);
   for (auto batch = 0U; batch < 5U; ++batch) {
      now += std::chrono::hours{1};
      owner->notify();
      context.restart();
      context.poll();
   }
   BOOST_TEST(owner->stats().failures == 33U);
   BOOST_TEST(capture->records().size() == recorded);
   owner->request_stop();
   auto joined = boost::asio::co_spawn(context, owner->async_join(), boost::asio::use_future);
   context.restart();
   context.poll();
   BOOST_REQUIRE(joined.wait_for(std::chrono::seconds{0}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(joined.get());
   BOOST_TEST(!owner->stats().running);
   capture->inspect({});
   lifecycle.request_stop();
}

BOOST_AUTO_TEST_CASE(p2p_logging_provider_failed_removal_is_distinct_from_successful_retry) {
   auto capture = std::make_shared<p2p_log_capture>();
   auto routes = p2p_log_routes{capture, forge::log_level::debug, {"forge.net.p2p.discovery"}};
   auto context = boost::asio::io_context{};
   auto removal_attempts = 0U;
   auto durable_owned = true;
   using registry_type = detail::dht_provider_registry;
   auto registry = std::make_shared<registry_type>(registry_type::callbacks{
       .track = [] { return std::make_shared<int>(1); },
       .launch = [&](std::function<boost::asio::awaitable<void>()> task) {
         boost::asio::co_spawn(context, std::move(task), boost::asio::detached);
          return true;
       },
       .prepare = [](protocol_id, dht::key, registry_type::schedule)
           -> boost::asio::awaitable<registry_type::prepared_provider> {
          co_return registry_type::prepared_provider{.provider = {}, .stamped_at = std::chrono::steady_clock::now()};
       },
       .publish = [](protocol_id, dht::key, dht::peer, dht::query_options query)
           -> boost::asio::awaitable<std::size_t> { co_return query.quorum; },
       .remove = [&](protocol_id, dht::key) -> boost::asio::awaitable<void> {
          if (++removal_attempts == 1U) { throw std::runtime_error{"unsafe-provider-error-sentinel"}; }
          durable_owned = false;
          co_return;
       },
       .publication_limit = [](const protocol_id&) { return std::size_t{4}; },
   });
   registry->open_admission();
   auto acquired = boost::asio::co_spawn(context,
       registry->async_acquire(protocol_id{.value = "/unsafe-provider-protocol-sentinel"},
           dht::key{.bytes = {0xde, 0xad, 0xbe, 0xef}}, dht::query_options{.requested_count = 1, .quorum = 1},
           registry_type::schedule{.provider_ttl = std::chrono::seconds{90}, .address_ttl = std::chrono::seconds{60},
                                   .republish_interval = std::chrono::seconds{30}}), boost::asio::use_future);
   context.poll();
   BOOST_REQUIRE(acquired.wait_for(std::chrono::seconds{0}) == std::future_status::ready);
   auto lease = acquired.get();
   capture->inspect([&] { static_cast<void>(registry->active(lease)); });
   auto failed = boost::asio::co_spawn(context, registry->async_release(lease), boost::asio::use_future);
   context.restart();
   context.poll();
   BOOST_REQUIRE(failed.wait_for(std::chrono::seconds{0}) == std::future_status::ready);
   BOOST_CHECK_THROW(failed.get(), std::runtime_error);
   BOOST_TEST(durable_owned);
   BOOST_TEST(!registry->active(lease));
#ifndef FORGE_DISABLE_LOGGING
   BOOST_TEST(capture->count("dht.provider_removal_failed") == 1U);
   BOOST_TEST(capture->count("dht.provider_removed") == 0U);
#endif
   auto retried = boost::asio::co_spawn(context, registry->async_release(lease), boost::asio::use_future);
   context.restart();
   context.poll();
   BOOST_REQUIRE(retried.wait_for(std::chrono::seconds{0}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(retried.get());
   BOOST_TEST(removal_attempts == 2U);
   BOOST_TEST(!durable_owned);
#ifndef FORGE_DISABLE_LOGGING
   BOOST_TEST(capture->count("dht.provider_removal_failed") == 1U);
   BOOST_TEST(capture->count("dht.provider_removed") == 1U);
#else
   BOOST_TEST(capture->records().empty());
#endif
   for (const auto& record : capture->records()) {
      const auto event = std::ranges::find(record.fields, std::string{"event"}, &forge::log_field::key);
      if (event != record.fields.end() &&
          (event->value == "dht.provider_removal_failed" || event->value == "dht.provider_removed")) {
         const auto retained = std::ranges::find(record.fields, std::string{"retained_for_retry"}, &forge::log_field::key);
         BOOST_REQUIRE(retained != record.fields.end());
         BOOST_TEST(retained->value == (event->value == "dht.provider_removal_failed" ? "true" : "false"));
         BOOST_TEST(record.level.value == forge::log_level::debug);
      }
      const auto text = forge::format_json_log_record(record);
      BOOST_TEST(text.find("unsafe-provider-error-sentinel") == std::string::npos);
      BOOST_TEST(text.find("unsafe-provider-protocol-sentinel") == std::string::npos);
      BOOST_TEST(text.find("deadbeef") == std::string::npos);
   }
   registry->seal();
   auto drained = boost::asio::co_spawn(context, registry->async_drain(), boost::asio::use_future);
   context.restart();
   context.poll();
   BOOST_REQUIRE(drained.wait_for(std::chrono::seconds{0}) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(drained.get());
   capture->inspect({});
}

BOOST_AUTO_TEST_CASE(p2p_logging_source_sites_use_constant_routes_and_never_serialize_payloads_or_errors) {
   const auto root = std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path();
   const auto site = std::regex{R"(\b[tdiwe]log\s*\()"};
   const auto route = std::regex{R"route(^[tdiwe]log\s*\(\s*"forge\.(net\.p2p(?:\.(?:connection|discovery|relay|pubsub))?|plugins\.net\.p2p\.node)")route"};
   auto sites = 0U;
   for (const auto& owner : {root / "libraries/net/p2p", root / "plugins/net/p2p/node"}) {
      for (const auto& entry : std::filesystem::directory_iterator{owner}) {
         if (entry.path().extension() != ".cpp") { continue; }
         auto input = std::ifstream{entry.path()};
         BOOST_REQUIRE(input.good());
         const auto text = std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
         BOOST_TEST(text.find("capture_and_log") == std::string::npos);
         for (auto it = std::sregex_iterator{text.begin(), text.end(), site}; it != std::sregex_iterator{}; ++it) {
            const auto start = static_cast<std::size_t>(it->position());
            const auto end = text.find(';', start);
            BOOST_REQUIRE(end != std::string::npos);
            const auto call = text.substr(start, end - start);
            BOOST_TEST(std::regex_search(call, route));
            BOOST_TEST(call.find("(\"event\",") != std::string::npos);
            if (call.find("\"topology.failed\"") != std::string::npos ||
                call.find("\"topology.stopped_unexpectedly\"") != std::string::npos) {
               BOOST_TEST(call.starts_with("wlog"));
            }
            for (const auto unsafe : {".what(", "(\"topic\",", "(\"payload\",", "(\"data\",",
                                      "(\"key\",", "(\"error\",", "(\"document\",", "(\"psk\",",
                                      "private_key", "certificate_pem", "status_text", "signed_peer_record"}) {
               BOOST_TEST(call.find(unsafe) == std::string::npos);
            }
            ++sites;
         }
      }
   }
   BOOST_TEST(sites > 0U);
}

BOOST_AUTO_TEST_CASE(p2p_lifecycle_stop_is_latched_before_task_cancellation_handler_installation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto tracker = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(tracker.begin_start());

   auto operation = tracker.track();
   BOOST_REQUIRE(operation.active());
   const auto stop_source = operation.stop_source();

   // Request stop before any cancellation handler is installed. The durable
   // latch is the task-entry guard for this exact registration race.
   tracker.request_stop();
   BOOST_REQUIRE(stop_source);
   BOOST_TEST(stop_source->stop_requested());
   operation.release();
   forge::asio::blocking::run(runtime, tracker.wait());
}

BOOST_AUTO_TEST_CASE(p2p_lifecycle_stop_latch_is_sticky_and_waits_for_operation_release) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto tracker = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(tracker.begin_start());

   auto operation = tracker.track();
   BOOST_REQUIRE(operation.active());
   const auto stop_source = operation.stop_source();

   tracker.request_stop();
   BOOST_REQUIRE(stop_source);
   BOOST_TEST(stop_source->stop_requested());

   auto waiting = boost::asio::co_spawn(runtime.context(), tracker.wait(), boost::asio::use_future);
   BOOST_CHECK(waiting.wait_for(std::chrono::milliseconds{25}) == std::future_status::timeout);
   operation.release();
   BOOST_CHECK(waiting.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   waiting.get();
}

BOOST_AUTO_TEST_CASE(p2p_node_optional_bootstrap_reports_degraded_and_keeps_running) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto options = make_lifecycle_node_options("lifecycle-optional");
   options.lifecycle.bootstrap = {{.address = unavailable_bootstrap("lifecycle-optional-bootstrap").to_multiaddr()}};
   options.lifecycle.startup_budget = std::chrono::milliseconds{100};
   options.lifecycle.connect_timeout = std::chrono::milliseconds{25};
   auto value = node{runtime, std::move(options)};

   const auto status = forge::asio::blocking::run(runtime, value.async_start());
   BOOST_TEST(static_cast<int>(status.phase) == static_cast<int>(lifecycle_phase::maintenance));
   BOOST_TEST(status.configured_bootstrap == 1U);
   BOOST_TEST(status.connected_bootstrap == 0U);
   BOOST_TEST(status.degraded);
   BOOST_TEST(!status.last_bootstrap_failure.empty());
   BOOST_TEST(value.lifecycle_state().last_bootstrap_failure == status.last_bootstrap_failure);

   forge::asio::blocking::run(runtime, value.async_stop());
   BOOST_TEST(static_cast<int>(value.lifecycle_state().phase) == static_cast<int>(lifecycle_phase::stopped));
}

BOOST_AUTO_TEST_CASE(p2p_node_strict_bootstrap_retries_until_shared_startup_budget) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto options = make_lifecycle_node_options("lifecycle-strict-failure");
   options.lifecycle.bootstrap = {{.address = unavailable_bootstrap("lifecycle-strict-unavailable").to_multiaddr()}};
   options.lifecycle.requirement = bootstrap_requirement::require_connection;
   options.lifecycle.startup_budget = std::chrono::milliseconds{180};
   options.lifecycle.connect_timeout = std::chrono::milliseconds{25};
   options.lifecycle.bootstrap_retry_initial_delay = std::chrono::milliseconds{20};
   options.lifecycle.bootstrap_retry_max_delay = std::chrono::milliseconds{40};
   options.lifecycle.bootstrap_retry_jitter = 0.0;
   auto value = node{runtime, std::move(options)};

   const auto started = std::chrono::steady_clock::now();
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, value.async_start()), exceptions::timeout);
   const auto elapsed = std::chrono::steady_clock::now() - started;

   BOOST_TEST(elapsed >= std::chrono::milliseconds{140});
   BOOST_TEST(elapsed < std::chrono::seconds{2});
   BOOST_TEST(static_cast<int>(value.lifecycle_state().phase) == static_cast<int>(lifecycle_phase::stopped));
}

BOOST_AUTO_TEST_CASE(p2p_node_stop_cancels_active_bootstrap_dial) {
   namespace asio = boost::asio;
   using tcp = asio::ip::tcp;

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto acceptor = std::make_shared<tcp::acceptor>(runtime.context(), tcp::endpoint{tcp::v4(), 0});
   acceptor->listen();
   auto socket = std::make_shared<tcp::socket>(runtime.context());
   auto accepted = std::make_shared<std::promise<void>>();
   auto accepted_future = accepted->get_future();
   asio::co_spawn(
       runtime.context(),
       [acceptor, socket, accepted]() -> asio::awaitable<void> {
          auto error = boost::system::error_code{};
          co_await acceptor->async_accept(*socket, asio::redirect_error(asio::use_awaitable, error));
          if (!error) {
             accepted->set_value();
          }
       },
       asio::detached);

   const auto bootstrap_identity = forge::tests::p2p::make_identity_fixture("lifecycle-active-dial-peer");
   const auto bootstrap_peer = make_peer_id_from_certificate_pem(bootstrap_identity.certificate_pem);
   auto options = make_lifecycle_node_options("lifecycle-active-dial-node");
   options.lifecycle.bootstrap = {
       {.address = parse_endpoint("/ip4/127.0.0.1/tcp/" + std::to_string(acceptor->local_endpoint().port()) + "/p2p/" +
                                  bootstrap_peer.to_string()).to_multiaddr()}};
   options.lifecycle.requirement = bootstrap_requirement::require_connection;
   options.lifecycle.startup_budget = std::chrono::seconds{5};
   options.lifecycle.connect_timeout = std::chrono::seconds{5};
   auto value = node{runtime, std::move(options)};

   auto startup = asio::co_spawn(runtime.context(), value.async_start(), asio::use_future);
   BOOST_REQUIRE(accepted_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready);

   const auto started = std::chrono::steady_clock::now();
   forge::asio::blocking::run(runtime, value.async_stop());
   const auto elapsed =
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
   BOOST_TEST(elapsed.count() < 750);
   BOOST_REQUIRE(startup.wait_for(std::chrono::milliseconds{100}) == std::future_status::ready);
   auto startup_failed = false;
   try {
      static_cast<void>(startup.get());
   } catch (const std::exception&) {
      startup_failed = true;
   }
   BOOST_TEST(startup_failed);
   BOOST_TEST(static_cast<int>(value.lifecycle_state().phase) == static_cast<int>(lifecycle_phase::stopped));

   auto ignored = boost::system::error_code{};
   socket->close(ignored);
   acceptor->close(ignored);
}

BOOST_AUTO_TEST_CASE(p2p_node_lifecycle_bootstrap_connects_and_dynamic_removal_unprotects_peer) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};

   auto server_options = make_lifecycle_node_options("lifecycle-server");
   server_options.lifecycle.listen = {parse_endpoint("/ip4/127.0.0.1/tcp/0")};
   auto server = node{runtime, std::move(server_options)};
   const auto server_status = forge::asio::blocking::run(runtime, server.async_start());
   BOOST_TEST(static_cast<int>(server_status.phase) == static_cast<int>(lifecycle_phase::maintenance));
   const auto address = server.local_endpoint();
   BOOST_REQUIRE(address.has_value());
   BOOST_REQUIRE(address->peer.has_value());

   auto client_options = make_lifecycle_node_options("lifecycle-client");
   client_options.lifecycle.bootstrap = {{.address = address->to_multiaddr()}};
   client_options.lifecycle.requirement = bootstrap_requirement::require_connection;
   client_options.lifecycle.startup_budget = std::chrono::seconds{3};
   client_options.lifecycle.connect_timeout = std::chrono::seconds{2};
   auto client = node{runtime, std::move(client_options)};

   const auto client_status = forge::asio::blocking::run(runtime, client.async_start());
   BOOST_TEST(client_status.connected_bootstrap == 1U);
   BOOST_TEST(!client_status.degraded);
   BOOST_TEST(client.is_peer_protected(server.local_peer()));

   forge::asio::blocking::run(runtime, client.async_set_bootstrap({}));
   BOOST_TEST(client.lifecycle_state().configured_bootstrap == 0U);
   BOOST_TEST(!client.is_peer_protected(server.local_peer()));

   forge::asio::blocking::run(runtime, client.async_stop());
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, client.async_set_bootstrap({})), exceptions::closed);
   BOOST_TEST(client.lifecycle_state().configured_bootstrap == 0U);
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_node_peerless_bootstrap_learns_authenticated_peer) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server_options = make_lifecycle_node_options("lifecycle-peerless-bootstrap-server");
   server_options.lifecycle.listen = {parse_endpoint("/ip4/127.0.0.1/tcp/0")};
   auto server = node{runtime, std::move(server_options)};
   static_cast<void>(forge::asio::blocking::run(runtime, server.async_start()));
   auto address = server.local_endpoint();
   BOOST_REQUIRE(address.has_value());
   address->peer.reset();

   auto client_options = make_lifecycle_node_options("lifecycle-peerless-bootstrap-client");
   client_options.lifecycle.bootstrap = {{.address = address->to_multiaddr()}};
   client_options.lifecycle.requirement = bootstrap_requirement::require_connection;
   client_options.lifecycle.startup_budget = std::chrono::seconds{3};
   auto client = node{runtime, std::move(client_options)};
   const auto status = forge::asio::blocking::run(runtime, client.async_start());

   BOOST_TEST(status.connected_bootstrap == 1U);
   BOOST_TEST(client.is_peer_protected(server.local_peer()));
   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_node_peerless_bootstrap_replaces_authenticated_peer_protection) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto first_options = make_lifecycle_node_options("lifecycle-peerless-rotation-first");
   first_options.lifecycle.listen = {parse_endpoint("/ip4/127.0.0.1/tcp/0")};
   auto first = node{runtime, std::move(first_options)};
   static_cast<void>(forge::asio::blocking::run(runtime, first.async_start()));
   auto address = first.local_endpoint();
   BOOST_REQUIRE(address.has_value());
   const auto first_peer = first.local_peer();
   address->peer.reset();

   auto client_options = make_lifecycle_node_options("lifecycle-peerless-rotation-client");
   client_options.lifecycle.bootstrap = {{.address = address->to_multiaddr()}};
   client_options.lifecycle.requirement = bootstrap_requirement::require_connection;
   client_options.lifecycle.startup_budget = std::chrono::seconds{3};
   client_options.lifecycle.maintenance_interval = std::chrono::milliseconds{25};
   client_options.lifecycle.bootstrap_retry_initial_delay = std::chrono::milliseconds{25};
   client_options.lifecycle.bootstrap_retry_max_delay = std::chrono::milliseconds{50};
   client_options.lifecycle.bootstrap_retry_jitter = 0.0;
   auto client = node{runtime, std::move(client_options)};
   static_cast<void>(forge::asio::blocking::run(runtime, client.async_start()));
   BOOST_TEST(client.is_peer_protected(first_peer));

   forge::asio::blocking::run(runtime, first.async_stop());
   auto second_options = make_lifecycle_node_options("lifecycle-peerless-rotation-second");
   second_options.lifecycle.listen = {*address};
   auto second = node{runtime, std::move(second_options)};
   static_cast<void>(forge::asio::blocking::run(runtime, second.async_start()));
   const auto second_peer = second.local_peer();
   BOOST_TEST(static_cast<bool>(first_peer != second_peer));

   BOOST_REQUIRE(
       eventually([&] { return client.is_peer_protected(second_peer) && !client.is_peer_protected(first_peer); },
                  std::chrono::seconds{3}));

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, second.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_node_peerless_bootstrap_alias_removal_preserves_shared_peer_protection) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto first_options = make_lifecycle_node_options("lifecycle-peerless-alias-first");
   first_options.lifecycle.listen = {
       parse_endpoint("/ip4/127.0.0.1/tcp/0"),
       parse_endpoint("/ip4/127.0.0.1/udp/0/quic-v1"),
   };
   auto first = node{runtime, std::move(first_options)};
   static_cast<void>(forge::asio::blocking::run(runtime, first.async_start()));
   auto addresses = first.local_endpoints();
   BOOST_REQUIRE_EQUAL(addresses.size(), 2U);
   for (auto& address : addresses) {
      address.peer.reset();
   }
   const auto tcp = std::ranges::find_if(addresses, [](const auto& address) { return address.is_direct_tcp(); });
   const auto quic = std::ranges::find_if(addresses, [](const auto& address) { return address.is_direct_quic(); });
   BOOST_REQUIRE(tcp != addresses.end());
   BOOST_REQUIRE(quic != addresses.end());
   const auto tcp_address = *tcp;
   const auto quic_address = *quic;
   const auto first_peer = first.local_peer();

   auto client_options = make_lifecycle_node_options("lifecycle-peerless-alias-client");
   client_options.lifecycle.bootstrap = {
       {.address = tcp_address.to_multiaddr()},
       {.address = quic_address.to_multiaddr()},
   };
   client_options.lifecycle.requirement = bootstrap_requirement::require_connection;
   client_options.lifecycle.startup_budget = std::chrono::seconds{3};
   client_options.lifecycle.maintenance_interval = std::chrono::milliseconds{25};
   client_options.lifecycle.bootstrap_retry_initial_delay = std::chrono::milliseconds{25};
   client_options.lifecycle.bootstrap_retry_max_delay = std::chrono::milliseconds{50};
   client_options.lifecycle.bootstrap_retry_jitter = 0.0;
   auto client = node{runtime, std::move(client_options)};
   static_cast<void>(forge::asio::blocking::run(runtime, client.async_start()));

   BOOST_REQUIRE(
       eventually([&] { return client.lifecycle_state().connected_bootstrap == 2U; }, std::chrono::seconds{3}));
   BOOST_TEST(client.is_peer_protected(first_peer));

   forge::asio::blocking::run(runtime, first.async_stop());
   auto second_options = make_lifecycle_node_options("lifecycle-peerless-alias-second");
   second_options.lifecycle.listen = {tcp_address};
   auto second = node{runtime, std::move(second_options)};
   static_cast<void>(forge::asio::blocking::run(runtime, second.async_start()));
   const auto second_peer = second.local_peer();
   BOOST_REQUIRE(first_peer != second_peer);

   BOOST_REQUIRE(eventually([&] { return client.is_peer_protected(second_peer); }, std::chrono::seconds{3}));
   BOOST_TEST(client.is_peer_protected(first_peer));

   forge::asio::blocking::run(runtime, client.async_set_bootstrap({bootstrap_peer{.address = tcp_address.to_multiaddr()}}));
   BOOST_REQUIRE(
       eventually([&] { return !client.is_peer_protected(first_peer) && client.is_peer_protected(second_peer); },
                  std::chrono::seconds{3}));

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, second.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_node_strict_bootstrap_cancellation_does_not_report_loser_failure) {
   namespace asio = boost::asio;
   using tcp = asio::ip::tcp;

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto stalled_acceptor = std::make_shared<tcp::acceptor>(runtime.context(), tcp::endpoint{tcp::v4(), 0});
   stalled_acceptor->listen();
   auto stalled_socket = std::make_shared<tcp::socket>(runtime.context());
   asio::co_spawn(
       runtime.context(),
       [stalled_acceptor, stalled_socket]() -> asio::awaitable<void> {
          auto error = boost::system::error_code{};
          co_await stalled_acceptor->async_accept(*stalled_socket, asio::redirect_error(asio::use_awaitable, error));
       },
       asio::detached);

   auto server_options = make_lifecycle_node_options("lifecycle-bootstrap-race-server");
   server_options.lifecycle.listen = {parse_endpoint("/ip4/127.0.0.1/tcp/0")};
   auto server = node{runtime, std::move(server_options)};
   static_cast<void>(forge::asio::blocking::run(runtime, server.async_start()));
   const auto available = server.local_endpoint();
   BOOST_REQUIRE(available.has_value());

   const auto stalled_identity = forge::tests::p2p::make_identity_fixture("lifecycle-bootstrap-race-stalled");
   const auto stalled_peer = make_peer_id_from_certificate_pem(stalled_identity.certificate_pem);
   const auto stalled =
       parse_endpoint("/ip4/127.0.0.1/tcp/" + std::to_string(stalled_acceptor->local_endpoint().port()) + "/p2p/" +
                      stalled_peer.to_string());

   auto client_options = make_lifecycle_node_options("lifecycle-bootstrap-race-client");
   client_options.lifecycle.bootstrap = {{.address = available->to_multiaddr()}, {.address = stalled.to_multiaddr()}};
   client_options.lifecycle.requirement = bootstrap_requirement::require_connection;
   client_options.lifecycle.startup_budget = std::chrono::seconds{3};
   client_options.lifecycle.connect_timeout = std::chrono::seconds{2};
   client_options.lifecycle.max_parallel_bootstrap = 2;
   auto client = node{runtime, std::move(client_options)};

   const auto status = forge::asio::blocking::run(runtime, client.async_start());
   BOOST_TEST(status.connected_bootstrap == 1U);
   BOOST_TEST(status.last_bootstrap_failure.empty());
   BOOST_TEST(client.lifecycle_state().last_bootstrap_failure.empty());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
   auto ignored = boost::system::error_code{};
   stalled_socket->close(ignored);
   stalled_acceptor->close(ignored);
}

BOOST_AUTO_TEST_CASE(p2p_node_destructor_requests_maintenance_stop) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   {
      auto value = node{runtime, make_lifecycle_node_options("lifecycle-destructor-stop")};
      static_cast<void>(forge::asio::blocking::run(runtime, value.async_start()));
   }
   std::this_thread::sleep_for(std::chrono::milliseconds{50});

   auto replacement = node{runtime, make_lifecycle_node_options("lifecycle-after-destructor")};
   const auto status = forge::asio::blocking::run(runtime, replacement.async_start());
   BOOST_TEST(static_cast<int>(status.phase) == static_cast<int>(lifecycle_phase::maintenance));
   forge::asio::blocking::run(runtime, replacement.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_node_connect_waits_for_identify_and_push_replaces_protocol_snapshot) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto product_protocol = protocol_id{.value = "/product/lifecycle-identify/1"};

   auto server_options = make_lifecycle_node_options("lifecycle-identify-server");
   server_options.lifecycle.listen = {parse_endpoint("/ip4/127.0.0.1/tcp/0")};
   auto server = node{runtime, std::move(server_options)};
   server.register_protocol_handler(product_protocol,
                                    [](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
                                       co_await incoming.stream.async_close();
                                    });
   static_cast<void>(forge::asio::blocking::run(runtime, server.async_start()));

   auto client = node{runtime, make_lifecycle_node_options("lifecycle-identify-client")};
   static_cast<void>(forge::asio::blocking::run(runtime, client.async_start()));
   const auto address = server.local_endpoint();
   BOOST_REQUIRE(address.has_value());

   const auto session = forge::asio::blocking::run(
       runtime, client.async_connect(*address, node::connect_options{.expected_peer = server.local_peer()}));
   BOOST_TEST(static_cast<int>(session.identify_state) == static_cast<int>(identify::state::identified));

   auto record = client.peers().find(server.local_peer());
   BOOST_REQUIRE(record.has_value());
   BOOST_TEST(supports(*record, product_protocol));

   BOOST_TEST(server.unregister_protocol_handler(product_protocol));
   BOOST_REQUIRE(eventually(
       [&] {
          const auto updated = client.peers().find(server.local_peer());
          return updated && !supports(*updated, product_protocol);
       },
       std::chrono::seconds{2}));

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_protocol_registration_rejects_unrepresentable_identify_snapshot) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto invalid_base_options = make_lifecycle_node_options("lifecycle-identify-invalid-base");
   invalid_base_options.identify.max_own_message_size = 1;
   BOOST_CHECK_THROW(node(runtime, std::move(invalid_base_options)), exceptions::invalid_options);
   auto invalid_base_protocols = make_lifecycle_node_options("lifecycle-identify-invalid-base-protocols");
   invalid_base_protocols.identify.max_protocols = 1;
   BOOST_CHECK_THROW(node(runtime, std::move(invalid_base_protocols)), exceptions::invalid_options);

   auto server_options = make_lifecycle_node_options("lifecycle-identify-capacity-server");
   server_options.lifecycle.listen = {parse_endpoint("/ip4/127.0.0.1/tcp/0")};
   auto server = node{runtime, std::move(server_options)};

   BOOST_CHECK_THROW(
       server.register_protocol_handler(protocol_id{.value = "missing-leading-slash"},
                                        [](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
                                           co_await incoming.stream.async_close();
                                        }),
       exceptions::invalid_options);
   BOOST_CHECK_THROW(server.register_protocol_handler(
                         protocol_id{.value = "/" + std::string(identify::limits{}.max_protocol_size, 'x')},
                         [](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
                            co_await incoming.stream.async_close();
                         }),
                     exceptions::invalid_options);
   auto count_options = make_lifecycle_node_options("lifecycle-identify-count-limit");
   count_options.identify.max_protocols = 9;
   const auto count_limit = count_options.identify.max_protocols;
   count_options.lifecycle.listen = {parse_endpoint("/ip4/127.0.0.1/tcp/0")};
   auto count_limited = node{runtime, std::move(count_options)};
   auto count_rejected = std::optional<protocol_id>{};
   // Service opt-in changes the builtin count. Fill the configured capacity,
   // then verify the actual advertised count rather than assuming nine builtins.
   for (auto index = std::size_t{}; index <= count_limit; ++index) {
      auto protocol = protocol_id{.value = "/product/count-limit/" + std::to_string(index)};
      try {
         count_limited.register_protocol_handler(protocol,
             [](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
                co_await incoming.stream.async_close();
             });
      } catch (const exceptions::backpressure_rejected&) {
         count_rejected = std::move(protocol);
         break;
      }
   }
   BOOST_REQUIRE(count_rejected);
   BOOST_CHECK_THROW(count_limited.register_protocol_handler(
                         *count_rejected,
                         [](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
                            co_await incoming.stream.async_close();
                         }),
                     exceptions::backpressure_rejected);

   auto accepted = std::vector<protocol_id>{};
   auto rejected = std::optional<protocol_id>{};
   for (auto index = 0U; index < 64U; ++index) {
      auto protocol = protocol_id{
          .value = "/product/identify-capacity/" + std::to_string(index) + "/" + std::string(180, 'x'),
      };
      try {
         server.register_protocol_handler(protocol,
                                          [](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
                                             co_await incoming.stream.async_close();
                                          });
         accepted.push_back(std::move(protocol));
      } catch (const exceptions::backpressure_rejected&) {
         rejected = std::move(protocol);
         break;
      }
   }
   BOOST_REQUIRE(rejected.has_value());
   BOOST_REQUIRE(!accepted.empty());
   BOOST_TEST(!server.unregister_protocol_handler(*rejected));

   static_cast<void>(forge::asio::blocking::run(runtime, server.async_start()));
   const auto address = server.local_endpoint();
   BOOST_REQUIRE(address.has_value());
   auto client = node{runtime, make_lifecycle_node_options("lifecycle-identify-capacity-client")};
   static_cast<void>(forge::asio::blocking::run(runtime, client.async_start()));
   const auto session = forge::asio::blocking::run(
       runtime, client.async_connect(*address, node::connect_options{.expected_peer = server.local_peer()}));
   BOOST_TEST(static_cast<int>(session.identify_state) == static_cast<int>(identify::state::identified));

   const auto record = client.peers().find(server.local_peer());
   BOOST_REQUIRE(record.has_value());
   BOOST_TEST(!supports(*record, *rejected));
   BOOST_TEST(std::ranges::all_of(accepted, [&](const auto& protocol) { return supports(*record, protocol); }));

   static_cast<void>(forge::asio::blocking::run(runtime, count_limited.async_start()));
   const auto count_address = count_limited.local_endpoint();
   BOOST_REQUIRE(count_address);
   static_cast<void>(forge::asio::blocking::run(runtime, client.async_connect(*count_address)));
   const auto count_record = client.peers().find(count_limited.local_peer());
   BOOST_REQUIRE(count_record);
   BOOST_TEST(count_record->protocols.size() == count_limit);
   BOOST_TEST(!supports(*count_record, *count_rejected));

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
   forge::asio::blocking::run(runtime, count_limited.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_identify_push_fanout_covers_multiple_bounded_batches) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto product_protocol = protocol_id{.value = "/product/lifecycle-fanout/1"};

   auto server_options = make_lifecycle_node_options("lifecycle-fanout-server");
   server_options.lifecycle.listen = {parse_endpoint("/ip4/127.0.0.1/tcp/0")};
   server_options.identify.max_push_operations = 2;
   auto server = node{runtime, std::move(server_options)};
   static_cast<void>(forge::asio::blocking::run(runtime, server.async_start()));
   const auto address = server.local_endpoint();
   BOOST_REQUIRE(address.has_value());

   auto clients = std::vector<std::unique_ptr<node>>{};
   for (auto index = 0U; index < 5U; ++index) {
      auto client = std::make_unique<node>(
          runtime, make_lifecycle_node_options("lifecycle-fanout-client-" + std::to_string(index)));
      static_cast<void>(forge::asio::blocking::run(runtime, client->async_start()));
      static_cast<void>(forge::asio::blocking::run(
          runtime, client->async_connect(*address, node::connect_options{.expected_peer = server.local_peer()})));
      clients.push_back(std::move(client));
   }

   server.register_protocol_handler(product_protocol,
                                    [](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
                                       co_await incoming.stream.async_close();
                                    });
   BOOST_REQUIRE(eventually(
       [&] {
          return std::ranges::all_of(clients, [&](const auto& client) {
             const auto record = client->peers().find(server.local_peer());
             return record && supports(*record, product_protocol);
          });
       },
       std::chrono::seconds{3}));

   BOOST_TEST(server.unregister_protocol_handler(product_protocol));
   BOOST_REQUIRE(eventually(
       [&] {
          return std::ranges::all_of(clients, [&](const auto& client) {
             const auto record = client->peers().find(server.local_peer());
             return record && !supports(*record, product_protocol);
          });
       },
       std::chrono::seconds{3}));

   for (auto& client : clients) {
      forge::asio::blocking::run(runtime, client->async_stop());
   }
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_node_identify_failure_keeps_authenticated_session_usable) {
   auto capture = std::make_shared<p2p_log_capture>();
   auto routes = p2p_log_routes{capture, forge::log_level::debug, {"forge.net.p2p.connection"}};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};

   auto server_options = make_lifecycle_node_options("lifecycle-identify-failure-server");
   server_options.lifecycle.listen = {parse_endpoint("/ip4/127.0.0.1/tcp/0")};
   server_options.agent_version = std::string(128, 'a');
   auto server = node{runtime, std::move(server_options)};
   static_cast<void>(forge::asio::blocking::run(runtime, server.async_start()));

   auto client_options = make_lifecycle_node_options("lifecycle-identify-failure-client");
   client_options.identify.max_version_size = 32;
   auto client = node{runtime, std::move(client_options)};
   static_cast<void>(forge::asio::blocking::run(runtime, client.async_start()));
   const auto address = server.local_endpoint();
   BOOST_REQUIRE(address.has_value());

   capture->inspect([&] {
      static_cast<void>(client.metrics());
      static_cast<void>(server.metrics());
      static_cast<void>(client.diagnostics().sessions);
      static_cast<void>(server.diagnostics().sessions);
   });
   const auto session = forge::asio::blocking::run(
       runtime, client.async_connect(*address, node::connect_options{.expected_peer = server.local_peer()}));
   BOOST_TEST(static_cast<int>(session.identify_state) == static_cast<int>(identify::state::failed));
   BOOST_TEST(session.capabilities.bits == 0U);

   const auto pushed_protocol = protocol_id{.value = "/product/push-after-failed-identify/1"};
   auto push = forge::asio::blocking::run(
       runtime, server.async_open_protocol_stream(client.local_peer(), builtins::identify_push));
   const auto document = identify::document{
       .protocol_version = "/push/1",
       .agent_version = "push/1",
       .protocols = std::vector<protocol_id>{builtins::ping, builtins::identify_push, pushed_protocol},
   };
   forge::asio::blocking::run(runtime, push.async_write(length_delimited(identify::encode(document))));
   forge::asio::blocking::run(runtime, push.async_close());
   BOOST_REQUIRE(eventually(
       [&] {
          const auto record = client.peers().find(server.local_peer());
          return record && supports(*record, pushed_protocol);
       },
       std::chrono::seconds{2}));
   const auto diagnostics = client.diagnostics();
   const auto active = std::ranges::find_if(
       diagnostics.sessions, [&](const auto& value) { return value.remote_peer == server.local_peer(); });
   BOOST_REQUIRE(active != diagnostics.sessions.end());
   BOOST_TEST(static_cast<int>(active->identify_state) == static_cast<int>(identify::state::failed));
   BOOST_TEST(active->capabilities.bits == 0U);

   const auto latency = forge::asio::blocking::run(runtime, client.async_ping(server.local_peer()));
   BOOST_TEST(latency >= std::chrono::milliseconds{0});

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
   capture->inspect({});
#ifndef FORGE_DISABLE_LOGGING
   BOOST_TEST(capture->count("connection.admitted") >= 2U);
   BOOST_TEST(capture->count("identify.completed") >= 2U);
#else
   BOOST_TEST(capture->records().empty());
#endif
   for (const auto& record : capture->records()) {
      BOOST_TEST(record.level.value == forge::log_level::debug);
      BOOST_TEST(forge::format_json_log_record(record).find(std::string(128, 'a')) == std::string::npos);
      const auto event = std::ranges::find(record.fields, std::string{"event"}, &forge::log_field::key);
      if (event != record.fields.end() && event->value == "connection.admitted") {
         const auto peer = std::ranges::find(record.fields, std::string{"peer_id"}, &forge::log_field::key);
         BOOST_REQUIRE(peer != record.fields.end());
         BOOST_TEST((peer->value == client.local_peer().to_string() || peer->value == server.local_peer().to_string()));
      }
   }
}

} // namespace forge::net::p2p
