module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

module forge.plugins.chain.signer.plugin;

import forge.chain.protocol.time;
import forge.chain.api.block_signer;
import forge.chain.protocol.block_signing;
import forge.plugins.chain.signer.exceptions;
import forge.plugins.chain.signer.types;

#include "details/plugin_impl.hxx"
#include "details/runtime_state.hxx"

namespace forge::plugins::chain::signer {
namespace {

[[nodiscard]] forge::chain::protocol::time_point_sec system_now() {
   const auto elapsed =
       std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch());
   if (elapsed.count() < 0 || static_cast<std::uint64_t>(elapsed.count()) > std::numeric_limits<std::uint32_t>::max()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_lifecycle, "system clock is outside the Chain time range");
   }
   return forge::chain::protocol::time_point_sec{static_cast<std::uint32_t>(elapsed.count())};
}

} // namespace

plugin::impl::impl(plugin_options value) : options_{std::move(value)} {}

plugin::impl::~impl() = default;

void plugin::impl::set_config(config value) {
   options_.initial_config = std::move(value);
}

void plugin::impl::initialize() {
   const auto& settings = options_.initial_config;
   if (settings.max_inflight == 0U) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer max-inflight must be positive");
   }
   if (settings.max_per_caller == 0U || settings.shutdown_timeout_ms == 0U) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer lifecycle limits must be positive");
   }
   if (!options_.now) {
      options_.now = system_now;
   }

   auto runtime = std::make_shared<runtime_state>(runtime_state{
       .policy = std::make_shared<signing_policy>(options_, settings),
       .admission = std::make_shared<admission_queue>(
           static_cast<std::size_t>(settings.max_inflight), static_cast<std::size_t>(settings.max_queued),
           static_cast<std::size_t>(settings.max_queued_bytes), static_cast<std::size_t>(settings.max_per_caller)),
       .shutdown_timeout = std::chrono::milliseconds{static_cast<std::int64_t>(settings.shutdown_timeout_ms)},
   });

   auto close_admission = false;
   {
      const auto lock = std::scoped_lock{lifecycle_mutex_};
      runtime_ = runtime;
      close_admission = stop_requested_.load(std::memory_order_acquire);
   }
   if (close_admission) {
      runtime->admission->close();
   }
}

boost::asio::awaitable<admission_lease> plugin::impl::acquire(std::string caller, std::size_t bytes) {
   const auto runtime = runtime_snapshot();
   if (runtime == nullptr) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_lifecycle, "Chain signer plugin is not initialized");
   }
   co_return co_await runtime->admission->acquire(std::move(caller), bytes);
}

void plugin::impl::request_stop() noexcept {
   stop_requested_.store(true, std::memory_order_release);
   const auto runtime = runtime_snapshot();
   if (runtime != nullptr) {
      runtime->admission->close();
   }
}

boost::asio::awaitable<void> plugin::impl::wait_for_drain() {
   const auto runtime = runtime_snapshot();
   if (runtime != nullptr) {
      auto executor = co_await boost::asio::this_coro::executor;
      auto timer = boost::asio::steady_timer{executor};
      timer.expires_after(runtime->shutdown_timeout);
      using namespace boost::asio::experimental::awaitable_operators;
      auto result = co_await (runtime->admission->wait_for_drain() || timer.async_wait(boost::asio::use_awaitable));
      if (result.index() != 0U) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_lifecycle, "Chain signer shutdown timed out");
      }
   }
}

bool plugin::impl::stopping() const noexcept {
   return stop_requested_.load(std::memory_order_acquire);
}

signing_policy::transaction_selection
plugin::impl::select_transaction(const forge::chain::transaction::unsigned_transaction& transaction,
                                 const forge::api::auth::authenticated_caller& caller) const {
   const auto runtime = runtime_snapshot();
   if (runtime == nullptr) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_lifecycle, "Chain signer plugin is not initialized");
   }
   return runtime->policy->select_transaction(transaction, caller, options_.now());
}

signing_policy::finality_selection plugin::impl::select_finality() const {
   const auto runtime = runtime_snapshot();
   if (runtime == nullptr) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_lifecycle, "Chain signer plugin is not initialized");
   }
   return runtime->policy->select_finality();
}

signing_policy::block_selection
plugin::impl::select_block(const forge::chain::protocol::block_sign_request& request,
                           const forge::api::auth::authenticated_caller& caller) const {
   const auto runtime = runtime_snapshot();
   if (runtime == nullptr) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_lifecycle, "Chain signer plugin is not initialized");
   }
   return runtime->policy->select_block(request, caller);
}

void plugin::impl::audit(audit_entry value) const noexcept {
   if (options_.audit != nullptr) {
      options_.audit->record(value);
   }
}

std::shared_ptr<const runtime_state> plugin::impl::runtime_snapshot() const {
   const auto lock = std::scoped_lock{lifecycle_mutex_};
   return runtime_;
}

} // namespace forge::plugins::chain::signer
