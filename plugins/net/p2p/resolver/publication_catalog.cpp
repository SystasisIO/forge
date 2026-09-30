module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <algorithm>
#include <cstddef>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.resolver.plugin;

import forge.api.core.binding;
import forge.api.p2p.publication;
import forge.api.transport.options;
import forge.asio.notification;
import forge.asio.task;
import forge.exceptions;
import forge.net.p2p.protocol;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.resolver.exceptions;
import forge.plugins.net.p2p.resolver.types;

#include "details/publication_catalog.hxx"

namespace forge::plugins::net::p2p::resolver::detail {
namespace {

[[nodiscard]] std::string api_key(const entry& value) {
   return value.id.value + "#" + std::to_string(value.version.major);
}

} // namespace

publication_catalog::generation::generation(std::string protocol, std::vector<entry> entries)
    : protocol_{std::move(protocol)}, entries_{std::move(entries)} {}

void publication_catalog::generation::set_child(std::shared_ptr<forge::api::p2p::publication> child) noexcept {
   auto current = std::shared_ptr<forge::api::p2p::publication>{};
   auto close = false;
   {
      const auto lock = std::scoped_lock{mutex_};
      if (!child_) {
         child_ = std::move(child);
      }
      current = child_;
      close = !active_;
   }
   if (close && current) {
      current->close();
   }
}

const std::string& publication_catalog::generation::protocol() const noexcept {
   return protocol_;
}

const std::vector<entry>& publication_catalog::generation::entries() const noexcept {
   return entries_;
}

bool publication_catalog::generation::active() const noexcept {
   auto child = std::shared_ptr<forge::api::p2p::publication>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      if (!active_) {
         return false;
      }
      child = child_;
   }
   return child && child->active();
}

void publication_catalog::generation::close() noexcept {
   auto child = std::shared_ptr<forge::api::p2p::publication>{};
   if (seal(child) && child) {
      child->close();
   }
}

bool publication_catalog::generation::seal(std::shared_ptr<forge::api::p2p::publication>& child) noexcept {
   const auto lock = std::scoped_lock{mutex_};
   if (!active_) {
      return false;
   }
   active_ = false;
   child = child_;
   return true;
}

boost::asio::awaitable<void> publication_catalog::generation::async_close() {
   close();
   auto child = std::shared_ptr<forge::api::p2p::publication>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      child = child_;
   }
   if (child) {
      co_await child->async_close();
   }
   co_return;
}

publication_catalog::publication_catalog(forge::asio::task::scheduler& scheduler)
    : scheduler_{&scheduler}, owner_executor_{scheduler.runtime_context().context().get_executor()} {}

publication_catalog::~publication_catalog() {
   request_close();
}

forge::api::p2p::publication
publication_catalog::publish(forge::plugins::net::p2p::node::api& p2p, forge::api::core::binding_plan plan,
                             forge::net::p2p::protocol_id protocol, forge::api::transport::options options,
                             std::vector<entry> entries, std::size_t max_apis) {
   auto next = std::make_shared<generation>(protocol.value, std::move(entries));
   const auto owner = std::weak_ptr<publication_catalog>{shared_from_this()};
   auto outer = forge::api::p2p::detail::publication_access::make(
      owner_executor_,
      [owner, next] {
         if (const auto catalog = owner.lock()) {
            catalog->close_generation(next);
            return;
         }
         next->close();
      },
      [next]() -> boost::asio::awaitable<void> {
         co_await next->async_close();
      },
      [next] { return next->active(); });

   const auto protocol_value = protocol.value;
   auto new_protocol = false;
   {
      const auto lock = std::scoped_lock{mutex_};
      if (closed_) {
         FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "resolver API publications are closed");
      }
      if (pending_.contains(protocol_value)) {
         FORGE_THROW_EXCEPTION(exceptions::duplicate_api, "resolver API protocol publication is pending",
                               forge::exceptions::ctx("protocol", protocol_value));
      }

      validate_publish_locked(protocol_value, next->entries(), max_apis);
      validate_retirement_backlog_locked(max_apis);
      const auto inserted = pending_.try_emplace(protocol_value, next).second;
      if (!inserted) {
         FORGE_THROW_EXCEPTION(exceptions::duplicate_api, "resolver API protocol publication is pending",
                               forge::exceptions::ctx("protocol", protocol_value));
      }
      new_protocol = !active_.contains(protocol_value);
      if (new_protocol) {
         try {
            order_.push_back(protocol_value);
         } catch (...) {
            pending_.erase(protocol_value);
            throw;
         }
      }
      ++inflight_publishes_;
   }

   auto child = std::shared_ptr<forge::api::p2p::publication>{};
   try {
      child = std::make_shared<forge::api::p2p::publication>(
         p2p.publish_api(std::move(plan), std::move(protocol), std::move(options)));
   } catch (...) {
      if (cancel_reservation(protocol_value, next, new_protocol)) {
         FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "resolver API publications are closed");
      }
      throw;
   }

   next->set_child(std::move(child));

   auto previous = std::shared_ptr<generation>{};
   auto rejected_by_shutdown = false;
   auto notify = false;
   {
      const auto lock = std::scoped_lock{mutex_};
      const auto pending = pending_.find(protocol_value);
      if (pending == pending_.end() || pending->second != next) {
         rejected_by_shutdown = true;
      } else {
         auto pending_node = pending_.extract(pending);
         if (closed_) {
            closing_.insert(std::move(pending_node));
            rejected_by_shutdown = true;
         } else {
            const auto active = active_.find(protocol_value);
            if (active == active_.end()) {
               active_.insert(std::move(pending_node));
            } else {
               auto active_node = active_.extract(active);
               previous = std::move(active_node.mapped());
               active_node.mapped() = next;
               active_.insert(std::move(active_node));
               pending_node.mapped() = previous;
               closing_.insert(std::move(pending_node));
            }
         }
      }
      notify = finish_publish_locked();
   }
   if (notify) {
      publishes_ready_.notify();
   }

   if (previous) {
      schedule_retirement(previous);
   }
   if (rejected_by_shutdown) {
      schedule_retirement(next);
      FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "resolver API publications are closed");
   }
   return outer;
}

std::vector<entry> publication_catalog::snapshot() const {
   auto generations = std::vector<std::shared_ptr<generation>>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      generations.reserve(order_.size());
      for (const auto& protocol : order_) {
         if (pending_.contains(protocol)) {
            continue;
         }
         const auto found = active_.find(protocol);
         if (found != active_.end()) {
            generations.push_back(found->second);
         }
      }
   }

   auto result = std::vector<entry>{};
   for (const auto& generation : generations) {
      if (!generation->active()) {
         continue;
      }
      const auto& entries = generation->entries();
      result.insert(result.end(), entries.begin(), entries.end());
   }
   return result;
}

void publication_catalog::close_generation(const std::shared_ptr<generation>& generation) noexcept {
   auto retired = false;
   {
      const auto lock = std::scoped_lock{mutex_};
      const auto found = active_.find(generation->protocol());
      if (found != active_.end() && found->second == generation) {
         auto node = active_.extract(found);
         closing_.insert(std::move(node));
         retired = true;
         const auto ordered = std::ranges::find(order_, generation->protocol());
         if (ordered != order_.end()) {
            order_.erase(ordered);
         }
      }
   }
   if (retired) {
      schedule_retirement(generation);
      return;
   }
   generation->close();
}

void publication_catalog::schedule_retirement(const std::shared_ptr<generation>& generation) noexcept {
   generation->close();

   const auto owner = weak_from_this();
   if (owner.expired()) {
      return;
   }

   auto notify = false;
   try {
      const auto lock = std::scoped_lock{mutex_};
      const auto existing = std::ranges::find_if(
         retirements_, [&](const retirement_record& retirement) { return retirement.identity == generation.get(); });
      if (existing != retirements_.end()) {
         return;
      }

      const auto inserted = retirements_.emplace(retirements_.end(), retirement_record{
          .identity = generation.get(),
          .value = generation,
      });
      ++inflight_retirements_;
      auto submission = scheduler_->submit(forge::asio::task::awaitable{
          .priority = forge::asio::task::priority{100},
          .name = "p2p-resolver-publication-retirement",
          .work = [owner, generation](forge::asio::task::context&) -> boost::asio::awaitable<void> {
             if (const auto catalog = owner.lock()) {
                auto failure = std::exception_ptr{};
                try {
                   co_await generation->async_close();
                } catch (...) {
                   failure = std::current_exception();
                }
                catalog->finish_retirement(generation.get(), std::move(failure));
                co_return;
             }

             try {
                co_await generation->async_close();
             } catch (...) {
                // The catalog owner is gone, so no shutdown observer remains.
             }
             co_return;
          },
      });
      if (!submission.accepted()) {
         retirements_.erase(inserted);
         if (inflight_retirements_ > 0) {
            --inflight_retirements_;
            notify = inflight_retirements_ == 0;
         }
      } else {
         inserted->handle = std::move(submission);
      }
   } catch (...) {
      reject_retirement(generation.get(), std::current_exception());
   }
   if (notify) {
      retirements_ready_.notify();
   }
}

void publication_catalog::finish_retirement(const generation* generation, std::exception_ptr failure) noexcept {
   auto notify = false;
   {
      const auto lock = std::scoped_lock{mutex_};
      const auto found = std::ranges::find_if(
         retirements_, [&](const retirement_record& retirement) { return retirement.identity == generation; });
      if (found == retirements_.end()) {
         return;
      }

      remember_retirement_failure_locked(std::move(failure));
      if (inflight_retirements_ > 0) {
         --inflight_retirements_;
         notify = inflight_retirements_ == 0;
      }
      retirements_.erase(found);
      for (auto closing = closing_.begin(); closing != closing_.end();) {
         if (closing->second.get() == generation) {
            closing = closing_.erase(closing);
         } else {
            ++closing;
         }
      }
   }
   if (notify) {
      retirements_ready_.notify();
   }
}

void publication_catalog::abandon_retirement(const generation* generation) noexcept {
   auto notify = false;
   {
      const auto lock = std::scoped_lock{mutex_};
      const auto found = std::ranges::find_if(
         retirements_, [&](const retirement_record& retirement) { return retirement.identity == generation; });
      if (found == retirements_.end()) {
         return;
      }

      retirements_.erase(found);
      if (inflight_retirements_ > 0) {
         --inflight_retirements_;
         notify = inflight_retirements_ == 0;
      }
   }
   if (notify) {
      retirements_ready_.notify();
   }
}

void publication_catalog::reject_retirement(const generation* generation, std::exception_ptr failure) noexcept {
   auto notify = false;
   {
      const auto lock = std::scoped_lock{mutex_};
      const auto found = std::ranges::find_if(
         retirements_, [&](const retirement_record& retirement) { return retirement.identity == generation; });
      if (found != retirements_.end()) {
         retirements_.erase(found);
         if (inflight_retirements_ > 0) {
            --inflight_retirements_;
            notify = inflight_retirements_ == 0;
         }
      }
      remember_retirement_failure_locked(std::move(failure));
   }
   if (notify) {
      retirements_ready_.notify();
   }
}

void publication_catalog::remember_retirement_failure_locked(std::exception_ptr failure) noexcept {
   if (!retirement_failure_ && failure) {
      retirement_failure_ = std::move(failure);
   }
}

void publication_catalog::request_close() noexcept {
   auto notify = false;
   auto closing = std::vector<std::shared_ptr<generation>>{};
   auto pending = std::vector<std::shared_ptr<generation>>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      if (closed_) {
         return;
      }
      closed_ = true;
      closing_.merge(active_);
      order_.clear();
      closing.reserve(closing_.size());
      for (const auto& [protocol, generation] : closing_) {
         static_cast<void>(protocol);
         closing.push_back(generation);
      }
      pending.reserve(pending_.size());
      for (const auto& [protocol, generation] : pending_) {
         static_cast<void>(protocol);
         pending.push_back(generation);
      }
      notify = inflight_publishes_ == 0;
   }

   for (const auto& generation : pending) {
      generation->close();
   }
   for (const auto& generation : closing) {
      schedule_retirement(generation);
   }
   if (notify) {
      publishes_ready_.notify();
   }
}

boost::asio::awaitable<void> publication_catalog::async_close() {
   request_close();
   co_await wait_for_publishes();

   co_await wait_for_retirements();

   auto fallback = std::vector<std::shared_ptr<generation>>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      auto identities = std::set<const generation*>{};
      fallback.reserve(closing_.size());
      for (const auto& [protocol, generation] : closing_) {
         static_cast<void>(protocol);
         if (identities.insert(generation.get()).second) {
            fallback.push_back(generation);
         }
      }
      closing_.clear();
      retirements_.clear();
   }
   for (const auto& generation : fallback) {
      try {
         co_await generation->async_close();
      } catch (...) {
         const auto lock = std::scoped_lock{mutex_};
         remember_retirement_failure_locked(std::current_exception());
      }
   }

   auto failure = std::exception_ptr{};
   {
      const auto lock = std::scoped_lock{mutex_};
      failure = retirement_failure_;
   }
   if (failure) {
      std::rethrow_exception(failure);
   }
}

void publication_catalog::validate_publish_locked(const std::string& protocol, const std::vector<entry>& entries,
                                                  std::size_t max_apis) const {
   auto keys = std::set<std::string>{};
   auto total = std::size_t{0};
   const auto include = [&](const std::string& candidate_protocol, const std::shared_ptr<generation>& generation) {
      if (candidate_protocol == protocol) {
         return;
      }
      for (const auto& value : generation->entries()) {
         keys.insert(api_key(value));
         ++total;
      }
   };
   for (const auto& [candidate_protocol, generation] : active_) {
      include(candidate_protocol, generation);
   }
   for (const auto& [candidate_protocol, generation] : pending_) {
      if (candidate_protocol == protocol) {
         continue;
      }
      include(candidate_protocol, generation);
   }
   if (entries.size() > max_apis || total > max_apis - entries.size()) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "resolver local API limit exceeded");
   }
   for (const auto& value : entries) {
      if (!keys.insert(api_key(value)).second) {
         FORGE_THROW_EXCEPTION(exceptions::duplicate_api, "duplicate resolver API publication",
                               forge::exceptions::ctx("api", value.id.value));
      }
   }
}

void publication_catalog::validate_retirement_backlog_locked(std::size_t max_apis) const {
   if (closing_.size() >= max_apis) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "resolver publication retirement backlog limit exceeded",
                            forge::exceptions::ctx("limit", std::to_string(max_apis)));
   }
}

bool publication_catalog::cancel_reservation(const std::string& protocol, const std::shared_ptr<generation>& generation,
                                              bool remove_order) noexcept {
   auto closed = false;
   auto notify = false;
   {
      const auto lock = std::scoped_lock{mutex_};
      const auto found = pending_.find(protocol);
      if (found != pending_.end() && found->second == generation) {
         pending_.erase(found);
         if (remove_order) {
            const auto ordered = std::ranges::find(order_, protocol);
            if (ordered != order_.end()) {
               order_.erase(ordered);
            }
         }
         notify = finish_publish_locked();
      }
      closed = closed_;
   }
   generation->close();
   if (notify) {
      publishes_ready_.notify();
   }
   return closed;
}

bool publication_catalog::finish_publish_locked() noexcept {
   if (inflight_publishes_ > 0) {
      --inflight_publishes_;
   }
   return closed_ && inflight_publishes_ == 0;
}

boost::asio::awaitable<void> publication_catalog::wait_for_publishes() {
   for (;;) {
      const auto observed = publishes_ready_.epoch();
      {
         const auto lock = std::scoped_lock{mutex_};
         if (inflight_publishes_ == 0) {
            co_return;
         }
      }
      co_await publishes_ready_.async_wait(observed);
   }
}

boost::asio::awaitable<void> publication_catalog::wait_for_retirements() {
   for (;;) {
      const auto observed = retirements_ready_.epoch();
      auto identity = static_cast<const generation*>(nullptr);
      auto handle = forge::asio::task::handle{};
      {
         const auto lock = std::scoped_lock{mutex_};
         const auto retirement = std::ranges::find_if(retirements_, [](const retirement_record& value) {
            return value.handle.accepted();
         });
         if (retirement != retirements_.end()) {
            identity = retirement->identity;
            handle = std::move(retirement->handle);
         }
         if (inflight_retirements_ == 0) {
            co_return;
         }
      }
      if (handle.valid()) {
         try {
            co_await handle.wait();
         } catch (...) {
            abandon_retirement(identity);
         }
         continue;
      }
      co_await retirements_ready_.async_wait(observed);
   }
}

std::shared_ptr<publication_catalog> make_publication_catalog(forge::asio::task::scheduler& scheduler) {
   return std::make_shared<publication_catalog>(scheduler);
}

forge::api::p2p::publication
publish_catalog_api(const std::shared_ptr<publication_catalog>& catalog,
                    forge::plugins::net::p2p::node::api& p2p, forge::api::core::binding_plan plan,
                    forge::net::p2p::protocol_id protocol, forge::api::transport::options options,
                    std::vector<entry> entries, std::size_t max_apis) {
   if (!catalog) {
      FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "resolver API publication catalog is unavailable");
   }
   return catalog->publish(p2p, std::move(plan), std::move(protocol), std::move(options), std::move(entries), max_apis);
}

std::vector<entry> publication_catalog_snapshot(const std::shared_ptr<publication_catalog>& catalog) {
   if (!catalog) {
      return {};
   }
   return catalog->snapshot();
}

void request_close_publication_catalog(const std::shared_ptr<publication_catalog>& catalog) noexcept {
   if (catalog) {
      catalog->request_close();
   }
}

boost::asio::awaitable<void>
async_close_publication_catalog(const std::shared_ptr<publication_catalog>& catalog) {
   if (catalog) {
      co_await catalog->async_close();
   }
}

} // namespace forge::plugins::net::p2p::resolver::detail
