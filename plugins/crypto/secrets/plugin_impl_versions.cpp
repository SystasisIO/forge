module;

#include <boost/asio/awaitable.hpp>
#include <unistd.h>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <forge/exceptions/macros.hpp>
#include <atomic>

module forge.plugins.crypto.secrets.plugin;

import forge.app.plugin_lifecycle;
import forge.asio.compute;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.keystore.ownership;
import forge.crypto.keystore.types;
import forge.config.core.component;
import forge.config.core.decode;
import forge.plugins.crypto.secrets.exceptions;
import forge.plugins.crypto.secrets.version_types;

#include "details/plugin_impl.hxx"
#include "details/version_store.hxx"

namespace forge::plugins::crypto::secrets {

std::shared_ptr<version_store> plugin::impl::configured_versions(forge::config::core::component_view view,
                                                                 const config& legacy) const {
   auto decoded =
       forge::config::core::decode<version_store_config>(view.source(), std::string{view.section()} + ".versions");
   if (!decoded.ok()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Managed secret configuration is invalid");
   }
   return std::make_shared<version_store>(std::move(decoded.value),
                                          forge::crypto::keystore::decrypt_limits{
                                              .max_scrypt_n = legacy.encrypted_file_max_scrypt_n,
                                              .max_scrypt_r = legacy.encrypted_file_max_scrypt_r,
                                              .max_scrypt_p = legacy.encrypted_file_max_scrypt_p,
                                              .max_scrypt_memory_bytes = legacy.encrypted_file_max_scrypt_memory_bytes,
                                          },
                                          lifecycle);
}

std::shared_ptr<version_store> plugin::impl::versions_store() const {
   if (process != static_cast<std::uint64_t>(::getpid())) {
      FORGE_THROW_EXCEPTION(exceptions::ownership_lost, "Managed secret ownership is no longer valid");
   }
   auto current = std::atomic_load_explicit(&versions, std::memory_order_acquire);
   if (!current) {
      FORGE_THROW_EXCEPTION(exceptions::store_unavailable, "Managed secret store is unavailable");
   }
   return current;
}

boost::asio::awaitable<void> plugin::impl::initialize_versions(forge::app::plugin_context& context) {
   const auto current = versions_store();
   co_await current->initialize(context.has_compute() ? context.compute() : forge::asio::compute::executor{});
}

void plugin::impl::stop_versions() noexcept {
   if (process != static_cast<std::uint64_t>(::getpid())) {
      return;
   }
   if (auto current = std::atomic_load_explicit(&versions, std::memory_order_acquire)) {
      current->request_stop();
   }
}

boost::asio::awaitable<void> plugin::impl::shutdown_versions() {
   if (process != static_cast<std::uint64_t>(::getpid())) {
      FORGE_THROW_EXCEPTION(exceptions::ownership_lost, "Managed secret ownership is no longer valid");
   }
   // App plugin_lifecycle drains the active initialize hook before granting
   // the shutdown hook (begin_owned/drain_cleanup); only API work remains here.
   if (auto current = std::atomic_load_explicit(&versions, std::memory_order_acquire)) {
      co_await current->shutdown();
   }
}

} // namespace forge::plugins::crypto::secrets
