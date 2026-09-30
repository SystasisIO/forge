module;

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.crypto.wallet.plugin;

import forge.asio.task;
import forge.asio.compute;
import forge.crypto.wallet.api;
import forge.plugins.net.http.server.api;
import forge.raw.raw;

#include "details/plugin_impl.hxx"

namespace forge::plugins::crypto::wallet {
namespace management = forge::crypto::wallet;

plugin::impl::impl(plugin_options options)
    : settings{std::move(options.initial_config)}, allow_http_management{options.allow_http_management} {
   for (auto& wallet : options.wallets) {
      if (!wallet || !wallets.emplace(wallet->name(), wallet).second) {
         throw management::exceptions::invalid_request{"Duplicate or missing wallet provider"};
      }
   }
}

void plugin::impl::validate() const {
   if (settings.publish_http && !allow_http_management) {
      throw management::exceptions::permission_denied{"Wallet management publication is disabled by the host"};
   }
   if (settings.directory.empty() || settings.max_wallets == 0 || settings.max_wallets > 256 ||
       settings.max_pending == 0 || settings.max_pending > 4096 || settings.max_request_bytes < 1024 ||
       settings.max_request_bytes > 1048576 || settings.permissions.size() > 1024) {
      throw management::exceptions::invalid_request{"Invalid wallet resource limits"};
   }
   std::set<std::string> names;
   for (const auto& [name, wallet] : wallets) {
      names.insert(name);
   }
   std::set<std::string> configured;
   for (const auto& wallet : settings.wallets) {
      static_cast<void>(provider{wallet.name});
      if (!configured.insert(wallet.name).second || wallet.timeout_seconds > 86400 ||
          (!wallet.unlock_file.empty() && (!wallet.open || wallet.timeout_seconds != 0))) {
         throw management::exceptions::invalid_request{"Invalid startup wallet configuration"};
      }
      names.insert(wallet.name);
   }
   if (names.size() > settings.max_wallets) {
      throw management::exceptions::resource_exhausted{"Too many configured wallets"};
   }
   for (const auto& entry : settings.permissions) {
      const auto hex = [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); };
      if (entry.fingerprint.size() != 64 || !std::ranges::all_of(entry.fingerprint, hex) || entry.wallets.empty() ||
          entry.wallets.size() > 256 || entry.operations.empty() || entry.operations.size() > 12) {
         throw management::exceptions::invalid_request{"Invalid wallet permission"};
      }
      for (const auto& name : entry.wallets) {
         static_cast<void>(provider{name});
      }
      for (const auto operation : entry.operations) {
         if (operation < management::operation::create || operation > management::operation::remove_key) {
            throw management::exceptions::invalid_request{"Invalid wallet operation permission"};
         }
      }
   }
}

bool plugin::impl::allowed(const forge::api::auth::authenticated_caller& caller, std::string_view wallet,
                           management::operation operation) const {
   if (!caller.transport_authenticated() || caller.source != forge::api::auth::caller_source::tls_certificate) {
      return false;
   }
   const auto fingerprint = caller.fingerprint.str();
   return std::ranges::any_of(settings.permissions, [&](const auto& entry) {
      return entry.fingerprint == fingerprint &&
             std::ranges::find(entry.operations, operation) != entry.operations.end() &&
             (wallet.empty() || std::ranges::find(entry.wallets, wallet) != entry.wallets.end());
   });
}

void plugin::impl::authorize(const forge::api::auth::authenticated_caller& caller, std::string_view wallet,
                             management::operation operation) const {
   if (!allowed(caller, wallet, operation)) {
      throw management::exceptions::permission_denied{"Wallet operation is not permitted"};
   }
   if (stopping.load(std::memory_order_acquire)) {
      throw management::exceptions::unavailable{"Wallet service is stopping"};
   }
}

std::shared_ptr<provider> plugin::impl::find(std::string name, bool create) {
   std::lock_guard lock{mutex};
   if (stopping.load(std::memory_order_acquire) || !scheduler) {
      throw management::exceptions::unavailable{"Wallet service is unavailable"};
   }
   const auto found = wallets.find(name);
   if (found != wallets.end()) {
      return found->second;
   }
   if (!create) {
      throw management::exceptions::not_found{"Wallet is not open"};
   }
   if (wallets.size() >= settings.max_wallets) {
      throw management::exceptions::resource_exhausted{"Wallet limit reached"};
   }
   auto wallet = std::make_shared<provider>(std::move(name));
   wallet->initialize(settings.directory, *scheduler, compute, 300, settings.max_pending);
   if (started) {
      wallet->startup();
   }
   wallets.emplace(wallet->name(), wallet);
   return wallet;
}

std::vector<std::shared_ptr<provider>> plugin::impl::snapshot() const {
   std::lock_guard lock{mutex};
   std::vector<std::shared_ptr<provider>> result;
   result.reserve(wallets.size());
   for (const auto& [name, wallet] : wallets) {
      result.push_back(wallet);
   }
   return result;
}

} // namespace forge::plugins::crypto::wallet
