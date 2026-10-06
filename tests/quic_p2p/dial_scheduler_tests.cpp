module;

#include <boost/test/unit_test.hpp>
#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <future>
#include <initializer_list>
#include <map>
#include <memory>
#include <span>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/compat/move_only_function.hpp>

module forge.net.p2p.node;

import forge.asio.blocking;
import forge.asio.notification;
import forge.asio.runtime;
import forge.multiformats.multiaddr;
import forge.net.dns.exceptions;
import forge.net.dns.resolver;
import forge.net.dns.types;
import forge.net.p2p.address_resolution;
import forge.net.p2p.dialing;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.resource_manager;
import forge.net.transport.session;

#include "../../libraries/net/p2p/details/cancellation_latch.hxx"
#include "../../libraries/net/p2p/details/dial_scheduler.hxx"

namespace {

namespace dns = forge::net::dns;
namespace p2p = forge::net::p2p;

class dial_script final {
 public:
   enum class behavior {
      succeed,
      attributable_failure,
      wait_for_cancel_then_succeed,
      wait_for_cancel_then_fail,
      wait_for_release_then_succeed,
      wait_for_release_then_attributable_failure,
      delayed_progress_then_attributable_failure,
      terminal_rejection,
      address_rejection,
   };

   struct start_event {
      std::string endpoint;
      std::optional<p2p::peer_id> expected_peer;
      std::chrono::steady_clock::time_point deadline;
      std::chrono::steady_clock::time_point started;
   };

   std::map<std::string, dns::address_response> address_responses;
   std::map<std::string, dns::text_response> text_responses;
   std::set<std::string> address_not_found;
   std::map<std::string, behavior> behaviors;
   std::shared_ptr<p2p::resource_manager> resources;
   bool block_discards = false;
   bool block_dns_until_stop = false;
   bool block_dns_terminal_after_stop = false;
   bool block_attempt_terminal_after_stop = false;
   std::size_t throwing_discards = 0;
   std::atomic_bool owner_stopping = false;

   [[nodiscard]] boost::asio::awaitable<dns::address_response>
   resolve_addresses(std::string name, dns::address_family, dns::query_options, std::stop_token stop) {
      auto response = dns::address_response{};
      auto block = false;
      auto not_found = false;
      {
         const auto lock = std::scoped_lock{mutex_};
         if (const auto found = address_responses.find(name); found != address_responses.end()) {
            response = found->second;
         }
         not_found = address_not_found.contains(name);
         block = block_dns_until_stop;
         if (block) {
            ++address_lookups_;
         }
      }
      if (!block) {
         if (not_found) {
            FORGE_THROW_EXCEPTION(dns::exceptions::not_found, "scripted DNS name not found");
         }
         co_return response;
      }
      address_lookup_started_.notify_all();
      co_await wait_for_stop(stop, dns_stop_wakeup_);
      if (block_dns_terminal_after_stop) {
         {
            const auto lock = std::scoped_lock{mutex_};
            dns_terminal_waiting_ = true;
         }
         dns_terminal_waiting_changed_.notify_all();
         co_await wait_for_dns_terminal_release();
      }
      FORGE_THROW_EXCEPTION(dns::exceptions::canceled, "scripted DNS lookup canceled");
   }

   [[nodiscard]] boost::asio::awaitable<dns::text_response>
   resolve_txt(std::string name, dns::query_options, std::stop_token) {
      if (const auto found = text_responses.find(name); found != text_responses.end()) {
         co_return found->second;
      }
      co_return dns::text_response{};
   }

   [[nodiscard]] boost::asio::awaitable<p2p::detail::direct_attempt>
   start(p2p::endpoint value, std::optional<p2p::peer_id> expected_peer,
         std::chrono::steady_clock::time_point deadline, std::shared_ptr<p2p::cancellation_latch> cancellation,
         p2p::direct::tcp_transport_progress_handler progress) {
      auto selected = behavior::succeed;
      {
         const auto lock = std::scoped_lock{mutex_};
         const auto key = value.to_string();
         if (const auto found = behaviors.find(key); found != behaviors.end()) {
            selected = found->second;
         }
         starts_.push_back({
             .endpoint = key,
             .expected_peer = std::move(expected_peer),
             .deadline = deadline,
             .started = std::chrono::steady_clock::now(),
         });
         ++active_attempts_;
         if (active_attempts_ > peak_attempts_) {
            peak_attempts_ = active_attempts_;
         }
      }
      started_.notify_all();
      [[maybe_unused]] const auto active_attempt = active_attempt_guard{*this};
      auto attempt = p2p::detail::direct_attempt{};
      if (resources) {
         auto session = resources->reserve_session(p2p::resource_manager::session_direction::outbound);
         if (!session) {
            FORGE_THROW_EXCEPTION(p2p::exceptions::backpressure_rejected, "scripted session reservation rejected");
         }
         auto descriptor = session->reserve_file_descriptors(1);
         if (!descriptor) {
            FORGE_THROW_EXCEPTION(p2p::exceptions::backpressure_rejected, "scripted descriptor reservation rejected");
         }
         attempt.resources = std::make_shared<p2p::detail::direct_attempt_resources>();
         attempt.resources->session = std::move(*session);
         attempt.resources->file_descriptor = std::move(*descriptor);
      }

      if (selected == behavior::delayed_progress_then_attributable_failure) {
         auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
         timer.expires_after(std::chrono::milliseconds{20});
         co_await timer.async_wait(boost::asio::use_awaitable);
         if (progress) {
            progress();
         }
      }
      if (selected == behavior::succeed || selected == behavior::wait_for_cancel_then_succeed) {
         if (selected == behavior::succeed) {
            co_return std::move(attempt);
         }
         co_await wait_for_cancel(std::move(cancellation));
         co_return std::move(attempt);
      }
      if (selected == behavior::wait_for_cancel_then_fail) {
         co_await wait_for_cancel(std::move(cancellation));
         FORGE_THROW_EXCEPTION(p2p::exceptions::canceled, "scripted direct attempt canceled");
      }
      if (selected == behavior::wait_for_release_then_succeed ||
          selected == behavior::wait_for_release_then_attributable_failure) {
         co_await wait_for_start_release(std::move(cancellation));
         if (selected == behavior::wait_for_release_then_succeed) {
            co_return std::move(attempt);
         }
      }
      if (selected == behavior::terminal_rejection) {
         FORGE_THROW_EXCEPTION(p2p::exceptions::backpressure_rejected,
                               "scripted terminal direct attempt rejection");
      }
      if (selected == behavior::address_rejection) {
         FORGE_THROW_EXCEPTION(p2p::exceptions::connection_rejected, "scripted address gater rejection");
      }
      FORGE_THROW_EXCEPTION(p2p::exceptions::peer_not_found, "scripted attributable direct attempt failure");
   }

   [[nodiscard]] boost::asio::awaitable<void> discard(p2p::detail::direct_attempt&) {
      auto block = false;
      auto should_throw = false;
      {
         const auto lock = std::scoped_lock{mutex_};
         ++discards_;
         block = block_discards;
         if (throwing_discards != 0) {
            --throwing_discards;
            should_throw = true;
         }
      }
      discarded_.notify_all();
      if (should_throw) {
         FORGE_THROW_EXCEPTION(p2p::exceptions::backpressure_rejected, "scripted discard failure");
      }
      if (block) {
         const auto observed = discard_release_->epoch();
         static_cast<void>(co_await discard_release_->async_wait(observed));
      }
   }

   [[nodiscard]] bool wait_for_starts(std::size_t count) {
      auto lock = std::unique_lock{mutex_};
      return started_.wait_for(lock, std::chrono::seconds{1}, [&] { return starts_.size() >= count; });
   }

   [[nodiscard]] bool wait_for_discards(std::size_t count) {
      auto lock = std::unique_lock{mutex_};
      return discarded_.wait_for(lock, std::chrono::seconds{1}, [&] { return discards_ >= count; });
   }

   [[nodiscard]] bool wait_for_active_attempts(std::size_t count) {
      auto lock = std::unique_lock{mutex_};
      return active_.wait_for(lock, std::chrono::seconds{1}, [&] { return active_attempts_ >= count; });
   }

   [[nodiscard]] bool wait_for_address_lookups(std::size_t count) {
      auto lock = std::unique_lock{mutex_};
      return address_lookup_started_.wait_for(lock, std::chrono::seconds{1}, [&] { return address_lookups_ >= count; });
   }

   [[nodiscard]] bool wait_for_dns_terminal_waiting() {
      auto lock = std::unique_lock{mutex_};
      return dns_terminal_waiting_changed_.wait_for(lock, std::chrono::seconds{1}, [&] { return dns_terminal_waiting_; });
   }

   [[nodiscard]] bool wait_for_attempt_terminal_waiting() {
      auto lock = std::unique_lock{mutex_};
      return attempt_terminal_waiting_changed_.wait_for(lock, std::chrono::seconds{1}, [&] {
         return attempt_terminal_waiting_;
      });
   }

   [[nodiscard]] bool attempt_terminal_waiting() const {
      const auto lock = std::scoped_lock{mutex_};
      return attempt_terminal_waiting_;
   }

   [[nodiscard]] std::vector<start_event> starts() const {
      const auto lock = std::scoped_lock{mutex_};
      return starts_;
   }

   [[nodiscard]] std::size_t discards() const {
      const auto lock = std::scoped_lock{mutex_};
      return discards_;
   }

   void observe_terminal_root_outcomes(std::vector<p2p::detail::dial_root_outcome> value) noexcept {
      const auto lock = std::scoped_lock{mutex_};
      ++terminal_root_observations_;
      terminal_root_outcomes_.emplace(std::move(value));
   }

   [[nodiscard]] std::size_t terminal_root_observations() const {
      const auto lock = std::scoped_lock{mutex_};
      return terminal_root_observations_;
   }

   [[nodiscard]] std::vector<p2p::detail::dial_root_outcome> terminal_root_outcomes() const {
      const auto lock = std::scoped_lock{mutex_};
      return terminal_root_outcomes_.value_or(std::vector<p2p::detail::dial_root_outcome>{});
   }

   [[nodiscard]] std::size_t active_attempts() const {
      const auto lock = std::scoped_lock{mutex_};
      return active_attempts_;
   }

   [[nodiscard]] std::size_t peak_attempts() const {
      const auto lock = std::scoped_lock{mutex_};
      return peak_attempts_;
   }

   [[nodiscard]] std::size_t finished_attempts() const {
      const auto lock = std::scoped_lock{mutex_};
      return finished_attempts_;
   }

   void release_discards() noexcept {
      discard_release_->notify();
   }

   void release_starts() noexcept {
      {
         const auto lock = std::scoped_lock{mutex_};
         starts_released_ = true;
      }
      start_release_->notify();
   }

   void release_dns_terminal() noexcept {
      {
         const auto lock = std::scoped_lock{mutex_};
         dns_terminal_released_ = true;
      }
      dns_terminal_release_->notify();
   }

   void release_attempt_terminal() noexcept {
      {
         const auto lock = std::scoped_lock{mutex_};
         attempt_terminal_released_ = true;
      }
      attempt_terminal_release_->notify();
   }

 private:
   class active_attempt_guard final {
    public:
      explicit active_attempt_guard(dial_script& script) noexcept : script_(&script) {}

      ~active_attempt_guard() {
         script_->finish_attempt();
      }

    private:
      dial_script* script_;
   };

   boost::asio::awaitable<void> wait_for_cancel(const std::shared_ptr<p2p::cancellation_latch>& cancellation) {
      auto subscription = p2p::cancellation_latch::subscribe(cancellation, [wakeup = cancel_wakeup_] noexcept {
         wakeup->notify();
      });
      while (!cancellation->stop_requested()) {
         const auto observed = cancel_wakeup_->epoch();
         if (cancellation->stop_requested()) {
            break;
         }
         static_cast<void>(co_await cancel_wakeup_->async_wait(observed));
      }
      if (block_attempt_terminal_after_stop) {
         {
            const auto lock = std::scoped_lock{mutex_};
            attempt_terminal_waiting_ = true;
         }
         attempt_terminal_waiting_changed_.notify_all();
         co_await wait_for_attempt_terminal_release();
      }
   }

   static boost::asio::awaitable<void> wait_for_stop(std::stop_token stop,
                                                       const std::shared_ptr<forge::asio::notification>& wakeup) {
      auto callback = std::stop_callback{stop, [wakeup] noexcept { wakeup->notify(); }};
      while (!stop.stop_requested()) {
         const auto observed = wakeup->epoch();
         if (stop.stop_requested()) {
            break;
         }
         static_cast<void>(co_await wakeup->async_wait(observed));
      }
   }

   boost::asio::awaitable<void> wait_for_dns_terminal_release() {
      for (;;) {
         {
            const auto lock = std::scoped_lock{mutex_};
            if (dns_terminal_released_) {
               co_return;
            }
         }
         const auto observed = dns_terminal_release_->epoch();
         {
            const auto lock = std::scoped_lock{mutex_};
            if (dns_terminal_released_) {
               co_return;
            }
         }
         static_cast<void>(co_await dns_terminal_release_->async_wait(observed));
      }
   }

   boost::asio::awaitable<void> wait_for_attempt_terminal_release() {
      for (;;) {
         {
            const auto lock = std::scoped_lock{mutex_};
            if (attempt_terminal_released_) {
               co_return;
            }
         }
         const auto observed = attempt_terminal_release_->epoch();
         {
            const auto lock = std::scoped_lock{mutex_};
            if (attempt_terminal_released_) {
               co_return;
            }
         }
         static_cast<void>(co_await attempt_terminal_release_->async_wait(observed));
      }
   }

   boost::asio::awaitable<void> wait_for_start_release(
       const std::shared_ptr<p2p::cancellation_latch>& cancellation) {
      auto subscription = p2p::cancellation_latch::subscribe(cancellation, [wakeup = start_release_] noexcept {
         wakeup->notify();
      });
      for (;;) {
         {
            const auto lock = std::scoped_lock{mutex_};
            if (starts_released_ || cancellation->stop_requested()) {
               co_return;
            }
         }
         const auto observed = start_release_->epoch();
         {
            const auto lock = std::scoped_lock{mutex_};
            if (starts_released_ || cancellation->stop_requested()) {
               co_return;
            }
         }
         static_cast<void>(co_await start_release_->async_wait(observed));
      }
   }

   void finish_attempt() noexcept {
      {
         const auto lock = std::scoped_lock{mutex_};
         --active_attempts_;
         ++finished_attempts_;
      }
      active_.notify_all();
   }

   mutable std::mutex mutex_;
   std::condition_variable started_;
   std::condition_variable discarded_;
   std::condition_variable active_;
   std::condition_variable address_lookup_started_;
   std::condition_variable dns_terminal_waiting_changed_;
   std::condition_variable attempt_terminal_waiting_changed_;
   std::vector<start_event> starts_;
   std::size_t discards_ = 0;
   std::size_t address_lookups_ = 0;
   std::size_t active_attempts_ = 0;
   std::size_t peak_attempts_ = 0;
   std::size_t finished_attempts_ = 0;
   std::size_t terminal_root_observations_ = 0;
   bool starts_released_ = false;
   bool dns_terminal_released_ = false;
   bool attempt_terminal_released_ = false;
   bool dns_terminal_waiting_ = false;
   bool attempt_terminal_waiting_ = false;
   std::optional<std::vector<p2p::detail::dial_root_outcome>> terminal_root_outcomes_;
   std::shared_ptr<forge::asio::notification> cancel_wakeup_ = std::make_shared<forge::asio::notification>();
   std::shared_ptr<forge::asio::notification> discard_release_ = std::make_shared<forge::asio::notification>();
   std::shared_ptr<forge::asio::notification> start_release_ = std::make_shared<forge::asio::notification>();
   std::shared_ptr<forge::asio::notification> dns_stop_wakeup_ = std::make_shared<forge::asio::notification>();
   std::shared_ptr<forge::asio::notification> dns_terminal_release_ = std::make_shared<forge::asio::notification>();
   std::shared_ptr<forge::asio::notification> attempt_terminal_release_ =
       std::make_shared<forge::asio::notification>();
};

[[nodiscard]] dns::address_response addresses(std::initializer_list<std::string_view> values) {
   auto result = dns::address_response{};
   for (const auto value : values) {
      result.answers.push_back({.value = boost::asio::ip::make_address(value), .ttl = std::chrono::seconds{60}});
   }
   return result;
}

[[nodiscard]] dns::text_response dnsaddr_records(std::initializer_list<std::string_view> values) {
   auto result = dns::text_response{};
   for (const auto value : values) {
      const auto record = "dnsaddr=" + std::string{value};
      result.answers.push_back({.value = {record.begin(), record.end()}, .ttl = std::chrono::seconds{60}});
   }
   return result;
}

[[nodiscard]] p2p::detail::dial_scheduler::operation_callbacks callbacks_for(const std::shared_ptr<dial_script>& script) {
   return {
       .resolve_addresses =
           [script](std::string name, dns::address_family family, dns::query_options options,
                    std::stop_token stop) {
               return script->resolve_addresses(std::move(name), family, std::move(options), stop);
            },
       .resolve_txt =
           [script](std::string name, dns::query_options options,
                    std::stop_token stop) {
               return script->resolve_txt(std::move(name), std::move(options), stop);
            },
       .start_attempt =
           [script](p2p::endpoint value, std::optional<p2p::peer_id> expected_peer,
                    std::chrono::steady_clock::time_point deadline,
                    std::shared_ptr<p2p::cancellation_latch> cancellation,
                    p2p::direct::tcp_transport_progress_handler progress) {
              return script->start(std::move(value), std::move(expected_peer), deadline,
                                   std::move(cancellation), std::move(progress));
           },
       .discard_attempt = [script](p2p::detail::direct_attempt& value) { return script->discard(value); },
       .is_owner_stopping = [script] noexcept { return script->owner_stopping.load(std::memory_order_acquire); },
       .observe_terminal_root_outcomes = [script](std::vector<p2p::detail::dial_root_outcome> value) noexcept {
          script->observe_terminal_root_outcomes(std::move(value));
       },
   };
}

[[nodiscard]] p2p::detail::dial_scheduler::request request_for(std::vector<std::string> values,
                                                                 std::chrono::milliseconds timeout,
                                                                 std::optional<p2p::peer_id> expected_peer = {},
                                                                 std::stop_token stop = {},
                                                                 std::chrono::milliseconds attempt_timeout =
                                                                     std::chrono::seconds{1}) {
   auto roots = std::vector<forge::multiformats::multiaddr>{};
   roots.reserve(values.size());
   for (auto& value : values) {
      roots.push_back(forge::multiformats::multiaddr::parse(std::move(value)));
   }
   return {
       .roots = std::move(roots),
       .expected_peer = std::move(expected_peer),
       .logical_deadline = std::chrono::steady_clock::now() + timeout,
       .attempt_timeout = attempt_timeout,
       .stop = stop,
   };
}

[[nodiscard]] p2p::detail::dial_scheduler::request request_for(std::string value,
                                                                 std::chrono::milliseconds timeout,
                                                                 std::optional<p2p::peer_id> expected_peer = {},
                                                                 std::stop_token stop = {},
                                                                 std::chrono::milliseconds attempt_timeout =
                                                                     std::chrono::seconds{1}) {
   return request_for(std::vector<std::string>{std::move(value)}, timeout, std::move(expected_peer), stop,
                      attempt_timeout);
}

} // namespace

BOOST_AUTO_TEST_SUITE(dial_scheduler_tests)

BOOST_AUTO_TEST_CASE(dial_scheduler_bounds_actual_launches_and_preserves_unlaunched_root_attribution) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->address_responses.emplace("capped.test", addresses({"2400::1", "8.8.8.9"}));
   script->address_responses.emplace("exhausted.test", addresses({"8.8.8.8"}));
   script->behaviors.emplace("/ip6/2400::1/udp/4001/quic-v1", dial_script::behavior::attributable_failure);
   script->behaviors.emplace("/ip4/8.8.8.8/udp/4001/quic-v1",
                             dial_script::behavior::wait_for_release_then_attributable_failure);
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 4}};
   auto request = request_for(std::vector<std::string>{"/dns/capped.test/udp/4001/quic-v1",
                                                       "/dns/exhausted.test/udp/4001/quic-v1"},
                              std::chrono::seconds{5});
   request.max_attempts = 2;
   auto result = boost::asio::co_spawn(runtime.context().get_executor(),
                                     scheduler.async_dial(std::move(request), callbacks_for(script)),
                                     boost::asio::use_future);
   BOOST_REQUIRE(script->wait_for_starts(2));
   BOOST_CHECK(result.wait_for(std::chrono::milliseconds{30}) == std::future_status::timeout);
   BOOST_TEST(script->terminal_root_observations() == 0U);
   script->release_starts();
   BOOST_REQUIRE(result.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   BOOST_CHECK_EXCEPTION(result.get(), forge::exceptions::base, [](const auto& error) {
      return p2p::exceptions::code_of(error) == p2p::exceptions::code::peer_not_found;
   });
   BOOST_TEST(script->starts().size() == 2U);
   BOOST_TEST(script->finished_attempts() == 2U);
   BOOST_TEST(script->active_attempts() == 0U);
   BOOST_TEST(script->terminal_root_observations() == 1U);
   const auto outcomes = script->terminal_root_outcomes();
   BOOST_REQUIRE_EQUAL(outcomes.size(), 2U);
   BOOST_CHECK(outcomes[0].outcome == p2p::dialing::outcome::neutral);
   BOOST_CHECK(outcomes[1].outcome == p2p::dialing::outcome::failure);
   BOOST_TEST(scheduler.black_hole_status().udp.outcomes == 2U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_rejects_attempt_limits_outside_the_positive_bound) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {}};
   BOOST_TEST(p2p::detail::dial_scheduler::request{}.max_attempts == 100U);
   for (const auto limit : {std::size_t{0}, std::size_t{101}}) {
      auto request = request_for("/ip4/8.8.8.8/tcp/4001", std::chrono::seconds{1});
      request.max_attempts = limit;
      BOOST_CHECK_EXCEPTION(
          forge::asio::blocking::run(runtime, scheduler.async_dial(std::move(request), callbacks_for(script))),
          forge::exceptions::base, [](const auto& error) {
             return p2p::exceptions::code_of(error) == p2p::exceptions::code::invalid_options;
          });
   }
   BOOST_TEST(script->starts().empty());
}

BOOST_AUTO_TEST_CASE(dial_scheduler_tcp_only_filters_mixed_dnsaddr_before_detectors_and_launch_budget) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->text_responses.emplace("_dnsaddr.mixed.test", dnsaddr_records({
       "/ip6/2400::1/udp/4001/quic-v1", "/ip4/8.8.8.8/tcp/4001"}));
   script->text_responses.emplace("_dnsaddr.quic-only.test", dnsaddr_records({"/ip6/2400::2/udp/4001/quic-v1"}));
   script->behaviors.emplace("/ip6/2400::1/udp/4001/quic-v1", dial_script::behavior::terminal_rejection);
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {}};
   auto request = request_for(std::vector<std::string>{"/dnsaddr/mixed.test", "/dnsaddr/quic-only.test"},
                              std::chrono::seconds{1});
   request.max_attempts = 1;
   request.tcp_only = true;
   auto result = forge::asio::blocking::run(runtime, scheduler.async_dial(std::move(request), callbacks_for(script)));
   BOOST_TEST(result.winner.to_string() == "/ip4/8.8.8.8/tcp/4001");
   BOOST_REQUIRE_EQUAL(script->starts().size(), 1U);
   BOOST_REQUIRE_EQUAL(result.root_outcomes.size(), 2U);
   BOOST_CHECK(result.root_outcomes[0].outcome == p2p::dialing::outcome::success);
   BOOST_CHECK(result.root_outcomes[1].outcome == p2p::dialing::outcome::neutral);
   BOOST_TEST(scheduler.black_hole_status().udp.peer_requests == 0U);
   BOOST_TEST(scheduler.black_hole_status().ipv6.peer_requests == 0U);
   BOOST_TEST(scheduler.black_hole_status().udp.outcomes == 0U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_tcp_only_rejects_no_eligible_target_without_attempts) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->text_responses.emplace("_dnsaddr.no-tcp.test", dnsaddr_records({"/ip6/2400::1/udp/4001/quic-v1"}));
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {}};
   auto request = request_for("/dnsaddr/no-tcp.test", std::chrono::seconds{1});
   request.tcp_only = true;
   BOOST_CHECK_EXCEPTION(
       forge::asio::blocking::run(runtime, scheduler.async_dial(std::move(request), callbacks_for(script))),
       forge::exceptions::base, [](const auto& error) {
          return p2p::exceptions::code_of(error) == p2p::exceptions::code::peer_not_found;
       });
   BOOST_TEST(script->starts().empty());
   BOOST_TEST(script->terminal_root_observations() == 0U);
   BOOST_TEST(scheduler.black_hole_status().udp.peer_requests == 0U);
   BOOST_TEST(scheduler.black_hole_status().ipv6.peer_requests == 0U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_prepare_peer_runs_once_with_inferred_identity_before_attempts) {
   const auto expected = p2p::peer_id::from_string("QmcgpsyWgH8Y8ajJz1Cu72KnS5uo2Aa2LpzU7kinSupNKC");
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   const auto first = "/ip6/2400::1/tcp/4001/p2p/" + expected.to_string();
   const auto second = "/ip4/8.8.8.8/tcp/4001/p2p/" + expected.to_string();
   script->text_responses.emplace("_dnsaddr.prepared.test", dnsaddr_records({first, second}));
   script->behaviors.emplace(first, dial_script::behavior::attributable_failure);
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 1}};
   auto calls = std::size_t{};
   auto prepared_peer = std::optional<p2p::peer_id>{};
   auto starts_before_prepare = std::size_t{};
   auto callbacks = callbacks_for(script);
   callbacks.prepare_peer = [&](const std::optional<p2p::peer_id>& peer) {
      ++calls;
      prepared_peer = peer;
      starts_before_prepare = script->starts().size();
   };
   static_cast<void>(forge::asio::blocking::run(
       runtime, scheduler.async_dial(request_for("/dnsaddr/prepared.test/p2p/" + expected.to_string(),
                                                 std::chrono::seconds{1}), std::move(callbacks))));
   BOOST_TEST(calls == 1U);
   BOOST_TEST(starts_before_prepare == 0U);
   BOOST_REQUIRE(prepared_peer.has_value());
   BOOST_TEST(prepared_peer->to_string() == expected.to_string());
   const auto starts = script->starts();
   BOOST_REQUIRE_EQUAL(starts.size(), 2U);
   for (const auto& start : starts) {
      BOOST_REQUIRE(start.expected_peer.has_value());
      BOOST_TEST(start.expected_peer->to_string() == expected.to_string());
   }
}

BOOST_AUTO_TEST_CASE(dial_scheduler_prepare_peer_rejection_is_terminal_without_attempts) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {}};
   auto callbacks = callbacks_for(script);
   auto calls = std::size_t{};
   auto anonymous = false;
   callbacks.prepare_peer = [&](const std::optional<p2p::peer_id>& peer) {
      ++calls;
      anonymous = !peer.has_value();
      FORGE_THROW_EXCEPTION(p2p::exceptions::connection_rejected, "scripted logical peer rejection");
   };
   BOOST_CHECK_EXCEPTION(
       forge::asio::blocking::run(runtime, scheduler.async_dial(
           request_for(std::vector<std::string>{"/ip6/2400::1/tcp/4001", "/ip4/8.8.8.8/tcp/4001"},
                       std::chrono::seconds{1}), std::move(callbacks))),
       forge::exceptions::base, [](const auto& error) {
          return p2p::exceptions::code_of(error) == p2p::exceptions::code::connection_rejected;
       });
   BOOST_TEST(calls == 1U);
   BOOST_TEST(anonymous);
   BOOST_TEST(script->starts().empty());
   BOOST_TEST(script->terminal_root_observations() == 0U);
   forge::asio::blocking::run(runtime, scheduler.async_close());
}

BOOST_AUTO_TEST_CASE(dial_scheduler_rechecks_stop_and_owner_after_prepare_peer) {
   for (const auto mode : {0, 1, 2}) {
      auto runtime = forge::asio::runtime{};
      auto script = std::make_shared<dial_script>();
      auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {}};
      auto stop = std::stop_source{};
      auto calls = std::size_t{};
      auto callbacks = callbacks_for(script);
      callbacks.prepare_peer = [&](const std::optional<p2p::peer_id>&) {
         ++calls;
         if (mode == 0) {
            static_cast<void>(stop.request_stop());
         } else if (mode == 1) {
            script->owner_stopping.store(true, std::memory_order_release);
         } else {
            scheduler.request_stop();
         }
      };
      BOOST_CHECK_EXCEPTION(
          forge::asio::blocking::run(runtime, scheduler.async_dial(
              request_for("/ip4/8.8.8.8/tcp/4001", std::chrono::seconds{1}, {}, stop.get_token()),
              std::move(callbacks))),
          forge::exceptions::base, [mode](const auto& error) {
             return p2p::exceptions::code_of(error) ==
                    (mode == 0 ? p2p::exceptions::code::canceled : p2p::exceptions::code::closed);
          });
      BOOST_TEST(calls == 1U);
      BOOST_TEST(script->starts().empty());
      BOOST_TEST(script->terminal_root_observations() == 0U);
      forge::asio::blocking::run(runtime, scheduler.async_close());
   }
}

BOOST_AUTO_TEST_CASE(dial_scheduler_address_gater_rejection_allows_sibling_without_detector_penalty) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->behaviors.emplace("/ip6/2400::1/udp/4001/quic-v1", dial_script::behavior::address_rejection);
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 1}};
   auto result = forge::asio::blocking::run(runtime, scheduler.async_dial(
       request_for(std::vector<std::string>{"/ip6/2400::1/udp/4001/quic-v1", "/ip4/8.8.8.8/udp/4001/quic-v1"},
                   std::chrono::seconds{1}), callbacks_for(script)));
   BOOST_REQUIRE_EQUAL(script->starts().size(), 2U);
   BOOST_REQUIRE_EQUAL(result.root_outcomes.size(), 2U);
   BOOST_CHECK(result.root_outcomes[0].outcome == p2p::dialing::outcome::neutral);
   BOOST_CHECK(result.root_outcomes[1].outcome == p2p::dialing::outcome::success);
   BOOST_TEST(scheduler.black_hole_status().udp.outcomes == 1U);
   BOOST_TEST(scheduler.black_hole_status().ipv6.outcomes == 0U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_all_address_gater_rejections_preserve_typed_error) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->behaviors.emplace("/ip6/2400::1/udp/4001/quic-v1", dial_script::behavior::address_rejection);
   script->behaviors.emplace("/ip4/8.8.8.8/udp/4001/quic-v1", dial_script::behavior::address_rejection);
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 1}};
   BOOST_CHECK_EXCEPTION(
       forge::asio::blocking::run(runtime, scheduler.async_dial(
           request_for(std::vector<std::string>{"/ip6/2400::1/udp/4001/quic-v1", "/ip4/8.8.8.8/udp/4001/quic-v1"},
                       std::chrono::seconds{1}), callbacks_for(script))),
       forge::exceptions::base, [](const auto& error) {
          return p2p::exceptions::code_of(error) == p2p::exceptions::code::connection_rejected;
       });
   BOOST_TEST(script->starts().size() == 2U);
   BOOST_TEST(script->terminal_root_observations() == 0U);
   BOOST_TEST(scheduler.black_hole_status().udp.outcomes == 0U);
   BOOST_TEST(scheduler.black_hole_status().ipv6.outcomes == 0U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_expands_ranks_and_records_attributable_feedback) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->address_responses.emplace("dial.test", addresses({"2400::1", "8.8.8.8"}));
   script->behaviors.emplace("/ip6/2400::1/udp/4001/quic-v1", dial_script::behavior::attributable_failure);
   script->behaviors.emplace("/ip4/8.8.8.8/udp/4001/quic-v1", dial_script::behavior::succeed);
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 1}};
   const auto expected = p2p::peer_id::from_string("QmcgpsyWgH8Y8ajJz1Cu72KnS5uo2Aa2LpzU7kinSupNKC");
   auto callbacks = callbacks_for(script);

   static_cast<void>(forge::asio::blocking::run(
       runtime, scheduler.async_dial(request_for("/dns/dial.test/udp/4001/quic-v1", std::chrono::seconds{1}, expected),
                                     std::move(callbacks))));

   const auto starts = script->starts();
   BOOST_REQUIRE_EQUAL(starts.size(), 2U);
   BOOST_TEST(starts[0].endpoint == "/ip6/2400::1/udp/4001/quic-v1");
   BOOST_TEST(starts[1].endpoint == "/ip4/8.8.8.8/udp/4001/quic-v1");
   BOOST_REQUIRE(starts[0].expected_peer.has_value());
   BOOST_REQUIRE(starts[1].expected_peer.has_value());
   BOOST_TEST(starts[0].expected_peer->to_string() == expected.to_string());
   BOOST_TEST(starts[1].expected_peer->to_string() == expected.to_string());
   BOOST_CHECK(starts[0].deadline != std::chrono::steady_clock::time_point::max());

   const auto detector = scheduler.black_hole_status();
   BOOST_TEST(detector.udp.outcomes == 2U);
   BOOST_TEST(detector.udp.successes == 1U);
   BOOST_TEST(detector.ipv6.outcomes == 1U);
   BOOST_TEST(detector.ipv6.successes == 0U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_launches_a_shared_concrete_target_once_and_credits_all_winner_roots) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->address_responses.emplace("first-winner-root.test", addresses({"8.8.8.8"}));
   script->address_responses.emplace("second-winner-root.test", addresses({"8.8.8.8"}));
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 1}};

   auto result = forge::asio::blocking::run(
       runtime,
       scheduler.async_dial(
           request_for(std::vector<std::string>{"/dns/first-winner-root.test/udp/4001/quic-v1",
                                                "/dns/second-winner-root.test/udp/4001/quic-v1"},
                       std::chrono::seconds{1}),
           callbacks_for(script)));

   const auto starts = script->starts();
   BOOST_REQUIRE_EQUAL(starts.size(), 1U);
   BOOST_TEST(starts.front().endpoint == "/ip4/8.8.8.8/udp/4001/quic-v1");
   BOOST_TEST(result.winner.to_string() == "/ip4/8.8.8.8/udp/4001/quic-v1");
   BOOST_REQUIRE_EQUAL(result.winner_roots.size(), 2U);
   BOOST_TEST(result.winner_roots[0].canonical.to_string() == "/dns/first-winner-root.test/udp/4001/quic-v1");
   BOOST_TEST(result.winner_roots[1].canonical.to_string() == "/dns/second-winner-root.test/udp/4001/quic-v1");
   BOOST_REQUIRE_EQUAL(result.root_outcomes.size(), 2U);
   BOOST_CHECK(result.root_outcomes[0].outcome == p2p::dialing::outcome::success);
   BOOST_CHECK(result.root_outcomes[1].outcome == p2p::dialing::outcome::success);
   const auto detector = scheduler.black_hole_status();
   BOOST_TEST(detector.udp.outcomes == 1U);
   BOOST_TEST(detector.udp.successes == 1U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_keeps_a_dns_failed_sibling_neutral_for_root_attribution) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->address_not_found.insert("failed-root-attribution.test");
   script->address_responses.emplace("winner-root-attribution.test", addresses({"8.8.4.4"}));
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 1}};

   auto result = forge::asio::blocking::run(
       runtime,
       scheduler.async_dial(
           request_for(std::vector<std::string>{"/dns/failed-root-attribution.test/udp/4001/quic-v1",
                                                "/dns/winner-root-attribution.test/udp/4001/quic-v1"},
                       std::chrono::seconds{1}),
           callbacks_for(script)));

   BOOST_REQUIRE_EQUAL(script->starts().size(), 1U);
   BOOST_REQUIRE_EQUAL(result.root_outcomes.size(), 2U);
   BOOST_CHECK(result.root_outcomes[0].outcome == p2p::dialing::outcome::neutral);
   BOOST_CHECK(result.root_outcomes[1].outcome == p2p::dialing::outcome::success);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_marks_each_exhausted_attributable_root_once) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->address_responses.emplace("first-failed-root.test", addresses({"8.8.8.1"}));
   script->address_responses.emplace("second-failed-root.test", addresses({"8.8.8.2"}));
   script->address_responses.emplace("winner-after-failures.test", addresses({"8.8.8.3"}));
   script->behaviors.emplace("/ip4/8.8.8.1/udp/4001/quic-v1", dial_script::behavior::attributable_failure);
   script->behaviors.emplace("/ip4/8.8.8.2/udp/4001/quic-v1", dial_script::behavior::attributable_failure);
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 1}};

   auto result = forge::asio::blocking::run(
       runtime,
       scheduler.async_dial(
           request_for(std::vector<std::string>{"/dns/first-failed-root.test/udp/4001/quic-v1",
                                                "/dns/second-failed-root.test/udp/4001/quic-v1",
                                                "/dns/winner-after-failures.test/udp/4001/quic-v1"},
                       std::chrono::seconds{1}),
           callbacks_for(script)));

   BOOST_REQUIRE_EQUAL(script->starts().size(), 3U);
   BOOST_REQUIRE_EQUAL(result.root_outcomes.size(), 3U);
   BOOST_CHECK(result.root_outcomes[0].outcome == p2p::dialing::outcome::failure);
   BOOST_CHECK(result.root_outcomes[1].outcome == p2p::dialing::outcome::failure);
   BOOST_CHECK(result.root_outcomes[2].outcome == p2p::dialing::outcome::success);
   BOOST_TEST(scheduler.black_hole_status().udp.outcomes == 3U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_keeps_a_root_neutral_when_a_planned_sibling_never_launches) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->address_responses.emplace("partially-launched-root.test", addresses({"2400::1", "8.8.8.9"}));
   script->address_responses.emplace("winner-before-sibling.test", addresses({"8.8.8.8"}));
   script->behaviors.emplace("/ip6/2400::1/udp/4001/quic-v1", dial_script::behavior::attributable_failure);
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 1}};

   auto result = forge::asio::blocking::run(
       runtime,
       scheduler.async_dial(
           request_for(std::vector<std::string>{"/dns/partially-launched-root.test/udp/4001/quic-v1",
                                                "/dns/winner-before-sibling.test/udp/4001/quic-v1"},
                       std::chrono::seconds{1}),
           callbacks_for(script)));

   const auto starts = script->starts();
   BOOST_REQUIRE_EQUAL(starts.size(), 2U);
   BOOST_TEST(starts[0].endpoint == "/ip6/2400::1/udp/4001/quic-v1");
   BOOST_TEST(starts[1].endpoint == "/ip4/8.8.8.8/udp/4001/quic-v1");
   BOOST_REQUIRE_EQUAL(result.root_outcomes.size(), 2U);
   BOOST_CHECK(result.root_outcomes[0].outcome == p2p::dialing::outcome::neutral);
   BOOST_CHECK(result.root_outcomes[1].outcome == p2p::dialing::outcome::success);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_observes_terminal_attributable_root_outcomes_before_rethrowing) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->address_not_found.insert("dns-neutral-terminal-root.test");
   script->address_responses.emplace("first-terminal-root.test", addresses({"8.8.8.1"}));
   script->address_responses.emplace("second-terminal-root.test", addresses({"8.8.8.2"}));
   script->behaviors.emplace("/ip4/8.8.8.1/udp/4001/quic-v1", dial_script::behavior::attributable_failure);
   script->behaviors.emplace("/ip4/8.8.8.2/udp/4001/quic-v1", dial_script::behavior::attributable_failure);
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 1}};

   BOOST_CHECK_EXCEPTION(
       forge::asio::blocking::run(
           runtime,
           scheduler.async_dial(
               request_for(std::vector<std::string>{"/dns/dns-neutral-terminal-root.test/udp/4001/quic-v1",
                                                    "/dns/first-terminal-root.test/udp/4001/quic-v1",
                                                    "/dns/second-terminal-root.test/udp/4001/quic-v1"},
                           std::chrono::seconds{1}),
               callbacks_for(script))),
       forge::exceptions::base, [](const auto& error) {
          return p2p::exceptions::code_of(error) == p2p::exceptions::code::peer_not_found;
       });

   BOOST_TEST(script->terminal_root_observations() == 1U);
   const auto outcomes = script->terminal_root_outcomes();
   BOOST_REQUIRE_EQUAL(outcomes.size(), 3U);
   BOOST_TEST(outcomes[0].root.canonical.to_string() == "/dns/dns-neutral-terminal-root.test/udp/4001/quic-v1");
   BOOST_CHECK(outcomes[0].outcome == p2p::dialing::outcome::neutral);
   BOOST_TEST(outcomes[1].root.canonical.to_string() == "/dns/first-terminal-root.test/udp/4001/quic-v1");
   BOOST_CHECK(outcomes[1].outcome == p2p::dialing::outcome::failure);
   BOOST_TEST(outcomes[2].root.canonical.to_string() == "/dns/second-terminal-root.test/udp/4001/quic-v1");
   BOOST_CHECK(outcomes[2].outcome == p2p::dialing::outcome::failure);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_ranks_multiple_roots_under_one_deadline) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->address_responses.emplace("private-root.test", addresses({"192.168.1.20"}));
   script->address_responses.emplace("public-root.test", addresses({"8.8.4.4"}));
   script->behaviors.emplace("/ip4/192.168.1.20/tcp/4001", dial_script::behavior::wait_for_release_then_succeed);
   script->behaviors.emplace("/ip4/8.8.4.4/tcp/4001", dial_script::behavior::wait_for_release_then_succeed);
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 2}};
   auto request = request_for(std::vector<std::string>{"/dns/private-root.test/tcp/4001",
                                                       "/dns/public-root.test/tcp/4001"},
                              std::chrono::seconds{1});
   const auto logical_deadline = request.logical_deadline;

   auto result = boost::asio::co_spawn(runtime.context().get_executor(),
                                        scheduler.async_dial(std::move(request), callbacks_for(script)),
                                        boost::asio::use_future);
   BOOST_REQUIRE(script->wait_for_starts(2));
   const auto starts = script->starts();
   BOOST_REQUIRE_EQUAL(starts.size(), 2U);
   BOOST_TEST(starts[0].endpoint == "/ip4/192.168.1.20/tcp/4001");
   BOOST_TEST(starts[1].endpoint == "/ip4/8.8.4.4/tcp/4001");
   BOOST_CHECK(starts[0].deadline == logical_deadline);
   BOOST_CHECK(starts[1].deadline == logical_deadline);

   script->release_starts();
   static_cast<void>(result.get());
   BOOST_TEST(script->finished_attempts() == 2U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_applies_peer_inferred_from_one_root_to_all_attempts) {
   const auto expected = p2p::peer_id::from_string("QmcgpsyWgH8Y8ajJz1Cu72KnS5uo2Aa2LpzU7kinSupNKC");
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->behaviors.emplace("/ip4/8.8.8.8/tcp/4001/p2p/" + expected.to_string(),
                             dial_script::behavior::wait_for_release_then_succeed);
   script->behaviors.emplace("/ip4/8.8.4.4/tcp/4001",
                             dial_script::behavior::wait_for_release_then_succeed);
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 2}};

   auto result = boost::asio::co_spawn(
       runtime.context().get_executor(),
       scheduler.async_dial(
           request_for(std::vector<std::string>{
                           "/ip4/8.8.8.8/tcp/4001/p2p/" + expected.to_string(),
                           "/ip4/8.8.4.4/tcp/4001"},
                       std::chrono::seconds{1}),
           callbacks_for(script)),
       boost::asio::use_future);
   BOOST_REQUIRE(script->wait_for_starts(2));
   const auto starts = script->starts();
   BOOST_REQUIRE_EQUAL(starts.size(), 2U);
   BOOST_REQUIRE(starts[0].expected_peer.has_value());
   BOOST_REQUIRE(starts[1].expected_peer.has_value());
   BOOST_TEST(starts[0].expected_peer->to_string() == expected.to_string());
   BOOST_TEST(starts[1].expected_peer->to_string() == expected.to_string());

   script->release_starts();
   static_cast<void>(result.get());
}

BOOST_AUTO_TEST_CASE(dial_scheduler_terminally_discards_late_loser_before_returning_winner) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->address_responses.emplace("race.test", addresses({"192.168.1.1", "8.8.8.8"}));
   script->behaviors.emplace("/ip4/192.168.1.1/tcp/4001", dial_script::behavior::wait_for_cancel_then_succeed);
   script->behaviors.emplace("/ip4/8.8.8.8/tcp/4001", dial_script::behavior::succeed);
   script->block_discards = true;
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 2}};

   auto result = boost::asio::co_spawn(
       runtime.context().get_executor(), scheduler.async_dial(request_for("/dns/race.test/tcp/4001", std::chrono::seconds{1}),
                                               callbacks_for(script)),
       boost::asio::use_future);
   BOOST_REQUIRE(script->wait_for_starts(2));
   BOOST_REQUIRE(script->wait_for_discards(1));
   BOOST_TEST(static_cast<int>(result.wait_for(std::chrono::milliseconds{20})) ==
              static_cast<int>(std::future_status::timeout));
   script->release_discards();
   static_cast<void>(result.get());
   BOOST_TEST(script->discards() == 1U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_deadline_and_stop_cancel_stalled_attempts_without_escape) {
   auto runtime = forge::asio::runtime{};
   auto deadline_script = std::make_shared<dial_script>();
   deadline_script->address_responses.emplace("deadline.test", addresses({"8.8.8.8"}));
   deadline_script->behaviors.emplace("/ip4/8.8.8.8/tcp/4001", dial_script::behavior::wait_for_cancel_then_fail);
   auto deadline_scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 1}};

   BOOST_CHECK_EXCEPTION(
       forge::asio::blocking::run(
           runtime, deadline_scheduler.async_dial(request_for("/dns/deadline.test/tcp/4001", std::chrono::milliseconds{20}),
                                                   callbacks_for(deadline_script))),
       forge::exceptions::base, [](const auto& error) {
          return p2p::exceptions::code_of(error) == p2p::exceptions::code::timeout;
       });
   BOOST_TEST(deadline_script->discards() == 0U);

   auto stop_script = std::make_shared<dial_script>();
   stop_script->address_responses.emplace("stop.test", addresses({"8.8.8.8"}));
   stop_script->behaviors.emplace("/ip4/8.8.8.8/tcp/4001", dial_script::behavior::wait_for_cancel_then_fail);
   auto stop_scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 1}};
   auto source = std::stop_source{};
   auto result = boost::asio::co_spawn(
       runtime.context().get_executor(),
       stop_scheduler.async_dial(request_for("/dns/stop.test/tcp/4001", std::chrono::seconds{1}, {}, source.get_token()),
                                 callbacks_for(stop_script)),
       boost::asio::use_future);
   BOOST_REQUIRE(stop_script->wait_for_starts(1));
   static_cast<void>(source.request_stop());
   BOOST_CHECK_EXCEPTION(static_cast<void>(result.get()), forge::exceptions::base, [](const auto& error) {
      return p2p::exceptions::code_of(error) == p2p::exceptions::code::canceled;
   });
   BOOST_TEST(stop_script->discards() == 0U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_clears_tcp_progress_hold_when_its_attempt_finishes) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->address_responses.emplace("hold.test", addresses({"1.1.1.1", "8.8.8.8"}));
   script->behaviors.emplace("/ip4/1.1.1.1/tcp/4001",
                             dial_script::behavior::delayed_progress_then_attributable_failure);
   script->behaviors.emplace("/ip4/8.8.8.8/tcp/4001", dial_script::behavior::succeed);
   auto scheduler = p2p::detail::dial_scheduler{
       runtime.context().get_executor(),
       {.ranker = {.public_delay = std::chrono::milliseconds{250}, .private_delay = std::chrono::milliseconds{30}},
        .max_concurrent_attempts = 1}};

   static_cast<void>(forge::asio::blocking::run(
       runtime,
       scheduler.async_dial(request_for("/dns/hold.test/tcp/4001", std::chrono::seconds{1}), callbacks_for(script))));

   const auto starts = script->starts();
   BOOST_REQUIRE_EQUAL(starts.size(), 2U);
   BOOST_TEST(starts[0].endpoint == "/ip4/1.1.1.1/tcp/4001");
   BOOST_TEST(starts[1].endpoint == "/ip4/8.8.8.8/tcp/4001");
   BOOST_TEST(std::chrono::duration_cast<std::chrono::milliseconds>(starts[1].started - starts[0].started).count() <
              200);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_filters_black_holes_but_retains_the_periodic_probe) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->address_responses.emplace("black-hole.test", addresses({"8.8.8.8"}));
   script->behaviors.emplace("/ip4/8.8.8.8/udp/4001/quic-v1", dial_script::behavior::attributable_failure);
   auto scheduler = p2p::detail::dial_scheduler{
       runtime.context().get_executor(), {.black_holes = {.window_size = 4, .min_successes = 1}, .max_concurrent_attempts = 1}};

   const auto run_failure = [&] {
      BOOST_CHECK_EXCEPTION(
          forge::asio::blocking::run(
              runtime,
              scheduler.async_dial(request_for("/dns/black-hole.test/udp/4001/quic-v1", std::chrono::seconds{1}),
                                   callbacks_for(script))),
          forge::exceptions::base, [](const auto& error) {
             return p2p::exceptions::code_of(error) == p2p::exceptions::code::peer_not_found;
          });
   };
   for (auto attempt = std::size_t{}; attempt < 4; ++attempt) {
      run_failure();
   }
   BOOST_TEST(static_cast<int>(scheduler.black_hole_status().udp.state) ==
              static_cast<int>(p2p::dialing::black_hole_state::blocked));
   BOOST_TEST(script->starts().size() == 4U);

   for (auto request = std::size_t{}; request < 3; ++request) {
      BOOST_CHECK_EXCEPTION(
          forge::asio::blocking::run(
              runtime,
              scheduler.async_dial(request_for("/dns/black-hole.test/udp/4001/quic-v1", std::chrono::seconds{1}),
                                   callbacks_for(script))),
          forge::exceptions::base, [](const auto& error) {
             return p2p::exceptions::code_of(error) == p2p::exceptions::code::peer_not_found;
          });
   }
   BOOST_TEST(script->starts().size() == 4U);
   run_failure();
   BOOST_TEST(script->starts().size() == 5U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_udp_and_ipv6_tcp_recover_after_successful_black_hole_probe) {
   for (const auto udp : {true, false}) {
      BOOST_TEST_CONTEXT("UDP=" << udp) {
         auto runtime = forge::asio::runtime{};
         auto script = std::make_shared<dial_script>();
         script->resources = std::make_shared<p2p::resource_manager>();
         const auto target = std::string{udp ? "/ip4/8.8.8.8/udp/4001/quic-v1" : "/ip6/2400::1/tcp/4001"};
         const auto root = std::string{udp ? "/dns4/recovery.test/udp/4001/quic-v1" : "/dns6/recovery.test/tcp/4001"};
         script->address_responses.emplace("recovery.test", addresses({udp ? "8.8.8.8" : "2400::1"}));
         script->behaviors.emplace(target, dial_script::behavior::attributable_failure);
         auto scheduler = p2p::detail::dial_scheduler{
             runtime.context().get_executor(),
             {.black_holes = {.window_size = 4, .min_successes = 1}, .max_concurrent_attempts = 1}};
         const auto counter = [&] {
            const auto status = scheduler.black_hole_status();
            return udp ? status.udp : status.ipv6;
         };
         const auto check_released = [&] {
            BOOST_TEST(script->active_attempts() == 0U);
            BOOST_TEST(script->finished_attempts() == script->starts().size());
            const auto resources = script->resources->current();
            BOOST_TEST(resources.system.outbound_connections == 0U);
            BOOST_TEST(resources.system.file_descriptors == 0U);
            BOOST_TEST(resources.active_dials == 0U);
            BOOST_TEST(resources.denied == 0U);
         };
         const auto fail = [&] {
            auto logical = script->resources->reserve_dial();
            BOOST_REQUIRE(logical);
            BOOST_CHECK_EXCEPTION(
                forge::asio::blocking::run(
                    runtime, scheduler.async_dial(request_for(root, std::chrono::seconds{1}), callbacks_for(script))),
                forge::exceptions::base, [](const auto& error) {
                   return p2p::exceptions::code_of(error) == p2p::exceptions::code::peer_not_found;
                });
            BOOST_TEST(script->resources->current().active_dials == 1U);
            logical.reset();
            check_released();
         };
         for (auto failures = std::size_t{1}; failures <= 4; ++failures) {
            fail();
            BOOST_TEST(script->starts().size() == failures);
            BOOST_TEST(counter().outcomes == failures);
            BOOST_TEST(counter().peer_requests == failures);
            BOOST_TEST(counter().successes == 0U);
            BOOST_CHECK(counter().state == (failures < 4 ? p2p::dialing::black_hole_state::probing
                                                        : p2p::dialing::black_hole_state::blocked));
         }
         // Recovery is available now, but only the scheduled probe may launch it.
         script->behaviors[target] = dial_script::behavior::succeed;
         for (auto suppressed = std::size_t{1}; suppressed < 4; ++suppressed) {
            fail();
            BOOST_TEST(script->starts().size() == 4U);
            BOOST_CHECK(counter().state == p2p::dialing::black_hole_state::blocked);
            BOOST_TEST(counter().outcomes == 4U);
            BOOST_TEST(counter().peer_requests == 4U + suppressed);
            BOOST_TEST(counter().next_probe_after == 4U - suppressed);
         }
         for (auto successes = std::size_t{}; successes < 2; ++successes) {
            auto logical = script->resources->reserve_dial();
            BOOST_REQUIRE(logical);
            auto winner = forge::asio::blocking::run(
                runtime, scheduler.async_dial(request_for(root, std::chrono::seconds{1}), callbacks_for(script)));
            BOOST_TEST(script->starts().size() == 5U + successes);
            BOOST_TEST(script->starts().back().endpoint == target);
            BOOST_TEST(script->active_attempts() == 0U);
            BOOST_REQUIRE_EQUAL(winner.winner_roots.size(), 1U);
            BOOST_TEST(winner.winner_roots.front().canonical.to_string() == root);
            BOOST_CHECK(counter().state == p2p::dialing::black_hole_state::probing);
            BOOST_TEST(counter().peer_requests == successes);
            BOOST_TEST(counter().outcomes == successes);
            BOOST_TEST(counter().successes == successes);
            BOOST_TEST(counter().next_probe_after == 0U);
            BOOST_TEST(script->resources->current().system.outbound_connections == 1U);
            BOOST_TEST(script->resources->current().system.file_descriptors == 1U);
            BOOST_TEST(script->resources->current().active_dials == 1U);
            winner.attempt.reset();
            logical.reset();
            check_released();
         }
         const auto status = scheduler.black_hole_status();
         const auto untouched = udp ? status.ipv6 : status.udp;
         BOOST_CHECK(untouched.state == p2p::dialing::black_hole_state::probing);
         BOOST_TEST(untouched.peer_requests == 0U);
         BOOST_TEST(untouched.outcomes == 0U);
         BOOST_TEST(untouched.successes == 0U);
         BOOST_TEST(script->peak_attempts() == 1U);
         BOOST_TEST(script->discards() == 0U);
         forge::asio::blocking::run(runtime, scheduler.async_close());
         check_released();
      }
   }
}

BOOST_AUTO_TEST_CASE(dial_scheduler_never_exceeds_the_configured_concurrent_attempt_bound) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->address_responses.emplace("concurrency.test", addresses({"192.168.1.1", "8.8.8.8"}));
   script->behaviors.emplace("/ip4/192.168.1.1/tcp/4001",
                             dial_script::behavior::wait_for_release_then_attributable_failure);
   script->behaviors.emplace("/ip4/8.8.8.8/tcp/4001", dial_script::behavior::succeed);
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 1}};

   auto result = boost::asio::co_spawn(
       runtime.context().get_executor(), scheduler.async_dial(request_for("/dns/concurrency.test/tcp/4001", std::chrono::seconds{1}),
                                               callbacks_for(script)),
       boost::asio::use_future);
   BOOST_REQUIRE(script->wait_for_starts(1));
   BOOST_REQUIRE(script->wait_for_active_attempts(1));
   BOOST_TEST(script->starts().size() == 1U);
   BOOST_TEST(script->peak_attempts() == 1U);

   script->release_starts();
   static_cast<void>(result.get());

   const auto starts = script->starts();
   BOOST_REQUIRE_EQUAL(starts.size(), 2U);
   BOOST_TEST(starts[0].endpoint == "/ip4/192.168.1.1/tcp/4001");
   BOOST_TEST(starts[1].endpoint == "/ip4/8.8.8.8/tcp/4001");
   BOOST_TEST(script->peak_attempts() == 1U);
   BOOST_TEST(script->active_attempts() == 0U);
   BOOST_TEST(script->finished_attempts() == 2U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_starts_the_next_eligible_item_early_after_an_attributable_failure) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->address_responses.emplace("early.test", addresses({"2400::1", "8.8.8.8"}));
   script->behaviors.emplace("/ip6/2400::1/udp/4001/quic-v1", dial_script::behavior::attributable_failure);
   script->behaviors.emplace("/ip4/8.8.8.8/udp/4001/quic-v1", dial_script::behavior::succeed);
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 1}};

   static_cast<void>(forge::asio::blocking::run(
       runtime,
       scheduler.async_dial(request_for("/dns/early.test/udp/4001/quic-v1", std::chrono::seconds{1}), callbacks_for(script))));

   const auto starts = script->starts();
   BOOST_REQUIRE_EQUAL(starts.size(), 2U);
   BOOST_TEST(starts[0].endpoint == "/ip6/2400::1/udp/4001/quic-v1");
   BOOST_TEST(starts[1].endpoint == "/ip4/8.8.8.8/udp/4001/quic-v1");
   BOOST_TEST(std::chrono::duration_cast<std::chrono::milliseconds>(starts[1].started - starts[0].started).count() <
              200);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_deadline_and_parent_stop_join_every_started_attempt) {
   auto runtime = forge::asio::runtime{};
   auto deadline_script = std::make_shared<dial_script>();
   deadline_script->address_responses.emplace("joined-deadline.test", addresses({"192.168.1.1", "8.8.8.8"}));
   deadline_script->behaviors.emplace("/ip4/192.168.1.1/tcp/4001", dial_script::behavior::wait_for_cancel_then_fail);
   deadline_script->behaviors.emplace("/ip4/8.8.8.8/tcp/4001", dial_script::behavior::wait_for_cancel_then_fail);
   auto deadline_scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 2}};

   auto deadline_result = boost::asio::co_spawn(
       runtime.context().get_executor(),
       deadline_scheduler.async_dial(request_for("/dns/joined-deadline.test/tcp/4001", std::chrono::milliseconds{100}),
                                     callbacks_for(deadline_script)),
       boost::asio::use_future);
   BOOST_REQUIRE(deadline_script->wait_for_starts(2));
   BOOST_REQUIRE(deadline_script->wait_for_active_attempts(2));
   BOOST_CHECK_EXCEPTION(static_cast<void>(deadline_result.get()), forge::exceptions::base, [](const auto& error) {
      return p2p::exceptions::code_of(error) == p2p::exceptions::code::timeout;
   });
   BOOST_TEST(deadline_script->active_attempts() == 0U);
   BOOST_TEST(deadline_script->finished_attempts() == 2U);

   auto stop_script = std::make_shared<dial_script>();
   stop_script->address_responses.emplace("joined-stop.test", addresses({"192.168.1.1", "8.8.8.8"}));
   stop_script->behaviors.emplace("/ip4/192.168.1.1/tcp/4001", dial_script::behavior::wait_for_cancel_then_fail);
   stop_script->behaviors.emplace("/ip4/8.8.8.8/tcp/4001", dial_script::behavior::wait_for_cancel_then_fail);
   auto stop_scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 2}};
   auto source = std::stop_source{};

   auto stop_result = boost::asio::co_spawn(
       runtime.context().get_executor(),
       stop_scheduler.async_dial(
           request_for("/dns/joined-stop.test/tcp/4001", std::chrono::seconds{1}, {}, source.get_token()),
           callbacks_for(stop_script)),
       boost::asio::use_future);
   BOOST_REQUIRE(stop_script->wait_for_starts(2));
   BOOST_REQUIRE(stop_script->wait_for_active_attempts(2));
   static_cast<void>(source.request_stop());
   BOOST_CHECK_EXCEPTION(static_cast<void>(stop_result.get()), forge::exceptions::base, [](const auto& error) {
      return p2p::exceptions::code_of(error) == p2p::exceptions::code::canceled;
   });
   BOOST_TEST(stop_script->active_attempts() == 0U);
   BOOST_TEST(stop_script->finished_attempts() == 2U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_inherited_cancellation_stops_stalled_attempt_and_joins_cleanup) {
   auto context = boost::asio::io_context{};
   const auto poll = [&] {
      context.restart();
      static_cast<void>(context.poll());
   };
   auto signal = boost::asio::cancellation_signal{};
   auto script = std::make_shared<dial_script>();
   script->resources = std::make_shared<p2p::resource_manager>();
   script->behaviors.emplace("/ip4/8.8.8.8/tcp/4001", dial_script::behavior::wait_for_cancel_then_succeed);
   script->block_attempt_terminal_after_stop = true;
   script->block_discards = true;
   auto scheduler = p2p::detail::dial_scheduler{context.get_executor(), {.max_concurrent_attempts = 1}};
   // The deadline only bounds the old-source busy loop, never releases a script gate.
   auto request = request_for("/ip4/8.8.8.8/tcp/4001", std::chrono::seconds{2});
   const auto deadline = request.logical_deadline;
   auto result = boost::asio::co_spawn(
       context, scheduler.async_dial(std::move(request), callbacks_for(script)),
       boost::asio::bind_cancellation_slot(
           signal.slot(), boost::asio::bind_executor(context.get_executor(), boost::asio::use_future)));
   poll();
   BOOST_REQUIRE_EQUAL(script->starts().size(), 1U);
   BOOST_TEST(script->active_attempts() == 1U);
   BOOST_TEST(script->resources->current().system.file_descriptors == 1U);

   // No executor thread runs during emit: the inherited wait is already parked.
   signal.emit(boost::asio::cancellation_type::terminal);
   poll();
   BOOST_CHECK(std::chrono::steady_clock::now() < deadline);
   BOOST_TEST(script->attempt_terminal_waiting());
   BOOST_TEST(script->active_attempts() == 1U);
   BOOST_CHECK(result.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);

   // Close is a join witness only; cancellation above must already have stopped the attempt.
   auto closed = boost::asio::co_spawn(
       context, scheduler.async_close(), boost::asio::bind_executor(context.get_executor(), boost::asio::use_future));
   poll();
   BOOST_CHECK(closed.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);
   script->release_attempt_terminal();
   poll();
   BOOST_TEST(script->active_attempts() == 0U);
   BOOST_TEST(script->finished_attempts() == 1U);
   BOOST_TEST(script->discards() == 1U);
   BOOST_TEST(script->resources->current().system.outbound_connections == 1U);
   BOOST_TEST(script->resources->current().system.file_descriptors == 1U);
   BOOST_CHECK(result.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);
   BOOST_CHECK(closed.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);

   script->release_discards();
   poll();
   BOOST_REQUIRE(result.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready);
   BOOST_CHECK_EXCEPTION(static_cast<void>(result.get()), forge::exceptions::base, [](const auto& error) {
      return p2p::exceptions::code_of(error) == p2p::exceptions::code::canceled;
   });
   BOOST_REQUIRE(closed.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready);
   closed.get();
   BOOST_TEST(script->resources->current().system.outbound_connections == 0U);
   BOOST_TEST(script->resources->current().system.file_descriptors == 0U);
   BOOST_TEST(script->terminal_root_observations() == 0U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_inherited_cancellation_before_winner_selection_discards_completion) {
   auto context = boost::asio::io_context{};
   const auto poll = [&] {
      context.restart();
      static_cast<void>(context.poll());
   };
   auto signal = boost::asio::cancellation_signal{};
   auto script = std::make_shared<dial_script>();
   script->resources = std::make_shared<p2p::resource_manager>();
   script->behaviors.emplace("/ip4/8.8.8.8/tcp/4001", dial_script::behavior::wait_for_release_then_succeed);
   script->block_discards = true;
   auto callbacks = callbacks_for(script);
   auto emitted = false;
   auto completed_owner_checks = std::size_t{};
   callbacks.is_owner_stopping = [&] noexcept {
      // The worker is terminal and its completion is available. Emit at the
      // final owner check before selection, not while the scheduler is waiting.
      if (!emitted && script->finished_attempts() == 1U && ++completed_owner_checks == 2U) {
         emitted = true;
         signal.emit(boost::asio::cancellation_type::terminal);
      }
      return false;
   };
   auto scheduler = p2p::detail::dial_scheduler{context.get_executor(), {.max_concurrent_attempts = 1}};
   auto result = boost::asio::co_spawn(
       context, scheduler.async_dial(request_for("/ip4/8.8.8.8/tcp/4001", std::chrono::seconds{2}),
                                     std::move(callbacks)),
       boost::asio::bind_cancellation_slot(
           signal.slot(), boost::asio::bind_executor(context.get_executor(), boost::asio::use_future)));
   poll();
   BOOST_REQUIRE_EQUAL(script->starts().size(), 1U);
   script->release_starts();
   poll();
   BOOST_TEST(emitted);
   BOOST_TEST(script->active_attempts() == 0U);
   BOOST_TEST(script->finished_attempts() == 1U);
   BOOST_TEST(script->discards() == 1U);
   BOOST_TEST(script->resources->current().system.file_descriptors == 1U);
   BOOST_CHECK(result.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);

   auto closed = boost::asio::co_spawn(
       context, scheduler.async_close(), boost::asio::bind_executor(context.get_executor(), boost::asio::use_future));
   poll();
   BOOST_CHECK(closed.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);
   script->release_discards();
   poll();
   BOOST_REQUIRE(result.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready);
   BOOST_CHECK_EXCEPTION(static_cast<void>(result.get()), forge::exceptions::base, [](const auto& error) {
      return p2p::exceptions::code_of(error) == p2p::exceptions::code::canceled;
   });
   BOOST_REQUIRE(closed.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready);
   closed.get();
   BOOST_TEST(script->resources->current().system.outbound_connections == 0U);
   BOOST_TEST(script->resources->current().system.file_descriptors == 0U);
   BOOST_TEST(script->terminal_root_observations() == 0U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_inherited_cancellation_after_winner_selection_preserves_winner_and_joins_loser) {
   auto context = boost::asio::io_context{};
   const auto poll = [&] {
      context.restart();
      static_cast<void>(context.poll());
   };
   auto signal = boost::asio::cancellation_signal{};
   auto script = std::make_shared<dial_script>();
   script->resources = std::make_shared<p2p::resource_manager>();
   script->behaviors.emplace("/ip4/192.168.1.1/tcp/4001", dial_script::behavior::wait_for_cancel_then_succeed);
   script->behaviors.emplace("/ip4/8.8.8.8/tcp/4001", dial_script::behavior::wait_for_release_then_succeed);
   script->block_attempt_terminal_after_stop = true;
   script->block_discards = true;
   auto scheduler = p2p::detail::dial_scheduler{context.get_executor(), {.max_concurrent_attempts = 2}};
   auto result = boost::asio::co_spawn(
       context, scheduler.async_dial(
                    request_for(std::vector<std::string>{"/ip4/192.168.1.1/tcp/4001", "/ip4/8.8.8.8/tcp/4001"},
                                std::chrono::seconds{2}),
                    callbacks_for(script)),
       boost::asio::bind_cancellation_slot(
           signal.slot(), boost::asio::bind_executor(context.get_executor(), boost::asio::use_future)));
   poll();
   BOOST_REQUIRE_EQUAL(script->starts().size(), 2U);
   script->release_starts();
   poll();
   BOOST_TEST(script->finished_attempts() == 1U);
   BOOST_TEST(script->active_attempts() == 1U);
   BOOST_TEST(script->attempt_terminal_waiting());
   // The winner has stopped the loser, whose terminal completion is still gated.
   signal.emit(boost::asio::cancellation_type::terminal);
   poll();
   BOOST_CHECK(result.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);
   auto closed = boost::asio::co_spawn(
       context, scheduler.async_close(), boost::asio::bind_executor(context.get_executor(), boost::asio::use_future));
   poll();
   BOOST_CHECK(closed.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);
   script->release_attempt_terminal();
   poll();
   BOOST_TEST(script->active_attempts() == 0U);
   BOOST_TEST(script->finished_attempts() == 2U);
   BOOST_TEST(script->discards() == 1U);
   BOOST_TEST(script->resources->current().system.file_descriptors == 2U);
   BOOST_CHECK(result.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);
   BOOST_CHECK(closed.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);
   script->release_discards();
   poll();
   BOOST_REQUIRE(result.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready);
   auto winner = result.get();
   BOOST_TEST(winner.winner.to_string() == "/ip4/8.8.8.8/tcp/4001");
   BOOST_REQUIRE(closed.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready);
   closed.get();
   BOOST_TEST(script->resources->current().system.outbound_connections == 1U);
   BOOST_TEST(script->resources->current().system.file_descriptors == 1U);
   winner.attempt.reset();
   BOOST_TEST(script->resources->current().system.outbound_connections == 0U);
   BOOST_TEST(script->resources->current().system.file_descriptors == 0U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_neutral_failures_do_not_feed_detectors_or_start_later_candidates) {
   auto runtime = forge::asio::runtime{};
   auto terminal_script = std::make_shared<dial_script>();
   terminal_script->address_responses.emplace("terminal.test", addresses({"2400::1", "8.8.8.8"}));
   terminal_script->behaviors.emplace("/ip6/2400::1/udp/4001/quic-v1", dial_script::behavior::terminal_rejection);
   terminal_script->behaviors.emplace("/ip4/8.8.8.8/udp/4001/quic-v1", dial_script::behavior::succeed);
   auto terminal_scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 1}};

   BOOST_CHECK_EXCEPTION(
       forge::asio::blocking::run(
           runtime,
           terminal_scheduler.async_dial(request_for("/dns/terminal.test/udp/4001/quic-v1", std::chrono::seconds{1}),
                                         callbacks_for(terminal_script))),
       forge::exceptions::base, [](const auto& error) {
          return p2p::exceptions::code_of(error) == p2p::exceptions::code::backpressure_rejected;
       });
   BOOST_TEST(terminal_script->starts().size() == 1U);
   BOOST_TEST(terminal_script->terminal_root_observations() == 0U);
   BOOST_TEST(terminal_scheduler.black_hole_status().udp.outcomes == 0U);
   BOOST_TEST(terminal_scheduler.black_hole_status().ipv6.outcomes == 0U);

   auto canceled_script = std::make_shared<dial_script>();
   canceled_script->address_responses.emplace("neutral-cancel.test", addresses({"2400::1", "8.8.8.8"}));
   canceled_script->behaviors.emplace("/ip6/2400::1/udp/4001/quic-v1",
                                      dial_script::behavior::wait_for_cancel_then_fail);
   canceled_script->behaviors.emplace("/ip4/8.8.8.8/udp/4001/quic-v1", dial_script::behavior::succeed);
   auto canceled_scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 1}};
   auto source = std::stop_source{};
   auto canceled_result = boost::asio::co_spawn(
       runtime.context().get_executor(),
       canceled_scheduler.async_dial(
           request_for("/dns/neutral-cancel.test/udp/4001/quic-v1", std::chrono::seconds{1}, {}, source.get_token()),
           callbacks_for(canceled_script)),
       boost::asio::use_future);
   BOOST_REQUIRE(canceled_script->wait_for_starts(1));
   static_cast<void>(source.request_stop());
   BOOST_CHECK_EXCEPTION(static_cast<void>(canceled_result.get()), forge::exceptions::base, [](const auto& error) {
      return p2p::exceptions::code_of(error) == p2p::exceptions::code::canceled;
   });
   BOOST_TEST(canceled_script->starts().size() == 1U);
   BOOST_TEST(canceled_script->active_attempts() == 0U);
   BOOST_TEST(canceled_script->finished_attempts() == 1U);
   BOOST_TEST(canceled_scheduler.black_hole_status().udp.outcomes == 0U);
   BOOST_TEST(canceled_scheduler.black_hole_status().ipv6.outcomes == 0U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_uses_absolute_private_and_public_launch_offsets) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->address_responses.emplace(
       "planned.test", addresses({"192.168.1.1", "192.168.1.2", "1.1.1.1", "8.8.8.8"}));
   for (const auto endpoint : {"/ip4/192.168.1.1/tcp/4001", "/ip4/192.168.1.2/tcp/4001",
                               "/ip4/1.1.1.1/tcp/4001", "/ip4/8.8.8.8/tcp/4001"}) {
      script->behaviors.emplace(endpoint, dial_script::behavior::wait_for_release_then_succeed);
   }
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 4}};

   const auto submitted = std::chrono::steady_clock::now();
   auto result = boost::asio::co_spawn(
       runtime.context().get_executor(), scheduler.async_dial(request_for("/dns/planned.test/tcp/4001", std::chrono::seconds{1}),
                                               callbacks_for(script)),
       boost::asio::use_future);
   BOOST_REQUIRE(script->wait_for_starts(4));

   const auto starts = script->starts();
   BOOST_REQUIRE_EQUAL(starts.size(), 4U);
   BOOST_TEST(starts[0].endpoint == "/ip4/192.168.1.1/tcp/4001");
   BOOST_TEST(starts[1].endpoint == "/ip4/1.1.1.1/tcp/4001");
   BOOST_TEST(starts[2].endpoint == "/ip4/192.168.1.2/tcp/4001");
   BOOST_TEST(starts[3].endpoint == "/ip4/8.8.8.8/tcp/4001");
   BOOST_TEST(std::chrono::duration_cast<std::chrono::milliseconds>(starts[2].started - submitted).count() >=
              20);
   BOOST_TEST(std::chrono::duration_cast<std::chrono::milliseconds>(starts[3].started - submitted).count() >=
              200);

   script->release_starts();
   static_cast<void>(result.get());
   BOOST_TEST(script->active_attempts() == 0U);
   BOOST_TEST(script->finished_attempts() == 4U);
   BOOST_TEST(script->discards() == 3U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_only_launches_early_after_the_last_active_failure) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->address_responses.emplace("early-after-drain.test", addresses({"192.168.1.1", "1.1.1.1", "8.8.8.8"}));
   script->behaviors.emplace("/ip4/192.168.1.1/tcp/4001",
                             dial_script::behavior::wait_for_release_then_attributable_failure);
   script->behaviors.emplace("/ip4/1.1.1.1/tcp/4001", dial_script::behavior::attributable_failure);
   script->behaviors.emplace("/ip4/8.8.8.8/tcp/4001", dial_script::behavior::succeed);
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 2}};

   auto result = boost::asio::co_spawn(
       runtime.context().get_executor(),
       scheduler.async_dial(request_for("/dns/early-after-drain.test/tcp/4001", std::chrono::seconds{1}),
                            callbacks_for(script)),
       boost::asio::use_future);
   BOOST_REQUIRE(script->wait_for_starts(2));
   BOOST_REQUIRE(script->wait_for_active_attempts(1));

   auto gate = boost::asio::steady_timer{runtime.context()};
   gate.expires_after(std::chrono::milliseconds{75});
   static_cast<void>(gate.async_wait(boost::asio::use_future).get());
   BOOST_TEST(script->starts().size() == 2U);

   script->release_starts();
   static_cast<void>(result.get());
   const auto starts = script->starts();
   BOOST_REQUIRE_EQUAL(starts.size(), 3U);
   BOOST_TEST(starts[2].endpoint == "/ip4/8.8.8.8/tcp/4001");
}

BOOST_AUTO_TEST_CASE(dial_scheduler_uses_the_candidate_attempt_deadline_not_the_logical_deadline) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->address_responses.emplace("candidate-deadline.test", addresses({"8.8.8.8"}));
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 1}};
   auto request = request_for("/dns/candidate-deadline.test/tcp/4001", std::chrono::seconds{1}, {}, {},
                              std::chrono::milliseconds{40});
   const auto logical_deadline = request.logical_deadline;

   static_cast<void>(forge::asio::blocking::run(runtime, scheduler.async_dial(std::move(request), callbacks_for(script))));

   const auto starts = script->starts();
   BOOST_REQUIRE_EQUAL(starts.size(), 1U);
   BOOST_CHECK(starts[0].deadline < logical_deadline);
   BOOST_TEST(std::chrono::duration_cast<std::chrono::milliseconds>(starts[0].deadline - starts[0].started).count() <=
              80);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_clips_unrepresentable_attempt_timeout_to_the_logical_deadline) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->address_responses.emplace("maximum-timeout.test", addresses({"8.8.8.8"}));
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 1}};
   auto request = request_for("/dns/maximum-timeout.test/tcp/4001", std::chrono::seconds{1}, {}, {},
                              std::chrono::milliseconds::max());
   const auto logical_deadline = request.logical_deadline;

   static_cast<void>(forge::asio::blocking::run(runtime, scheduler.async_dial(std::move(request), callbacks_for(script))));

   const auto starts = script->starts();
   BOOST_REQUIRE_EQUAL(starts.size(), 1U);
   BOOST_CHECK(starts[0].deadline == logical_deadline);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_owner_stop_failure_is_neutral_to_black_hole_feedback) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->owner_stopping.store(true, std::memory_order_release);
   script->address_responses.emplace("owner-stop.test", addresses({"2400::1"}));
   script->behaviors.emplace("/ip6/2400::1/udp/4001/quic-v1", dial_script::behavior::attributable_failure);
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 1}};

   BOOST_CHECK_EXCEPTION(
       forge::asio::blocking::run(
           runtime,
           scheduler.async_dial(request_for("/dns/owner-stop.test/udp/4001/quic-v1", std::chrono::seconds{1}),
                                callbacks_for(script))),
       forge::exceptions::base, [](const auto& error) {
          return p2p::exceptions::code_of(error) == p2p::exceptions::code::closed;
       });
   const auto status = scheduler.black_hole_status();
   BOOST_TEST(status.udp.outcomes == 0U);
   BOOST_TEST(status.ipv6.outcomes == 0U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_discards_an_authenticated_winner_when_its_owner_starts_stopping) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->address_responses.emplace("owner-stop-winner.test", addresses({"8.8.8.8"}));
   script->behaviors.emplace("/ip4/8.8.8.8/tcp/4001", dial_script::behavior::wait_for_release_then_succeed);
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 1}};

   auto result = boost::asio::co_spawn(
       runtime.context().get_executor(),
       scheduler.async_dial(request_for("/dns/owner-stop-winner.test/tcp/4001", std::chrono::seconds{1}),
                            callbacks_for(script)),
       boost::asio::use_future);
   BOOST_REQUIRE(script->wait_for_starts(1));
   script->owner_stopping.store(true, std::memory_order_release);
   script->release_starts();

   BOOST_CHECK_EXCEPTION(static_cast<void>(result.get()), forge::exceptions::base, [](const auto& error) {
      return p2p::exceptions::code_of(error) == p2p::exceptions::code::closed;
   });
   BOOST_TEST(script->discards() == 1U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_owned_awaitables_survive_scheduler_destruction_before_they_start) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   auto dial = std::optional<boost::asio::awaitable<p2p::detail::dial_result>>{};
   auto close = std::optional<boost::asio::awaitable<void>>{};

   {
      auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 1}};
      dial.emplace(scheduler.async_dial(request_for("/dns/lazy-lifetime.test/tcp/4001", std::chrono::seconds{1}),
                                        callbacks_for(script)));
      close.emplace(scheduler.async_close());
   }

   auto dial_result = boost::asio::co_spawn(runtime.context().get_executor(), std::move(*dial), boost::asio::use_future);
   auto close_result = boost::asio::co_spawn(runtime.context().get_executor(), std::move(*close), boost::asio::use_future);
   BOOST_CHECK_EXCEPTION(static_cast<void>(dial_result.get()), forge::exceptions::base, [](const auto& error) {
      return p2p::exceptions::code_of(error) == p2p::exceptions::code::closed;
   });
   static_cast<void>(close_result.get());
   BOOST_TEST(script->starts().empty());
}

BOOST_AUTO_TEST_CASE(dial_scheduler_throwing_discard_joins_terminal_worker_before_async_close) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->address_responses.emplace("discard-drain.test", addresses({"192.168.1.1", "8.8.8.8"}));
   script->behaviors.emplace("/ip4/192.168.1.1/tcp/4001", dial_script::behavior::wait_for_cancel_then_succeed);
   script->behaviors.emplace("/ip4/8.8.8.8/tcp/4001", dial_script::behavior::wait_for_release_then_succeed);
   script->block_attempt_terminal_after_stop = true;
   script->throwing_discards = 1;
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 2}};

   auto result = boost::asio::co_spawn(
       runtime.context().get_executor(),
       scheduler.async_dial(request_for("/dns/discard-drain.test/tcp/4001", std::chrono::seconds{1}), callbacks_for(script)),
       boost::asio::use_future);
   BOOST_REQUIRE(script->wait_for_starts(2));
   script->owner_stopping.store(true, std::memory_order_release);
   script->release_starts();
   BOOST_REQUIRE(script->wait_for_discards(1));
   BOOST_REQUIRE(script->wait_for_attempt_terminal_waiting());

   auto close_result =
       boost::asio::co_spawn(runtime.context().get_executor(), scheduler.async_close(), boost::asio::use_future);
   BOOST_TEST(static_cast<int>(close_result.wait_for(std::chrono::milliseconds{20})) ==
              static_cast<int>(std::future_status::timeout));
   script->release_attempt_terminal();

   BOOST_CHECK_EXCEPTION(static_cast<void>(result.get()), forge::exceptions::base, [](const auto& error) {
      return p2p::exceptions::code_of(error) == p2p::exceptions::code::backpressure_rejected;
   });
   static_cast<void>(close_result.get());
   BOOST_TEST(script->active_attempts() == 0U);
   BOOST_TEST(script->finished_attempts() == 2U);
   BOOST_TEST(script->discards() == 3U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_throwing_late_loser_discard_drains_the_selected_winner_before_async_close) {
   auto runtime = forge::asio::runtime{};
   auto script = std::make_shared<dial_script>();
   script->address_responses.emplace("winner-cleanup.test", addresses({"192.168.1.1", "8.8.8.8"}));
   // The private candidate can complete only after the public candidate wins
   // and stops it, making this a late loser rather than the selected winner.
   script->behaviors.emplace("/ip4/192.168.1.1/tcp/4001", dial_script::behavior::wait_for_cancel_then_succeed);
   script->behaviors.emplace("/ip4/8.8.8.8/tcp/4001", dial_script::behavior::succeed);
   script->block_discards = true;
   script->throwing_discards = 1;
   auto scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 2}};

   auto result = boost::asio::co_spawn(
       runtime.context().get_executor(),
       scheduler.async_dial(request_for("/dns/winner-cleanup.test/tcp/4001", std::chrono::seconds{1}), callbacks_for(script)),
       boost::asio::use_future);
   BOOST_REQUIRE(script->wait_for_starts(2));
   // The first late-loser discard throws; the retry remains terminally blocked.
   BOOST_REQUIRE(script->wait_for_discards(2));

   auto close_result =
       boost::asio::co_spawn(runtime.context().get_executor(), scheduler.async_close(), boost::asio::use_future);
   BOOST_TEST(static_cast<int>(close_result.wait_for(std::chrono::milliseconds{20})) ==
              static_cast<int>(std::future_status::timeout));

   script->release_discards();
   // The selected winner is now moved into the same pending-discard drain and
   // must be terminally released before either the dial or close can finish.
   BOOST_REQUIRE(script->wait_for_discards(3));
   BOOST_TEST(static_cast<int>(result.wait_for(std::chrono::milliseconds{20})) ==
              static_cast<int>(std::future_status::timeout));
   BOOST_TEST(static_cast<int>(close_result.wait_for(std::chrono::milliseconds{20})) ==
              static_cast<int>(std::future_status::timeout));

   script->release_discards();
   BOOST_CHECK_EXCEPTION(static_cast<void>(result.get()), forge::exceptions::base, [](const auto& error) {
      return p2p::exceptions::code_of(error) == p2p::exceptions::code::backpressure_rejected;
   });
   static_cast<void>(close_result.get());
   BOOST_TEST(script->active_attempts() == 0U);
   BOOST_TEST(script->finished_attempts() == 2U);
   BOOST_TEST(script->discards() == 3U);
}

BOOST_AUTO_TEST_CASE(dial_scheduler_close_joins_dns_and_attempt_terminal_drain) {
   auto runtime = forge::asio::runtime{};
   auto dns_script = std::make_shared<dial_script>();
   dns_script->block_dns_until_stop = true;
   dns_script->block_dns_terminal_after_stop = true;
   auto dns_scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 1}};

   auto dns_result = boost::asio::co_spawn(
       runtime.context().get_executor(),
       dns_scheduler.async_dial(request_for("/dns/close-dns.test/tcp/4001", std::chrono::seconds{1}),
                                callbacks_for(dns_script)),
       boost::asio::use_future);
   BOOST_REQUIRE(dns_script->wait_for_address_lookups(1));
   auto dns_close_first = boost::asio::co_spawn(runtime.context().get_executor(), dns_scheduler.async_close(), boost::asio::use_future);
   auto dns_close_second = boost::asio::co_spawn(runtime.context().get_executor(), dns_scheduler.async_close(), boost::asio::use_future);
   BOOST_REQUIRE(dns_script->wait_for_dns_terminal_waiting());
   BOOST_TEST(static_cast<int>(dns_close_first.wait_for(std::chrono::milliseconds{20})) ==
              static_cast<int>(std::future_status::timeout));
   BOOST_TEST(static_cast<int>(dns_close_second.wait_for(std::chrono::milliseconds{20})) ==
              static_cast<int>(std::future_status::timeout));
   dns_script->release_dns_terminal();
   BOOST_CHECK_EXCEPTION(static_cast<void>(dns_result.get()), forge::exceptions::base, [](const auto& error) {
      const auto code = p2p::exceptions::code_of(error);
      return code == p2p::exceptions::code::canceled || code == p2p::exceptions::code::closed;
   });
   static_cast<void>(dns_close_first.get());
   static_cast<void>(dns_close_second.get());

   auto attempt_script = std::make_shared<dial_script>();
   attempt_script->block_attempt_terminal_after_stop = true;
   attempt_script->address_responses.emplace("close-attempt.test", addresses({"8.8.8.8"}));
   attempt_script->behaviors.emplace("/ip4/8.8.8.8/tcp/4001", dial_script::behavior::wait_for_cancel_then_fail);
   auto attempt_scheduler = p2p::detail::dial_scheduler{runtime.context().get_executor(), {.max_concurrent_attempts = 1}};

   auto attempt_result = boost::asio::co_spawn(
       runtime.context().get_executor(),
       attempt_scheduler.async_dial(request_for("/dns/close-attempt.test/tcp/4001", std::chrono::seconds{1}),
                                    callbacks_for(attempt_script)),
       boost::asio::use_future);
   BOOST_REQUIRE(attempt_script->wait_for_starts(1));
   auto attempt_close =
       boost::asio::co_spawn(runtime.context().get_executor(), attempt_scheduler.async_close(), boost::asio::use_future);
   BOOST_REQUIRE(attempt_script->wait_for_attempt_terminal_waiting());
   BOOST_TEST(static_cast<int>(attempt_close.wait_for(std::chrono::milliseconds{20})) ==
              static_cast<int>(std::future_status::timeout));
   attempt_script->release_attempt_terminal();
   BOOST_CHECK_EXCEPTION(static_cast<void>(attempt_result.get()), forge::exceptions::base, [](const auto& error) {
      return p2p::exceptions::code_of(error) == p2p::exceptions::code::closed;
   });
   static_cast<void>(attempt_close.get());
}

BOOST_AUTO_TEST_SUITE_END()
