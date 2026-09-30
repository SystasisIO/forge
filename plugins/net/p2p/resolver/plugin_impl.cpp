module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/steady_timer.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.resolver.plugin;

import forge.api.core.binding;
import forge.api.core.connection;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.registry;
import forge.api.core.types;
import forge.api.p2p.publication;
import forge.api.transport.connection;
import forge.api.transport.options;
import forge.asio.notification;
import forge.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.protocol;
import forge.plugins.net.p2p.resolver.exceptions;
import forge.plugins.net.p2p.resolver.types;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.types;

#include "details/config.hxx"
#include "details/plugin_impl.hxx"
#include "details/managed_remote_invoker.hxx"

namespace forge::plugins::net::p2p::resolver {
namespace {

[[nodiscard]] bool valid_protocol(std::string_view value) noexcept {
   return !value.empty() && value.front() == '/';
}

[[nodiscard]] error project_error(const forge::api::core::error_descriptor& value) {
   return error{
       .name = value.name,
       .identity = value.identity,
       .status_code = value.status_code,
       .retryable = value.retryable,
   };
}

[[nodiscard]] method project_method(const forge::api::core::method_descriptor& value) {
   auto errors = std::vector<error>{};
   errors.reserve(value.errors.size());
   for (const auto& error : value.errors) {
      errors.push_back(project_error(error));
   }
   return method{.name = value.name, .kind = value.kind, .errors = std::move(errors)};
}

} // namespace

plugin::impl::impl() = default;

plugin::impl::~impl() {
   request_stop_publications();
}

response query_resolver_protocol(const std::shared_ptr<plugin::impl>& owner, const query& request) {
   if (!owner) {
      FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P API resolver plugin owner has expired");
   }

   {
      auto lock = std::scoped_lock{owner->mutex};
      if (owner->stopping) {
         FORGE_THROW_EXCEPTION(exceptions::remote_stopped, "P2P API resolver plugin is stopping");
      }
      if (!owner->initialized) {
         FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P API resolver plugin is not initialized");
      }
   }

   return owner->query_local(request);
}

std::shared_ptr<forge::plugins::net::p2p::node::api> plugin::impl::require_p2p() const {
   const auto lock = std::scoped_lock{mutex};
   if (stopping) {
      FORGE_THROW_EXCEPTION(exceptions::remote_stopped, "P2P API resolver plugin is stopping");
   }
   if (!initialized || !p2p) {
      FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P API resolver plugin is not initialized");
   }
   return p2p;
}

std::chrono::milliseconds plugin::impl::query_deadline(resolve_options value) const {
   return value.query_deadline.count() > 0 ? value.query_deadline : to_ms(settings.query_deadline_ms);
}

std::chrono::milliseconds plugin::impl::open_deadline(resolve_options value) const {
   return value.open_deadline.count() > 0 ? value.open_deadline : to_ms(settings.open_deadline_ms);
}

std::chrono::milliseconds plugin::impl::request_deadline(resolve_options value) const {
   return value.request_deadline.count() > 0 ? value.request_deadline : to_ms(settings.request_deadline_ms);
}

void plugin::impl::evict_cache_locked() {
   while (cache.size() > settings.max_cached_peers) {
      auto expired = std::ranges::find_if(
          cache, [](const auto& item) { return item.second.expires_at <= std::chrono::steady_clock::now(); });
      if (expired != cache.end()) {
         cache.erase(expired);
         continue;
      }
      auto oldest = cache.begin();
      for (auto iterator = cache.begin(); iterator != cache.end(); ++iterator) {
         if (iterator->second.stored_at < oldest->second.stored_at) {
            oldest = iterator;
         }
      }
      cache.erase(oldest);
   }
}

std::optional<std::vector<entry>> plugin::impl::cached_peer(const forge::net::p2p::peer_id& peer,
                                                            resolve_options options) const {
   if (options.force_refresh) {
      return std::nullopt;
   }
   const auto now = std::chrono::steady_clock::now();
   const auto key = peer.to_string();
   auto lock = std::scoped_lock{mutex};
   const auto found = cache.find(key);
   if (found == cache.end() || found->second.expires_at <= now) {
      return std::nullopt;
   }
   return found->second.apis;
}

void plugin::impl::store_peer(const forge::net::p2p::peer_id& peer, std::vector<entry> entries) {
   const auto now = std::chrono::steady_clock::now();
   auto lock = std::scoped_lock{mutex};
   cache[peer.to_string()] = cache_record{
       .apis = std::move(entries),
       .expires_at = now + to_ms(settings.cache_ttl_ms),
       .stored_at = now,
   };
   evict_cache_locked();
}

std::vector<entry> plugin::impl::local_snapshot() const {
   auto publications = std::shared_ptr<detail::publication_catalog>{};
   {
      const auto lock = std::scoped_lock{mutex};
      publications = local_publications;
   }
   return detail::publication_catalog_snapshot(publications);
}

forge::api::p2p::publication
plugin::impl::add_local(forge::api::core::binding_plan plan, forge::net::p2p::protocol_id route,
                        publish_options options) {
   auto p2p_api = require_p2p();
   validate_transport_options(options.transport);
   if (route.value.empty() || route.value.front() != '/' || plan.exports.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::duplicate_api, "resolver API publication is invalid",
                            forge::exceptions::ctx("protocol", route.value));
   }
   if (route == protocol) {
      FORGE_THROW_EXCEPTION(exceptions::duplicate_api, "resolver protocol is reserved for resolver metadata",
                            forge::exceptions::ctx("protocol", route.value));
   }
   const auto route_value = route.value;

   auto projected = std::vector<entry>{};
   projected.reserve(plan.exports.size());
   for (const auto& descriptor : plan.exports) {
      projected.push_back(project_descriptor(descriptor, route, options.transport));
   }
   for (const auto& value : projected) {
      validate_entry(value, "local");
   }

   try {
      auto publications = std::shared_ptr<detail::publication_catalog>{};
      {
         const auto lock = std::scoped_lock{mutex};
         publications = local_publications;
      }
      auto publication = detail::publish_catalog_api(
         publications, *p2p_api, std::move(plan), std::move(route), std::move(options.transport),
         std::move(projected), settings.max_apis_per_peer);

      auto stopping_now = false;
      {
         const auto lock = std::scoped_lock{mutex};
         stopping_now = stopping || !initialized;
      }
      if (stopping_now) {
         publication.close();
         FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P API resolver plugin is stopping");
      }
      return publication;
   } catch (const forge::plugins::net::p2p::node::exceptions::route_conflict& error) {
      FORGE_THROW_EXCEPTION(exceptions::duplicate_api, "P2P API route conflicts with resolver publication",
                            forge::exceptions::ctx("protocol", route_value),
                            forge::exceptions::ctx("error", error.message()));
   }
}

response plugin::impl::query_local(const query& request) const {
   auto entries = local_snapshot();
   if (!request.apis.empty()) {
      auto filtered = std::vector<entry>{};
      for (const auto& requested : request.apis) {
         if (auto selected = select_compatible(entries, requested)) {
            filtered.push_back(std::move(*selected));
         }
      }
      entries = std::move(filtered);
   }
   validate_response(entries);
   return response{.apis = std::move(entries)};
}

std::string plugin::impl::api_key(const forge::api::core::api_id& id, std::uint16_t major) {
   return id.value + "#" + std::to_string(major);
}

entry plugin::impl::project_descriptor(const forge::api::core::descriptor& descriptor,
                                       const forge::net::p2p::protocol_id& route,
                                       const forge::api::transport::options& options) const {
   auto methods = std::vector<method>{};
   methods.reserve(descriptor.methods.size());
   for (const auto& method : descriptor.methods) {
      methods.push_back(project_method(method));
   }
   return entry{
       .id = descriptor.id,
       .version = descriptor.version,
       .protocol = route.value,
       .codec = options.codec,
       .max_inflight = static_cast<std::uint64_t>(options.max_inflight),
       .max_frame_size = options.max_frame_size,
       .methods = std::move(methods),
   };
}

void plugin::impl::validate_entry(const entry& value, std::string_view source) const {
   if (value.id.value.empty() || value.version.major == 0 || !valid_protocol(value.protocol)) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "resolver API entry is invalid",
                            forge::exceptions::ctx("source", source), forge::exceptions::ctx("api", value.id.value),
                            forge::exceptions::ctx("protocol", value.protocol));
   }
   if (value.codec.value.empty() || value.max_inflight == 0 || value.max_frame_size == 0) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "resolver API entry limits are invalid",
                            forge::exceptions::ctx("source", source), forge::exceptions::ctx("api", value.id.value));
   }
   if (value.max_frame_size > (std::numeric_limits<std::uint32_t>::max)()) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "resolver API max frame size exceeds transport limit",
                            forge::exceptions::ctx("source", source), forge::exceptions::ctx("api", value.id.value));
   }
   if (value.methods.size() > settings.max_methods_per_api) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "resolver API method limit exceeded",
                            forge::exceptions::ctx("source", source), forge::exceptions::ctx("api", value.id.value));
   }
   auto method_names = std::set<std::string>{};
   for (const auto& method : value.methods) {
      if (method.name.empty() || !method_names.insert(method.name).second) {
         FORGE_THROW_EXCEPTION(exceptions::protocol_error, "resolver API method is invalid",
                               forge::exceptions::ctx("source", source), forge::exceptions::ctx("api", value.id.value));
      }
      if (method.errors.size() > settings.max_errors_per_method) {
         FORGE_THROW_EXCEPTION(exceptions::protocol_error, "resolver API error limit exceeded",
                               forge::exceptions::ctx("source", source), forge::exceptions::ctx("api", value.id.value),
                               forge::exceptions::ctx("method", method.name));
      }
   }
}

void plugin::impl::validate_response(const std::vector<entry>& entries) const {
   if (entries.size() > settings.max_apis_per_peer) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "resolver API response limit exceeded");
   }
   auto keys = std::set<std::string>{};
   for (const auto& value : entries) {
      validate_entry(value, "remote");
      const auto key = api_key(value.id, value.version.major) + "#" + std::to_string(value.version.revision);
      if (!keys.insert(key).second) {
         FORGE_THROW_EXCEPTION(exceptions::protocol_error, "resolver API response contains duplicate entry",
                               forge::exceptions::ctx("api", value.id.value));
      }
   }
}

void plugin::impl::validate_descriptor_compatible(const forge::api::core::descriptor& descriptor,
                                                  const entry& remote) const {
   if (!forge::api::core::compatible(forge::api::core::descriptor{.id = remote.id, .version = remote.version},
                                     forge::api::core::api_ref{
                                         .id = descriptor.id,
                                         .major = descriptor.version.major,
                                         .min_revision = descriptor.version.revision,
                                     })) {
      FORGE_THROW_EXCEPTION(exceptions::incompatible_api, "remote API version is incompatible",
                            forge::exceptions::ctx("api", descriptor.id.value));
   }
   for (const auto& local_method : descriptor.methods) {
      const auto found = std::ranges::find_if(remote.methods, [&](const auto& candidate) {
         return forge::api::core::compatible(
             forge::api::core::method_descriptor{.name = candidate.name, .kind = candidate.kind}, local_method);
      });
      if (found == remote.methods.end()) {
         FORGE_THROW_EXCEPTION(exceptions::incompatible_api, "remote API method is incompatible",
                               forge::exceptions::ctx("api", descriptor.id.value),
                               forge::exceptions::ctx("method", local_method.name));
      }
   }
}

std::optional<entry> plugin::impl::select_compatible(const std::vector<entry>& entries,
                                                     const forge::api::core::api_ref& requested) const {
   auto selected = std::optional<entry>{};
   for (const auto& value : entries) {
      if (!forge::api::core::compatible(forge::api::core::descriptor{.id = value.id, .version = value.version},
                                        requested)) {
         continue;
      }
      if (!selected || value.version.revision > selected->version.revision) {
         selected = value;
      }
   }
   return selected;
}

void plugin::impl::install_protocol(const std::shared_ptr<forge::plugins::net::p2p::node::api>& p2p_api) {
   auto plan = make_resolver_protocol_plan(protocol_registry, weak_from_this());
   auto publication = p2p_api->publish_api(std::move(plan), protocol, resolver_transport);
   auto stopping_now = false;
   {
      const auto lock = std::scoped_lock{mutex};
      stopping_now = stopping;
      if (!stopping_now) {
         resolver_protocol_publication = std::move(publication);
      }
   }
   if (stopping_now) {
      publication.close();
      FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P API resolver plugin is stopping");
   }
}

void plugin::impl::request_stop_publications() noexcept {
   auto publications = std::shared_ptr<detail::publication_catalog>{};
   {
      const auto lock = std::scoped_lock{mutex};
      stopping = true;
      publications = local_publications;
   }
   resolver_protocol_publication.close();
   detail::request_close_publication_catalog(publications);
}

boost::asio::awaitable<void> plugin::impl::shutdown_publications() {
   request_stop_publications();

   auto publications = std::shared_ptr<detail::publication_catalog>{};
   {
      const auto lock = std::scoped_lock{mutex};
      publications = local_publications;
   }

   auto failure = std::exception_ptr{};
   try {
      co_await resolver_protocol_publication.async_close();
   } catch (...) {
      failure = std::current_exception();
   }

   try {
      co_await detail::async_close_publication_catalog(publications);
   } catch (...) {
      if (!failure) {
         failure = std::current_exception();
      }
   }

   protocol_registry.clear();
   if (failure) {
      std::rethrow_exception(failure);
   }
}

boost::asio::awaitable<resolution>
plugin::impl::resolve_remote(forge::net::p2p::peer_id peer, forge::api::core::api_ref api, resolve_options options) {
   auto entries = cached_peer(peer, options);
   if (!entries) {
      entries = co_await query_remote_apis(peer, options);
      validate_response(*entries);
      store_peer(peer, *entries);
   }
   if (auto selected = select_compatible(*entries, api)) {
      co_return resolution{.api = std::move(*selected)};
   }
   const auto has_same_api = std::ranges::any_of(
       *entries, [&](const auto& candidate) { return candidate.id == api.id && candidate.version.major == api.major; });
   if (has_same_api) {
      FORGE_THROW_EXCEPTION(exceptions::incompatible_api, "remote peer has incompatible API revision",
                            forge::exceptions::ctx("api", api.id.value));
   }
   FORGE_THROW_EXCEPTION(exceptions::not_found, "remote peer does not advertise requested API",
                         forge::exceptions::ctx("api", api.id.value));
}

boost::asio::awaitable<resolved_connection>
plugin::impl::open_resolved_connection(forge::net::p2p::peer_id peer, forge::api::core::api_ref api,
                                       forge::api::core::descriptor descriptor, resolve_options options) {
   auto p2p_api = require_p2p();
   auto selected = co_await resolve_remote(peer, api, options);
   validate_descriptor_compatible(descriptor, selected.api);
   auto protocol = forge::net::p2p::protocol_id{.value = selected.api.protocol};
   auto connection = co_await p2p_api->open_api_connection(
       std::move(peer), std::move(protocol),
       forge::plugins::net::p2p::node::remote_options{
           .open_deadline = open_deadline(options),
           .codec = selected.api.codec,
           .max_inflight = static_cast<std::size_t>(selected.api.max_inflight),
           .deadline = request_deadline(options),
           .max_frame_size = static_cast<std::uint32_t>(selected.api.max_frame_size),
       });
   co_return resolved_connection{
       .connection = std::move(connection),
       .selected =
           forge::api::core::api_ref{
               .id = std::move(selected.api.id),
               .major = selected.api.version.major,
               .min_revision = selected.api.version.revision,
           },
   };
}

boost::asio::awaitable<std::vector<entry>> plugin::impl::query_remote_apis(forge::net::p2p::peer_id peer,
                                                                           resolve_options options) {
   auto p2p_api = require_p2p();
   co_return co_await query_resolver_peer(*p2p_api, std::move(peer), protocol,
                                          forge::plugins::net::p2p::node::remote_options{
                                              .open_deadline = open_deadline(options),
                                              .deadline = query_deadline(options),
                                          });
}

void plugin::impl::register_managed(const std::shared_ptr<detail::managed_remote_invoker>& value) {
   auto lock = std::scoped_lock{mutex};
   managed_remotes.erase(
       std::remove_if(managed_remotes.begin(), managed_remotes.end(), [](const auto& item) { return item.expired(); }),
       managed_remotes.end());
   if (stopping || !initialized) {
      FORGE_THROW_EXCEPTION(exceptions::remote_stopped, "managed remote owner is not running");
   }
   if (managed_remotes.size() >= settings.max_managed_remotes) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_remote, "managed remote capacity is exhausted");
   }
   managed_remotes.push_back(value);
}

void plugin::impl::request_stop_managed() noexcept {
   auto values = std::vector<std::shared_ptr<detail::managed_remote_invoker>>{};
   {
      auto lock = std::scoped_lock{mutex};
      stopping = true;
      for (auto& value : managed_remotes) {
         if (auto owned = value.lock()) {
            values.push_back(std::move(owned));
         }
      }
   }
   for (const auto& value : values) {
      value->request_stop();
   }
}

boost::asio::awaitable<void> plugin::impl::shutdown_managed() {
   auto values = std::vector<std::shared_ptr<detail::managed_remote_invoker>>{};
   {
      auto lock = std::scoped_lock{mutex};
      stopping = true;
      for (auto& value : managed_remotes) {
         if (auto owned = value.lock()) {
            values.push_back(std::move(owned));
         }
      }
      managed_remotes.clear();
   }
   for (const auto& value : values) {
      value->request_stop();
   }
   for (const auto& value : values) {
      co_await value->async_stop();
   }
}

} // namespace forge::plugins::net::p2p::resolver
