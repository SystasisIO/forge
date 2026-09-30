module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/this_coro.hpp>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

module forge.plugins.crypto.wallet.plugin;

import forge.api.core.exceptions;
import forge.asio.task;
import forge.asio.compute;
import forge.config.core.decode;
import forge.crypto.keystore.password;
import forge.crypto.wallet.api;
import forge.plugins.crypto.wallet.provider_source;
import forge.plugins.net.http.server.api;
import forge.plugins.net.http.server.types;
import forge.raw.raw;

#include "details/plugin_impl.hxx"
#include "details/api_impl.hxx"
#include "details/provider_source_impl.hxx"

namespace forge::plugins::crypto::wallet {
namespace management = forge::crypto::wallet;

plugin::plugin(plugin_options options) : impl_{std::make_shared<impl>(std::move(options))} {}
plugin::~plugin() = default;

forge::app::plugin_descriptor plugin::descriptor(plugin_options options) {
   return {.id = {"forge.plugins.crypto.wallet"},
           .enabled_by_default = false,
           .factory = [options = std::move(options)] { return std::make_unique<plugin>(options); }};
}

forge::app::plugin_id plugin::id() const {
   return {.value = "forge.plugins.crypto.wallet"};
}
std::string plugin::version() const {
   return "1.0.0";
}

std::optional<forge::config::core::component_descriptor> plugin::describe_config() const {
   return forge::config::core::describe_component<config>("plugins.crypto.wallet");
}

boost::asio::awaitable<void> plugin::configure(forge::config::core::component_view view) {
   auto decoded = forge::config::core::decode<config>(view.source(), view.section());
   if (!decoded.ok()) {
      throw management::exceptions::invalid_request{"Invalid wallet configuration"};
   }
   impl_->settings = std::move(decoded.value);
   impl_->validate();
   co_return;
}

boost::asio::awaitable<void> plugin::provide(forge::api::core::provider& provider) {
   provider.install<management::api>(std::make_shared<api_impl>(impl_));
   provider.install<provider_source>(std::make_shared<provider_source_impl>(impl_));
   co_return;
}

boost::asio::awaitable<void> plugin::initialize(forge::app::plugin_context& context) {
   impl_->validate();
   if (!context.has_compute()) {
      throw management::exceptions::invalid_request{"Wallet plugin requires the application's bounded compute pool"};
   }
   impl_->scheduler = &context.scheduler();
   impl_->compute = context.compute();
   for (const auto& startup : impl_->settings.wallets) {
      if (!impl_->wallets.contains(startup.name)) {
         impl_->wallets.emplace(startup.name, std::make_shared<wallet::provider>(startup.name));
      }
   }
   for (const auto& wallet : impl_->snapshot()) {
      wallet->initialize(impl_->settings.directory, context.scheduler(), context.compute(), 300,
                         impl_->settings.max_pending);
   }
   if (impl_->settings.publish_http) {
      impl_->http = context.apis().get<forge::plugins::net::http::server::api>(
          {.id = {"forge.plugins.net.http.server"}, .major = 2});
      co_await impl_->http->publish<management::api>({.require_mutual_tls = true});
   }
}

boost::asio::awaitable<void> plugin::startup() {
   for (const auto& wallet : impl_->snapshot()) {
      wallet->startup();
   }
   for (const auto& entry : impl_->settings.wallets) {
      auto wallet = impl_->find(entry.name, false);
      static_cast<void>(co_await wallet->set_timeout(entry.timeout_seconds));
      if (entry.open) {
         static_cast<void>(co_await wallet->open());
      }
      if (!entry.unlock_file.empty()) {
         // A single, explicitly configured startup operation. Manual lock never
         // schedules this again. File reading uses the existing bounded compute
         // executor, not a nested task-scheduler admission.
         co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
         auto password =
             co_await impl_->compute.execute({.name = "wallet.startup_password"}, [path = entry.unlock_file] {
                return forge::crypto::keystore::read_password(
                    {.source = forge::crypto::keystore::password_source::file, .file = path});
             });
         static_cast<void>(co_await wallet->unlock(std::move(password)));
         co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::enable_terminal_cancellation{});
      }
   }
   std::lock_guard lock{impl_->mutex};
   impl_->started = true;
}

void plugin::request_stop() noexcept {
   // Schema discovery instantiates and destroys plugins without initializing
   // them. Such an instance does not own injected providers' lifecycle.
   if (!impl_->scheduler) {
      return;
   }
   impl_->stopping.store(true, std::memory_order_release);
   std::lock_guard lock{impl_->mutex};
   for (const auto& [name, wallet] : impl_->wallets) {
      wallet->request_stop();
   }
}

boost::asio::awaitable<void> plugin::shutdown() {
   request_stop();
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   for (const auto& wallet : impl_->snapshot()) {
      co_await wallet->shutdown();
   }
}

} // namespace forge::plugins::crypto::wallet
