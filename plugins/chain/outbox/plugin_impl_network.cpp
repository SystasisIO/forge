module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <functional>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>
#include <variant>

module forge.plugins.chain.outbox.plugin;

import forge.api.chain.outbox;
import forge.api.http.proxy;
import forge.asio.notification;
import forge.asio.task;
import forge.chain.api.verified_client_factory;
import forge.chain.protocol.info;
import forge.chain.transaction.types;
import forge.api.core.exceptions;
import forge.chain.api.raw_client;
import forge.chain.api.submission_client;
import forge.chain.protocol.transaction_query;
import forge.exceptions;
import forge.net.http.base_url;
import forge.net.http.client;
import forge.plugins.net.p2p.resolver.api;

#include "details/plugin_impl.hxx"

namespace forge::plugins::chain::outbox {

void plugin::impl::configure_http(std::vector<http_profile> profiles) {
   if (!options.p2p && profiles.empty()) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                            "Chain outbox requires an HTTP endpoint when P2P is disabled");
   }

   http_endpoints.reserve(profiles.size());
   for (auto& profile : profiles) {
      auto url = forge::net::http::parse_base_url(profile.url);
      if ((url.scheme != "http" && url.scheme != "https") || profile.url.find_first_of("@?#") != std::string::npos) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                               "Chain endpoint requires an HTTP URL without credentials, query or fragment");
      }

      http_endpoints.emplace_back(std::move(profile.name), std::move(url));
   }
}

forge::api::chain::outbox::transport plugin::impl::connect() {
   using resolver = forge::plugins::net::p2p::resolver::api;
   auto peers = options.p2p ? apis->get<resolver>(resolver::ref()).shared() : std::shared_ptr<resolver>{};
   auto clients = std::map<std::string, std::shared_ptr<forge::net::http::client>>{};
   for (auto& [name, url] : http_endpoints) {
      clients.emplace(std::move(name), std::make_shared<forge::net::http::client>(scheduler->runtime_context(),
                                                                               std::move(url)));
   }
   http_endpoints.clear();
   const auto http = std::make_shared<const decltype(clients)>(std::move(clients));

   auto open = [peers, http]<typename Api>(
                   const forge::api::chain::destination& target) -> boost::asio::awaitable<forge::api::core::handle<Api>> {
      if (const auto* peer = std::get_if<forge::net::p2p::peer_id>(&target)) {
         if (!peers) {
            FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::unavailable, "Chain P2P transport is disabled");
         }
         co_return co_await peers->remote<Api>(*peer);
      }

      const auto client = http->find(std::get<forge::api::chain::http_endpoint>(target).name);
      if (client == http->end()) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::unavailable, "Chain HTTP endpoint is not configured");
      }
      co_return co_await forge::api::http::remote<Api>(*client->second);
   };

   info = [open, verification = options.verification](forge::api::chain::destination target)
       -> boost::asio::awaitable<forge::chain::protocol::info_response> {
      auto handle = co_await open.operator()<forge::chain::api::info>(target);
      auto client = forge::chain::api::make_verified_client(
          forge::chain::api::raw_client{{.information = std::move(handle)}}, verification);
      co_return co_await client.get_info();
   };

   using outbox = forge::api::chain::outbox;
   return outbox::transport{
       .submit = [weak = weak_from_this(), open](forge::chain::protocol::chain_id chain, forge::api::chain::destination target,
                                         forge::chain::protocol::transaction_submit_request request,
                                         std::stop_token cancel)
           -> boost::asio::awaitable<forge::chain::protocol::transaction_submit_response> {
          const auto state = weak.lock();
          if (!state || cancel.stop_requested() || state->stop.stop_requested()) {
             FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "Chain submission was stopped");
          }
          if (chain != state->options.verification.chain) {
             FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                                   "Outbox transaction belongs to another chain");
          }
          // Verify the destination before giving it a signed transaction.
          (void)co_await state->info(target);
          auto service = co_await open.operator()<forge::chain::api::submission>(target);
          co_await state->flush();
          if (cancel.stop_requested() || state->stop.stop_requested()) {
             FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "Chain submission was stopped");
          }
          const auto expiration = request.transaction.get_signed_transaction().expiration;
          if (expiration.chrono() <= std::chrono::system_clock::now()) {
             FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::deadline_exceeded,
                                   "Chain transaction expired before submission");
          }
          auto submission = forge::chain::api::submission_client{std::move(service), state->options.verification.service_limits};
          co_return co_await submission.submit(std::move(request));
       },
       .status = [weak = weak_from_this(), open](forge::chain::protocol::chain_id chain, forge::api::chain::destination target,
                                         forge::chain::protocol::transaction_id id, std::stop_token cancel)
           -> boost::asio::awaitable<forge::chain::protocol::transaction_status_response> {
          const auto state = weak.lock();
          if (!state || cancel.stop_requested() || state->stop.stop_requested()) {
             FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "Chain status query was stopped");
          }
          if (chain != state->options.verification.chain) {
             FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                                   "Outbox transaction belongs to another chain");
          }
          auto handle = co_await open.operator()<forge::chain::api::transaction>(target);
          auto client = forge::chain::api::make_verified_client(
              forge::chain::api::raw_client{{.transactions = std::move(handle)}}, state->options.verification);
          co_return co_await client.get_transaction_status({.id = id});
       },
       .configured = [peers = std::move(peers), http](const forge::api::chain::destination& target) {
          if (const auto* endpoint = std::get_if<forge::api::chain::http_endpoint>(&target)) {
             return http->contains(endpoint->name);
          }
          return static_cast<bool>(peers);
       },
   };
}

} // namespace forge::plugins::chain::outbox
