module;

#include <boost/asio/awaitable.hpp>

#include <forge/exceptions/macros.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

module forge.net.p2p.dht.record_store;

import forge.asio.gate;
import forge.net.p2p.exceptions;

#include "details/dht_record_store_impl.hxx"
#include "details/memory_dht_record_store_persistence.hxx"

namespace forge::net::p2p {

void validate(const dht::record_store::options& options) {
   if (options.max_values == 0 || options.max_providers == 0 || options.max_local_providers == 0 ||
       options.max_providers_per_key == 0 || options.max_total_bytes == 0 || options.max_record_bytes == 0 ||
       options.hydration_page_limit == 0 || options.prune_page_limit == 0 || options.max_hydration_pages == 0 ||
       options.max_persistence_waiters == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "DHT record store limits must be positive");
   }
}

void validate(const dht::profile& profile, const dht::record_store::options& options) {
   validate_declaration(profile);
   validate(options);
   if (options.max_providers_per_key > profile.limits.replication) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "DHT provider per-key capacity exceeds profile replication");
   }
}

dht::record_store::persistence::~persistence() = default;

dht::record_store::record_store(dht::profile profile) {
   auto defaults = options_for(profile);
   impl_ = std::make_shared<impl>(std::move(profile), std::move(defaults));
}

dht::record_store::record_store(dht::profile profile, options options_value)
    : impl_(std::make_shared<impl>(std::move(profile), std::move(options_value))) {}

dht::record_store::~record_store() = default;
dht::record_store::record_store(record_store&&) noexcept = default;
dht::record_store& dht::record_store::operator=(record_store&&) noexcept = default;

std::shared_ptr<dht::record_store::persistence> dht::record_store::make_memory_persistence() {
   return make_memory_dht_record_store_persistence();
}

dht::record_store::options dht::record_store::options_for(const dht::profile& profile) {
   validate_declaration(profile);
   auto result = options{};
   result.max_providers_per_key = profile.limits.replication;
   result.max_record_bytes = profile.limits.max_record_size;
   return result;
}

boost::asio::awaitable<dht::record_store::put_result>
dht::record_store::async_put(value_record incoming, std::chrono::system_clock::time_point now) {
   return impl::async_put_owned(impl_, std::move(incoming), now);
}

boost::asio::awaitable<std::optional<dht::record_store::put_result>>
dht::record_store::async_put_received(value_record incoming, std::chrono::system_clock::time_point now) {
   return impl::async_put_received_owned(impl_, std::move(incoming), now);
}

boost::asio::awaitable<void> dht::record_store::async_upsert_provider(provider_record value,
                                                                      std::chrono::system_clock::time_point now) {
   return impl::async_upsert_provider_owned(impl_, std::move(value), now);
}

boost::asio::awaitable<void> dht::record_store::async_remove_provider(provider_key key) {
   return impl::async_remove_provider_owned(impl_, std::move(key));
}

boost::asio::awaitable<void> dht::record_store::async_hydrate(std::chrono::system_clock::time_point now) {
   return impl::async_hydrate_owned(impl_, now);
}

boost::asio::awaitable<dht::record_store::prune_result>
dht::record_store::async_prune_expired(std::chrono::system_clock::time_point now) {
   return impl::async_prune_expired_owned(impl_, now);
}

boost::asio::awaitable<void> dht::record_store::async_flush() {
   return impl::async_flush_owned(impl_);
}

boost::asio::awaitable<void> dht::record_store::async_close() {
   return impl::async_close_owned(impl_);
}

std::optional<dht::record_store::value_record>
dht::record_store::find_value(const dht::key& key, std::chrono::system_clock::time_point now) const {
   return impl_->find_value(key, now);
}

std::vector<dht::record_store::provider_record>
dht::record_store::find_providers(const dht::key& key, std::size_t limit,
                                  std::chrono::system_clock::time_point now) const {
   return impl_->find_providers(key, limit, now);
}

dht::record_store::persistence_status dht::record_store::persistence_state() const {
   return impl_->persistence_state();
}

} // namespace forge::net::p2p
