module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/compat/move_only_function.hpp>
#include <boost/system/system_error.hpp>

module forge.net.p2p.node;

import forge.asio.notification;
import forge.exceptions;
import forge.multiformats.multiaddr;
import forge.net.dns.resolver;
import forge.net.dns.types;
import forge.net.p2p.address_resolution;
import forge.net.p2p.dialing;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.resource_manager;
import forge.net.transport.session;

#include "details/cancellation_latch.hxx"
#include "details/dial_scheduler.hxx"
#include "details/peer_failure.hxx"

namespace forge::net::p2p::detail {
namespace {

constexpr auto max_concurrent_attempts = std::size_t{4};

[[noreturn]] void throw_timeout() {
   FORGE_THROW_EXCEPTION(exceptions::timeout, "P2P direct dial scheduler deadline expired");
}

[[noreturn]] void throw_canceled() {
   FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P direct dial scheduler canceled");
}

[[noreturn]] void throw_closed() {
   FORGE_THROW_EXCEPTION(exceptions::closed, "P2P direct dial scheduler closed");
}

[[noreturn]] void throw_no_endpoint() {
   FORGE_THROW_EXCEPTION(exceptions::peer_not_found, "P2P direct dial scheduler found no eligible endpoint");
}

[[nodiscard]] exceptions::code failure_code(const std::exception_ptr& error) noexcept {
   if (!error) {
      return exceptions::code::internal;
   }
   try {
      std::rethrow_exception(error);
   } catch (const forge::exceptions::base& value) {
      return exceptions::code_of(value).value_or(exceptions::code::internal);
   } catch (...) {
      return exceptions::code::internal;
   }
}

[[nodiscard]] bool is_timeout_wait(const std::exception_ptr& error) noexcept {
   if (!error) {
      return false;
   }
   try {
      std::rethrow_exception(error);
   } catch (const boost::system::system_error& value) {
      return value.code() == boost::asio::error::timed_out;
   } catch (...) {
      return false;
   }
}

[[nodiscard]] bool is_operation_aborted_wait(const std::exception_ptr& error) noexcept {
   if (!error) {
      return false;
   }
   try {
      std::rethrow_exception(error);
   } catch (const boost::system::system_error& value) {
      return value.code() == boost::asio::error::operation_aborted;
   } catch (...) {
      return false;
   }
}

[[nodiscard]] dial_scheduler::clock::time_point
start_at(const dial_scheduler::clock::time_point started, const dial_plan_item& item) noexcept {
   const auto delay = std::chrono::duration_cast<dial_scheduler::clock::duration>(item.delay);
   const auto maximum = dial_scheduler::clock::time_point::max();
   if (delay.count() <= 0 || started >= maximum - delay) {
      return delay.count() <= 0 ? started : maximum;
   }
   return started + delay;
}

[[nodiscard]] boost::asio::awaitable<dns_address_expansion_result>
async_expand_with_callbacks(const dial_scheduler::policy& policy,
                            const std::shared_ptr<dial_scheduler::operation_callbacks>& callbacks,
                            std::vector<forge::multiformats::multiaddr> roots, std::optional<peer_id> expected_peer,
                            dial_scheduler::clock::time_point deadline, std::stop_token stop) {
   auto expander = dns_address_expander{
       policy.resolution,
       {.resolve_addresses =
            [callbacks](std::string name, forge::net::dns::address_family family,
                        forge::net::dns::query_options options, std::stop_token lookup_stop) {
               return callbacks->resolve_addresses(std::move(name), family, std::move(options), lookup_stop);
            },
        .resolve_txt =
            [callbacks](std::string name, forge::net::dns::query_options options, std::stop_token lookup_stop) {
               return callbacks->resolve_txt(std::move(name), std::move(options), lookup_stop);
            }}};
   co_return co_await expander.async_expand(std::move(roots), std::move(expected_peer), deadline, stop);
}

[[nodiscard]] std::vector<resolved_dial_target>
allowed_targets(std::vector<resolved_dial_target> targets, std::vector<endpoint> allowed) {
   auto result = std::vector<resolved_dial_target>{};
   result.reserve(allowed.size());
   auto target_indices = std::map<std::string, std::size_t>{};
   for (auto index = std::size_t{}; index < targets.size(); ++index) {
      target_indices.emplace(targets[index].concrete.to_string(), index);
   }
   for (const auto& value : allowed) {
      const auto key = value.to_string();
      if (const auto found = target_indices.find(key); found != target_indices.end()) {
         result.push_back(std::move(targets[found->second]));
         target_indices.erase(found);
      }
   }
   return result;
}

[[nodiscard]] bool has_root_index(const std::vector<std::size_t>& root_indices, std::size_t index) noexcept {
   return std::binary_search(root_indices.begin(), root_indices.end(), index);
}

[[nodiscard]] std::vector<dial_root_outcome>
root_outcomes_for(const std::vector<source_root>& roots, const std::vector<dial_plan_item>& plan,
                  const std::vector<bool>& launched, const std::vector<bool>& attributable_failures,
                  std::optional<std::size_t> winner_plan_index) {
   auto result = std::vector<dial_root_outcome>{};
   result.reserve(roots.size());
   const auto* winner_root_indices = winner_plan_index ? &plan[*winner_plan_index].root_indices : nullptr;
   for (auto root_index = std::size_t{}; root_index < roots.size(); ++root_index) {
      auto outcome = dialing::outcome::neutral;
      if (winner_root_indices && has_root_index(*winner_root_indices, root_index)) {
         outcome = dialing::outcome::success;
      } else {
         auto has_planned_target = false;
         auto all_planned_targets_failed = true;
         for (auto plan_index = std::size_t{}; plan_index < plan.size(); ++plan_index) {
            if (!has_root_index(plan[plan_index].root_indices, root_index)) {
               continue;
            }
            has_planned_target = true;
            all_planned_targets_failed =
                all_planned_targets_failed && launched[plan_index] && attributable_failures[plan_index];
         }
         if (has_planned_target && all_planned_targets_failed) {
            outcome = dialing::outcome::failure;
         }
      }
      result.push_back({.root = roots[root_index], .outcome = outcome});
   }
   return result;
}

[[nodiscard]] bool has_attributable_exhaustion(const std::vector<bool>& launched,
                                              const std::vector<bool>& attributable_failures) noexcept {
   auto any_launched = false;
   for (auto index = std::size_t{}; index < launched.size(); ++index) {
      if (!launched[index]) {
         continue;
      }
      any_launched = true;
      if (!attributable_failures[index]) {
         return false;
      }
   }
   return any_launched;
}

} // namespace

dial_scheduler::state::state()
    : cancellation_{std::make_shared<cancellation_latch>()}, wakeup_{std::make_shared<forge::asio::notification>()} {}

void dial_scheduler::state::prepare(std::size_t capacity) {
   const auto lock = std::scoped_lock{mutex_};
   completions_.resize(capacity);
   completion_order_.reserve(capacity);
   tcp_handshake_holds_.assign(capacity, clock::time_point{});
   completed_.assign(capacity, false);
}

void dial_scheduler::state::launch() noexcept {
   const auto lock = std::scoped_lock{mutex_};
   ++active_;
}

void dial_scheduler::state::publish(completion value) noexcept {
   auto wakeup = std::shared_ptr<forge::asio::notification>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      const auto index = value.plan_index;
      if (index >= completions_.size() || completed_[index]) {
         return;
      }
      // Capacity is prepared before workers launch, so this handoff cannot allocate.
      completed_[index] = true;
      tcp_handshake_holds_[index] = {};
      completions_[index].emplace(std::move(value));
      completion_order_.push_back(index);
      if (active_ != 0) {
         --active_;
      }
      wakeup = wakeup_;
   }
   wakeup->notify();
}

std::optional<dial_scheduler::completion> dial_scheduler::state::take_completion() {
   const auto lock = std::scoped_lock{mutex_};
   if (next_completion_ == completion_order_.size()) {
      return std::nullopt;
   }
   const auto index = completion_order_[next_completion_++];
   auto result = std::move(completions_[index]);
   completions_[index].reset();
   return result;
}

std::size_t dial_scheduler::state::active() const noexcept {
   const auto lock = std::scoped_lock{mutex_};
   return active_;
}

bool dial_scheduler::state::has_completion() const noexcept {
   const auto lock = std::scoped_lock{mutex_};
   return next_completion_ != completion_order_.size();
}

dial_scheduler::clock::time_point dial_scheduler::state::tcp_handshake_hold_until() const noexcept {
   const auto lock = std::scoped_lock{mutex_};
   auto result = clock::time_point{};
   for (const auto hold : tcp_handshake_holds_) {
      result = std::max(result, hold);
   }
   return result;
}

std::shared_ptr<cancellation_latch> dial_scheduler::state::cancellation() const noexcept {
   return cancellation_;
}

std::stop_token dial_scheduler::state::stop_token() const noexcept {
   return stop_source_.get_token();
}

std::shared_ptr<forge::asio::notification> dial_scheduler::state::wakeup() const noexcept {
   return wakeup_;
}

bool dial_scheduler::state::stop_requested() const noexcept {
   const auto lock = std::scoped_lock{mutex_};
   return stopped_;
}

void dial_scheduler::state::report_tcp_handshake_progress(std::size_t plan_index, std::chrono::milliseconds hold) noexcept {
   if (hold.count() <= 0) {
      return;
   }
   const auto now = clock::now();
   const auto duration = std::chrono::duration_cast<clock::duration>(hold);
   const auto maximum = clock::time_point::max();
   const auto until = now >= maximum - duration ? maximum : now + duration;
   auto wakeup = std::shared_ptr<forge::asio::notification>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      if (plan_index >= tcp_handshake_holds_.size() || completed_[plan_index]) {
         return;
      }
      tcp_handshake_holds_[plan_index] = std::max(tcp_handshake_holds_[plan_index], until);
      wakeup = wakeup_;
   }
   wakeup->notify();
}

void dial_scheduler::state::request_stop() noexcept {
   auto cancellation = std::shared_ptr<cancellation_latch>{};
   auto wakeup = std::shared_ptr<forge::asio::notification>{};
   auto request_stop = false;
   {
      const auto lock = std::scoped_lock{mutex_};
      if (!stopped_) {
         stopped_ = true;
         request_stop = true;
      }
      cancellation = cancellation_;
      wakeup = wakeup_;
   }
   if (request_stop) {
      static_cast<void>(stop_source_.request_stop());
   }
   cancellation->request_stop();
   wakeup->notify();
}

dial_scheduler::operation_registry::operation_registry() : changed_{std::make_shared<forge::asio::notification>()} {}

bool dial_scheduler::operation_registry::admit(const std::shared_ptr<state>& operation) {
   const auto lock = std::scoped_lock{mutex_};
   if (sealed_) {
      return false;
   }
   operations_.insert(operation);
   return true;
}

void dial_scheduler::operation_registry::retire(const std::shared_ptr<state>& operation) noexcept {
   auto changed = std::shared_ptr<forge::asio::notification>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      operations_.erase(operation);
      changed = changed_;
   }
   changed->notify();
}

void dial_scheduler::operation_registry::request_stop() noexcept {
   auto changed = std::shared_ptr<forge::asio::notification>{};
   auto operation = std::shared_ptr<state>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      sealed_ = true;
      if (!operations_.empty()) {
         operation = *operations_.begin();
      }
      changed = changed_;
   }
   while (operation) {
      operation->request_stop();
      const auto lock = std::scoped_lock{mutex_};
      const auto next = operations_.upper_bound(operation);
      operation = next == operations_.end() ? std::shared_ptr<state>{} : *next;
   }
   changed->notify();
}

boost::asio::awaitable<void> dial_scheduler::operation_registry::async_wait_empty() {
   for (;;) {
      auto changed = std::shared_ptr<forge::asio::notification>{};
      {
         const auto lock = std::scoped_lock{mutex_};
         if (operations_.empty()) {
            co_return;
         }
         changed = changed_;
      }
      const auto observed = changed->epoch();
      {
         const auto lock = std::scoped_lock{mutex_};
         if (operations_.empty()) {
            co_return;
         }
      }
      static_cast<void>(co_await changed->async_wait(observed));
   }
}

dial_scheduler::operation_ticket::operation_ticket(std::shared_ptr<operation_registry> registry,
                                                     std::shared_ptr<state> operation) noexcept
    : registry_(std::move(registry)), operation_(std::move(operation)) {}

dial_scheduler::operation_ticket::~operation_ticket() {
   registry_->retire(operation_);
}

dial_scheduler::owner::owner(boost::asio::any_io_executor executor, policy policy_value,
                              forge::net::dns::resolver_options resolver_options)
    : policy_{std::move(policy_value)}, resolver_{std::move(executor), std::move(resolver_options)},
      expander_{resolver_, policy_.resolution}, ranker_{policy_.ranker}, black_holes_{policy_.black_holes},
      operations_{std::make_shared<operation_registry>()} {
   dial_scheduler::validate_policy(policy_);
}

dial_scheduler::dial_scheduler(boost::asio::any_io_executor executor, policy policy_value,
                               forge::net::dns::resolver_options resolver_options)
    : owner_{std::make_shared<owner>(std::move(executor), std::move(policy_value), std::move(resolver_options))} {}

dial_scheduler::~dial_scheduler() {
   request_stop();
   if (owner_) {
      owner_->resolver_.request_cancel();
   }
}

void dial_scheduler::request_stop() noexcept {
   if (owner_) {
      owner_->operations_->request_stop();
   }
}

boost::asio::awaitable<void> dial_scheduler::async_close() {
   auto owner = owner_;
   return async_close_owned(std::move(owner));
}

boost::asio::awaitable<void> dial_scheduler::async_close_owned(std::shared_ptr<owner> owner) {
   if (!owner) {
      co_return;
   }
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   owner->operations_->request_stop();
   co_await owner->operations_->async_wait_empty();
   co_await owner->resolver_.async_close();
}

dialing::black_hole_status dial_scheduler::black_hole_status() const {
   return owner_->black_holes_.status();
}

bool dial_scheduler::attributable_failure(const std::exception_ptr& error, bool owner_is_stopping) noexcept {
   return remote_peer_attributable_failure(failure_code(error), owner_is_stopping);
}

bool dial_scheduler::is_canceled(const std::stop_token& stop) noexcept {
   return stop.stop_requested();
}

dial_scheduler::clock::time_point dial_scheduler::candidate_deadline(clock::time_point logical_deadline,
                                                                       std::chrono::milliseconds timeout,
                                                                       clock::time_point launched) noexcept {
   // Clip durations that cannot be represented by the scheduler clock before
   // converting them. The finite logical deadline is already the tighter bound.
   constexpr auto maximum_clock_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(clock::duration::max());
   if (timeout >= maximum_clock_timeout) {
      return logical_deadline;
   }
   const auto duration = std::chrono::duration_cast<clock::duration>(timeout);
   const auto maximum = clock::time_point::max();
   const auto attempt_deadline = launched >= maximum - duration ? maximum : launched + duration;
   return std::min(logical_deadline, attempt_deadline);
}

void dial_scheduler::validate_policy(const policy& value) {
   dial_ranker::validate_policy(value.ranker);
   black_hole_detector::validate_policy(value.black_holes);
   if (value.max_concurrent_attempts == 0 || value.max_concurrent_attempts > max_concurrent_attempts) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "P2P direct dial scheduler concurrency exceeds its immutable bound");
   }
}

void dial_scheduler::validate_request(const request& value) {
   if (value.max_attempts == 0 || value.max_attempts > request{}.max_attempts) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "P2P direct dial scheduler attempt count exceeds its positive bound");
   }
   if (value.roots.empty() || value.logical_deadline == clock::time_point::max() ||
       value.attempt_timeout <= std::chrono::milliseconds::zero()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "P2P direct dial scheduler requires finite positive operation time bounds");
   }
}

boost::asio::awaitable<dial_result> dial_scheduler::async_dial(request value, operation_callbacks callbacks) {
   auto owner = owner_;
   return async_dial_owned(std::move(owner), std::move(value), std::move(callbacks));
}

boost::asio::awaitable<void>
dial_scheduler::async_attempt_worker(std::shared_ptr<operation_callbacks> callbacks, std::shared_ptr<state> operation,
                                     std::optional<peer_id> expected_peer, clock::time_point deadline, dial_plan_item item,
                                     std::size_t index) {
   auto result = completion{.plan_index = index, .target = item.value};
   auto reported = std::make_shared<std::atomic_bool>(false);
   auto progress = direct::tcp_transport_progress_handler{};
   if (item.tcp && item.tcp_handshake_progress_hold.count() > 0) {
      progress = [operation, index, hold = item.tcp_handshake_progress_hold, reported] noexcept {
         if (!reported->exchange(true, std::memory_order_acq_rel)) {
            operation->report_tcp_handshake_progress(index, hold);
         }
      };
   }
   try {
      result.attempt.emplace(co_await callbacks->start_attempt(
          item.value, std::move(expected_peer), deadline, operation->cancellation(), std::move(progress)));
   } catch (...) {
      result.error = std::current_exception();
      result.attributable = attributable_failure(result.error, operation->stop_requested() || callbacks->is_owner_stopping());
   }
   operation->publish(std::move(result));
}

boost::asio::awaitable<void>
dial_scheduler::async_discard_preserving(std::shared_ptr<operation_callbacks> callbacks,
                                         std::vector<direct_attempt>& pending, direct_attempt attempt) {
   pending.emplace_back(std::move(attempt));
   co_await callbacks->discard_attempt(pending.back());
   pending.pop_back();
}

boost::asio::awaitable<std::exception_ptr>
dial_scheduler::async_drain_pending_discards(std::shared_ptr<operation_callbacks> callbacks,
                                             std::vector<direct_attempt>& pending) {
   auto first_error = std::exception_ptr{};
   while (!pending.empty()) {
      try {
         co_await callbacks->discard_attempt(pending.back());
      } catch (...) {
         if (!first_error) {
            first_error = std::current_exception();
         }
      }
      pending.pop_back();
   }
   co_return first_error;
}

boost::asio::awaitable<dial_result>
dial_scheduler::async_dial_owned(std::shared_ptr<owner> owner, request value, operation_callbacks callbacks) {
   if (!owner) {
      throw_closed();
   }
   validate_request(value);
   auto callback_set = std::make_shared<operation_callbacks>(std::move(callbacks));
   const auto has_resolver_callbacks = static_cast<bool>(callback_set->resolve_addresses) ||
                                       static_cast<bool>(callback_set->resolve_txt);
   if ((!callback_set->start_attempt || !callback_set->discard_attempt || !callback_set->is_owner_stopping ||
        !callback_set->observe_terminal_root_outcomes) ||
       (has_resolver_callbacks && (!callback_set->resolve_addresses || !callback_set->resolve_txt))) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P direct dial operation callbacks must be complete");
   }
   if (is_canceled(value.stop)) {
      throw_canceled();
   }
   if (clock::now() >= value.logical_deadline) {
      throw_timeout();
   }

   auto operation = std::make_shared<state>();
   if (!owner->operations_->admit(operation)) {
      throw_closed();
   }
   const auto ticket = operation_ticket{owner->operations_, operation};
   auto stop_callback = std::stop_callback{value.stop, [operation] noexcept { operation->request_stop(); }};
   if (is_canceled(value.stop)) {
      throw_canceled();
   }
   if (operation->stop_requested() || callback_set->is_owner_stopping()) {
      throw_closed();
   }

   auto expansion = dns_address_expansion_result{};
   if (has_resolver_callbacks) {
      expansion = co_await async_expand_with_callbacks(owner->policy_, callback_set, std::move(value.roots),
                                                        value.expected_peer, value.logical_deadline,
                                                        operation->stop_token());
   } else {
      expansion = co_await owner->expander_.async_expand(std::move(value.roots), value.expected_peer,
                                                          value.logical_deadline, operation->stop_token());
   }
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   const auto parent_canceled = [&] noexcept {
      return is_canceled(value.stop) || cancellation.cancelled() != boost::asio::cancellation_type::none;
   };
   if (parent_canceled()) {
      throw_canceled();
   }
   if (clock::now() >= value.logical_deadline) {
      throw_timeout();
   }
   if (operation->stop_requested() || callback_set->is_owner_stopping()) {
      throw_closed();
   }
   if (callback_set->prepare_peer) {
      callback_set->prepare_peer(expansion.expected_peer);
   }
   if (parent_canceled()) {
      throw_canceled();
   }
   if (operation->stop_requested() || callback_set->is_owner_stopping()) {
      throw_closed();
   }
   if (clock::now() >= value.logical_deadline) {
      throw_timeout();
   }

   auto concrete = std::vector<endpoint>{};
   concrete.reserve(expansion.targets.size());
   for (const auto& target : expansion.targets) {
      if (!value.tcp_only || target.concrete.is_direct_tcp()) {
         concrete.push_back(target.concrete);
      }
   }
   auto filtered = owner->black_holes_.filter_peer_dial(std::move(concrete));
   auto plan = owner->ranker_.rank(allowed_targets(std::move(expansion.targets), std::move(filtered.allowed)));
   if (plan.empty()) {
      throw_no_endpoint();
   }
   operation->prepare(plan.size());

   auto executor = boost::asio::any_io_executor{co_await boost::asio::this_coro::executor};
   auto wakeup = operation->wakeup();
   const auto started = clock::now();
   auto launched = std::vector<bool>(plan.size(), false);
   auto attributable_failures = std::vector<bool>(plan.size(), false);
   auto launched_count = std::size_t{};
   auto launch_early = false;
   auto winner = std::optional<direct_attempt>{};
   auto winner_plan_index = std::optional<std::size_t>{};
   auto terminal_error = std::exception_ptr{};
   auto best_error = std::exception_ptr{};
   auto best_error_index = (std::numeric_limits<std::size_t>::max)();
   auto rejection_error = std::exception_ptr{};
   auto rejection_error_index = (std::numeric_limits<std::size_t>::max)();
   auto pending_discards = std::vector<direct_attempt>{};
   // Every successful worker can leave one unpublished attempt, including a
   // selected winner that becomes unusable during terminal cleanup.
   pending_discards.reserve(plan.size());
   auto first_error = std::exception_ptr{};

   const auto launch = [&](std::size_t index) {
      const auto item = plan[index];
      const auto deadline = candidate_deadline(value.logical_deadline, value.attempt_timeout, clock::now());
      // Build every potentially-throwing worker and fallback object before
      // active_ changes. A worker cannot run until co_spawn below.
      auto worker = async_attempt_worker(callback_set, operation, expansion.expected_peer, deadline, item, index);
      auto launch_failure = std::make_shared<dial_scheduler::launch_failure>();
      launch_failure->value = completion{.plan_index = index, .target = item.value};
      const auto publish_launch_failure = [operation, launch_failure](std::exception_ptr error) noexcept {
         if (!error || launch_failure->published.exchange(true, std::memory_order_acq_rel)) {
            return;
         }
         launch_failure->value.error = std::move(error);
         operation->publish(std::move(launch_failure->value));
      };
      const auto completion_handler = [publish_launch_failure](std::exception_ptr error) noexcept {
         publish_launch_failure(std::move(error));
      };
      launched[index] = true;
      ++launched_count;
      operation->launch();
      try {
         boost::asio::co_spawn(executor, std::move(worker), completion_handler);
      } catch (...) {
         // This path owns only the prebuilt failure completion. In particular,
         // it cannot allocate or copy endpoint text while active_ is nonzero.
         publish_launch_failure(std::current_exception());
      }
   };

   const auto next_planned = [&](clock::time_point now, bool early) -> std::optional<std::size_t> {
      auto result = std::optional<std::size_t>{};
      for (auto index = std::size_t{}; index < plan.size(); ++index) {
         if (launched[index] || (!early && start_at(started, plan[index]) > now)) {
            continue;
         }
         if (!result || start_at(started, plan[index]) < start_at(started, plan[*result]) ||
             (start_at(started, plan[index]) == start_at(started, plan[*result]) && index < *result)) {
            result = index;
         }
      }
      return result;
   };

   const auto next_scheduled = [&]() -> std::optional<clock::time_point> {
      auto result = std::optional<clock::time_point>{};
      for (auto index = std::size_t{}; index < plan.size(); ++index) {
         if (launched[index]) {
            continue;
         }
         const auto scheduled = start_at(started, plan[index]);
         result = result ? std::min(*result, scheduled) : scheduled;
      }
      return result;
   };

   try {
      while (!winner && !terminal_error) {
         if (parent_canceled() || operation->stop_requested() || callback_set->is_owner_stopping() ||
             clock::now() >= value.logical_deadline) {
            operation->request_stop();
            break;
         }

         while (!winner && !terminal_error) {
            auto completion = operation->take_completion();
            if (!completion) {
               break;
            }
            const auto stopping = operation->stop_requested();
            const auto owner_stopping = callback_set->is_owner_stopping();
            if (parent_canceled()) {
               // Cancellation before winner selection owns this completion too.
               // Defer its close until cancellation is disabled for the drain.
               if (completion->attempt) {
                  pending_discards.emplace_back(std::move(*completion->attempt));
               }
               operation->request_stop();
               break;
            }
            if (owner_stopping) {
               if (completion->attempt) {
                  co_await async_discard_preserving(callback_set, pending_discards, std::move(*completion->attempt));
               }
               operation->request_stop();
               continue;
            }
            if (completion->attempt) {
               if (!winner && !terminal_error && !stopping) {
                  completion->attempt->target = completion->target;
                  winner.emplace(std::move(*completion->attempt));
                  winner_plan_index = completion->plan_index;
                  operation->request_stop();
               } else {
                  co_await async_discard_preserving(callback_set, pending_discards, std::move(*completion->attempt));
               }
               continue;
            }
            if (completion->attributable) {
               attributable_failures[completion->plan_index] = true;
            }
            if (completion->attributable && !stopping) {
               owner->black_holes_.record_address_outcome(completion->target, dialing::outcome::failure);
               launch_early = operation->active() == 0;
               if (!best_error || completion->plan_index < best_error_index) {
                  best_error = std::move(completion->error);
                  best_error_index = completion->plan_index;
               }
               continue;
            }
            if (!stopping && failure_code(completion->error) == exceptions::code::connection_rejected) {
               // Explicit connection rejection is candidate-local. Resource
               // admission and other local errors still terminate the operation.
               launch_early = operation->active() == 0;
               if (!rejection_error || completion->plan_index < rejection_error_index) {
                  rejection_error = std::move(completion->error);
                  rejection_error_index = completion->plan_index;
               }
               continue;
            }
            if (!stopping && !terminal_error) {
               terminal_error = std::move(completion->error);
               if (!first_error) {
                  first_error = terminal_error;
               }
               operation->request_stop();
            }
         }
         if (winner || terminal_error || operation->stop_requested()) {
            break;
         }

         const auto now = clock::now();
         const auto hold_until = operation->tcp_handshake_hold_until();
         const auto eligible = next_planned(now, launch_early);
         const auto launch_limit_reached = launched_count >= value.max_attempts || launched_count == plan.size();
         if (!launch_limit_reached && eligible && operation->active() < owner->policy_.max_concurrent_attempts &&
             now >= hold_until) {
            launch(*eligible);
            launch_early = false;
            continue;
         }

         if (operation->active() == 0 && launch_limit_reached) {
            if (operation->has_completion()) {
               continue;
            }
            break;
         }

         auto wake_at = value.logical_deadline;
         if (!launch_limit_reached && operation->active() < owner->policy_.max_concurrent_attempts) {
            if (const auto scheduled = next_scheduled()) {
               wake_at = std::min(wake_at, std::max(*scheduled, hold_until));
            }
         }
         const auto observed = wakeup->epoch();
         if (operation->has_completion() || (operation->active() == 0 && launch_limit_reached)) {
            continue;
         }
         auto wait_error = std::exception_ptr{};
         try {
            static_cast<void>(co_await wakeup->async_wait_until(observed, wake_at));
         } catch (...) {
            wait_error = std::current_exception();
         }
         if (is_operation_aborted_wait(wait_error) && parent_canceled()) {
            operation->request_stop();
         } else if (wait_error && !is_timeout_wait(wait_error) && !is_operation_aborted_wait(wait_error)) {
            terminal_error = std::move(wait_error);
            if (!first_error) {
               first_error = terminal_error;
            }
            operation->request_stop();
         }
      }
   } catch (...) {
      const auto error = std::current_exception();
      if (!first_error && !(is_operation_aborted_wait(error) && parent_canceled())) {
         first_error = error;
      }
      operation->request_stop();
   }

   // Snapshot before reset; a winner already selected retains terminal priority.
   const auto canceled = parent_canceled();
   const auto timed_out = !winner && !first_error && clock::now() >= value.logical_deadline;
   const auto closed = !winner && !first_error && !canceled && !timed_out && operation->stop_requested();
   operation->request_stop();

   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   while (operation->active() != 0 || operation->has_completion()) {
      while (auto completion = operation->take_completion()) {
         if (!completion->attempt) {
            if (completion->attributable) {
               attributable_failures[completion->plan_index] = true;
               try {
                  owner->black_holes_.record_address_outcome(completion->target, dialing::outcome::failure);
               } catch (...) {
                  if (!first_error) {
                     first_error = std::current_exception();
                  }
                  operation->request_stop();
               }
            }
            continue;
         }
         try {
            owner->black_holes_.record_address_outcome(completion->target, dialing::outcome::success);
         } catch (...) {
            if (!first_error) {
               first_error = std::current_exception();
            }
            operation->request_stop();
         }
         try {
            co_await async_discard_preserving(callback_set, pending_discards, std::move(*completion->attempt));
         } catch (...) {
            if (!first_error) {
               first_error = std::current_exception();
            }
            operation->request_stop();
         }
      }
      if (operation->active() != 0) {
         const auto observed = wakeup->epoch();
         if (operation->has_completion() || operation->active() == 0) {
            continue;
         }
         try {
            static_cast<void>(co_await wakeup->async_wait(observed));
         } catch (...) {
            if (!first_error) {
               first_error = std::current_exception();
            }
            operation->request_stop();
         }
      }
   }

   auto winner_owner_stopping = false;
   if (winner) {
      try {
         winner_owner_stopping = callback_set->is_owner_stopping();
      } catch (...) {
         if (!first_error) {
            first_error = std::current_exception();
         }
         winner_owner_stopping = true;
      }
   }
   if (winner && winner_owner_stopping) {
      try {
         co_await async_discard_preserving(callback_set, pending_discards, std::move(*winner));
      } catch (...) {
         if (!first_error) {
            first_error = std::current_exception();
         }
      }
      winner.reset();
   }
   if (winner) {
      try {
         owner->black_holes_.record_address_outcome(winner->target, dialing::outcome::success);
      } catch (...) {
         if (!first_error) {
            first_error = std::current_exception();
         }
      }
   }
   try {
      if (const auto discard_error = co_await async_drain_pending_discards(callback_set, pending_discards);
          discard_error && !first_error) {
         first_error = discard_error;
      }
   } catch (...) {
      if (!first_error) {
         first_error = std::current_exception();
      }
   }

   if (winner && first_error) {
      // The caller never receives a winner once cleanup has failed. Keep it
      // in the same owned drain so its terminal transport close is awaited.
      pending_discards.emplace_back(std::move(*winner));
      winner.reset();
      try {
         if (const auto discard_error = co_await async_drain_pending_discards(callback_set, pending_discards);
             discard_error && !first_error) {
            first_error = discard_error;
         }
      } catch (...) {
         if (!first_error) {
            first_error = std::current_exception();
         }
      }
   }

   const auto terminal_attributable_exhaustion =
       !winner && !first_error && !canceled && !timed_out && !closed && static_cast<bool>(best_error) &&
       has_attributable_exhaustion(launched, attributable_failures);
   if (terminal_attributable_exhaustion) {
      try {
         // A launch cap can leave plan entries untouched; root attribution must
         // still inspect those entries before crediting any root failure.
         callback_set->observe_terminal_root_outcomes(
             root_outcomes_for(expansion.roots, plan, launched, attributable_failures, std::nullopt));
      } catch (...) {
         // A terminal observer is attribution-only and must not replace the dial error.
      }
   }

   if (first_error) {
      std::rethrow_exception(first_error);
   }
   if (winner) {
      auto result = dial_result{};
      try {
         if (!winner_plan_index) {
            FORGE_THROW_EXCEPTION(exceptions::internal, "P2P direct dial winner has no plan attribution");
         }
         result.winner = winner->target;
         const auto& winner_indices = plan[*winner_plan_index].root_indices;
         result.winner_roots.reserve(winner_indices.size());
         for (const auto index : winner_indices) {
            result.winner_roots.push_back(expansion.roots[index]);
         }
         result.root_outcomes =
             root_outcomes_for(expansion.roots, plan, launched, attributable_failures, winner_plan_index);
      } catch (...) {
         first_error = std::current_exception();
      }
      if (first_error) {
         pending_discards.emplace_back(std::move(*winner));
         winner.reset();
         try {
            static_cast<void>(co_await async_drain_pending_discards(callback_set, pending_discards));
         } catch (...) {
            // Preserve the result-materialization failure after terminal drain.
         }
         std::rethrow_exception(first_error);
      }
      static_assert(std::is_nothrow_move_assignable_v<direct_attempt>);
      result.attempt = std::move(*winner);
      co_return result;
   }
   if (canceled) {
      throw_canceled();
   }
   if (timed_out) {
      throw_timeout();
   }
   if (winner_owner_stopping || closed) {
      throw_closed();
   }
   if (best_error) {
      std::rethrow_exception(best_error);
   }
   if (rejection_error) {
      std::rethrow_exception(rejection_error);
   }
   throw_no_endpoint();
}

} // namespace forge::net::p2p::detail
