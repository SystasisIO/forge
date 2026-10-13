module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

module forge.net.p2p.dht.record_store;

import forge.asio.gate;
import forge.net.p2p.exceptions;

#include "details/dht_record_store_impl.hxx"

namespace forge::net::p2p {
bool dht::record_store::impl::expired(std::chrono::system_clock::time_point value,
                                      std::chrono::system_clock::time_point now) noexcept {
   return value != std::chrono::system_clock::time_point{} && value <= now;
}

bool dht::record_store::impl::exceeds(std::size_t current, std::size_t removed, std::size_t added,
                                      std::size_t maximum) noexcept {
   return removed > current || added > maximum || current - removed > maximum - added;
}

std::string dht::record_store::impl::current_failure_message() {
   try {
      throw;
   } catch (const std::exception& error) {
      return error.what();
   } catch (...) {
      return "unknown persistence failure";
   }
}

std::string dht::record_store::impl::durability_failure_message(const dht::record_store::apply_result& result) {
   return result.durability_failure.empty() ? "persistence commit completed without durable acknowledgement"
                                            : result.durability_failure;
}

void dht::record_store::impl::throw_durability_uncertain(const dht::record_store::apply_result& result) {
   FORGE_THROW_EXCEPTION(exceptions::durability_uncertain, "DHT record state durability could not be confirmed",
                         forge::exceptions::ctx("reason", durability_failure_message(result)));
}

dht::record_store::impl::impl(dht::profile profile_value, dht::record_store::options options_value)
    : profile_(std::move(profile_value)), options_(std::move(options_value)), persistence_(options_.persistence) {
   validate(profile_);
   validate(profile_, options_);
   if (!persistence_) {
      persistence_ = dht::record_store::make_memory_persistence();
      options_.persistence = persistence_;
   }
}

boost::asio::awaitable<dht::record_store::put_result>
dht::record_store::impl::async_put_owned(std::shared_ptr<impl> self, dht::record_store::value_record incoming,
                                         std::chrono::system_clock::time_point now) {
   co_return co_await self->async_put(std::move(incoming), now);
}

boost::asio::awaitable<std::optional<dht::record_store::put_result>>
dht::record_store::impl::async_put_received_owned(std::shared_ptr<impl> self, dht::record_store::value_record incoming,
                                                  std::chrono::system_clock::time_point now) {
   co_return co_await self->async_put_received(std::move(incoming), now);
}

boost::asio::awaitable<void> dht::record_store::impl::async_upsert_provider_owned(
    std::shared_ptr<impl> self, dht::record_store::provider_record value, std::chrono::system_clock::time_point now) {
   co_await self->async_upsert_provider(std::move(value), now);
}

boost::asio::awaitable<void> dht::record_store::impl::async_remove_provider_owned(std::shared_ptr<impl> self,
                                                                                  dht::record_store::provider_key key) {
   co_await self->async_remove_provider(std::move(key));
}

boost::asio::awaitable<void> dht::record_store::impl::async_hydrate_owned(std::shared_ptr<impl> self,
                                                                          std::chrono::system_clock::time_point now) {
   co_await self->async_hydrate(now);
}

boost::asio::awaitable<dht::record_store::prune_result>
dht::record_store::impl::async_prune_expired_owned(std::shared_ptr<impl> self,
                                                   std::chrono::system_clock::time_point now) {
   co_return co_await self->async_prune_expired(now);
}

boost::asio::awaitable<void> dht::record_store::impl::async_flush_owned(std::shared_ptr<impl> self) {
   co_await self->async_flush();
}

boost::asio::awaitable<void> dht::record_store::impl::async_close_owned(std::shared_ptr<impl> self) {
   co_await self->async_close();
}

dht::record_store::impl::operation_admission::operation_admission(dht::record_store::impl* owner) noexcept
    : owner_(owner) {}

dht::record_store::impl::operation_admission::~operation_admission() {
   if (owner_) {
      owner_->release_operation();
   }
}

dht::record_store::impl::operation_admission::operation_admission(operation_admission&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)) {}

dht::record_store::impl::operation_admission&
dht::record_store::impl::operation_admission::operator=(operation_admission&& other) noexcept {
   if (this != &other) {
      if (owner_) {
         owner_->release_operation();
      }
      owner_ = std::exchange(other.owner_, nullptr);
   }
   return *this;
}

dht::record_store::impl::close_admission::close_admission(dht::record_store::impl* owner) noexcept : owner_(owner) {}

dht::record_store::impl::close_admission::~close_admission() {
   if (owner_) {
      owner_->release_close();
   }
}

dht::record_store::impl::close_admission::close_admission(close_admission&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)) {}

dht::record_store::impl::close_admission&
dht::record_store::impl::close_admission::operator=(close_admission&& other) noexcept {
   if (this != &other) {
      if (owner_) {
         owner_->release_close();
      }
      owner_ = std::exchange(other.owner_, nullptr);
   }
   return *this;
}

void dht::record_store::impl::ensure_open_locked() const {
   if (closing_ || closed_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "DHT record store is closing or closed");
   }
}

dht::record_store::impl::operation_admission dht::record_store::impl::admit_operation() {
   auto lock = std::scoped_lock{mutex_};
   ensure_open_locked();
   if (operation_admissions_ >= options_.max_persistence_waiters) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "DHT record store persistence waiter limit reached");
   }
   ++operation_admissions_;
   return operation_admission{this};
}

std::optional<dht::record_store::impl::close_admission> dht::record_store::impl::admit_close() {
   auto lock = std::scoped_lock{mutex_};
   if (closed_) {
      return std::nullopt;
   }
   if (close_waiters_ >= options_.max_persistence_waiters) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "DHT record store close waiter limit reached");
   }
   closing_ = true;
   ++close_waiters_;
   return close_admission{this};
}

void dht::record_store::impl::release_operation() noexcept {
   auto drainers = std::map<const void*, std::function<void()>>{};
   {
      auto lock = std::scoped_lock{mutex_};
      if (operation_admissions_ > 0) {
         --operation_admissions_;
      }
      if (operation_admissions_ == 0) {
         drainers.swap(operation_drainers_);
      }
   }
   for (const auto& [_, drain] : drainers) {
      drain();
   }
}

void dht::record_store::impl::release_close() noexcept {
   auto lock = std::scoped_lock{mutex_};
   if (close_waiters_ > 0) {
      --close_waiters_;
   }
}

boost::asio::awaitable<void> dht::record_store::impl::wait_for_operations() {
   while (true) {
      auto timer = std::make_shared<boost::asio::steady_timer>(co_await boost::asio::this_coro::executor);
      timer->expires_at(std::chrono::steady_clock::time_point::max());
      const auto* drainer_id = timer.get();
      {
         auto lock = std::scoped_lock{mutex_};
         if (operation_admissions_ == 0) {
            co_return;
         }
         operation_drainers_.emplace(drainer_id, [weak = std::weak_ptr{timer}] {
            if (auto current = weak.lock()) {
               boost::asio::post(current->get_executor(), [current] { current->cancel(); });
            }
         });
      }

      auto error = boost::system::error_code{};
      co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
      auto drained = false;
      {
         auto lock = std::scoped_lock{mutex_};
         operation_drainers_.erase(drainer_id);
         drained = operation_admissions_ == 0;
      }
      if (drained) {
         co_return;
      }
      if (error == boost::asio::error::operation_aborted) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "DHT record store close was canceled while draining");
      }
      if (error) {
         FORGE_THROW_EXCEPTION(exceptions::internal, "DHT record store close admission wait failed",
                               forge::exceptions::ctx("error", error.message()));
      }
   }
}

} // namespace forge::net::p2p
