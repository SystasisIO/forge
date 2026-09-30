module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>

module forge.plugins.chain.signer.plugin;

import forge.chain.protocol.time;
import forge.api.core.registry;
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

boost::asio::awaitable<void> plugin::impl::initialize(forge::api::core::view apis) {
   if (runtime_snapshot()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_lifecycle, "Chain signer is already initialized");
   }
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
   resolve_providers(apis);
   co_await resolve_finality_provider(apis);

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
   co_return;
}

boost::asio::awaitable<void> plugin::impl::resolve_finality_provider(const forge::api::core::view& apis) {
   const auto& binding = options_.initial_config.finality;
   if (!binding || !options_.resolve_finality_provider ||
       std::ranges::any_of(options_.finality_providers,
                           [&](const auto& value) { return value.name == binding->provider; })) {
      co_return;
   }
   if (binding->provider.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Finality provider name is empty");
   }
   std::shared_ptr<forge::crypto::bls::signer::provider> provider;
   try {
      provider = co_await options_.resolve_finality_provider(binding->provider, apis);
   } catch (...) {
      // A key source may expose secret contents in its native diagnostic text.
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Finality provider source failed");
   }
   if (!provider) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Finality provider source returned no provider");
   }
   options_.finality_providers.push_back({binding->provider, std::move(provider)});
}

void plugin::impl::resolve_providers(const forge::api::core::view& apis) {
   if (!options_.resolve_provider) {
      return;
   }
   std::set<std::string> present;
   for (const auto& provider : options_.providers) {
      // The existing policy compiler remains the single validator of explicit
      // providers, including duplicate names and null implementations.
      present.insert(provider.name);
   }
   std::set<std::string> referenced;
   for (const auto& profile : options_.initial_config.transaction_profiles) {
      referenced.insert(profile.signing.provider);
   }
   for (const auto& profile : options_.initial_config.block_profiles) {
      for (const auto& key : profile.signing) {
         referenced.insert(key.provider);
      }
   }
   auto resolved = std::vector<named_provider>{};
   for (const auto& name : referenced) {
      if (name.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Signer provider name is empty");
      }
      if (present.contains(name)) {
         continue;
      }
      std::shared_ptr<forge::crypto::signer::provider> provider;
      try {
         provider = options_.resolve_provider(name, apis);
      } catch (...) {
         // Provider sources may read private configuration; never forward their
         // diagnostic text into daemon/configure errors.
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Signer provider source failed");
      }
      if (!provider) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Signer provider source returned no provider");
      }
      resolved.push_back({name, std::move(provider)});
   }
   for (auto& provider : resolved) {
      options_.providers.push_back(std::move(provider));
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

signing_policy::block_selection plugin::impl::select_block(const forge::chain::protocol::block_sign_request& request,
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

std::shared_ptr<block_execution_handler> plugin::impl::block_execution() const noexcept {
   return options_.block_execution;
}

std::shared_ptr<const runtime_state> plugin::impl::runtime_snapshot() const {
   const auto lock = std::scoped_lock{lifecycle_mutex_};
   return runtime_;
}

} // namespace forge::plugins::chain::signer
