module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <boost/asio/awaitable.hpp>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.db.authenticated.store;

import forge.db.authenticated.exceptions;
import forge.db.authenticated.hash;
import forge.db.authenticated.transaction;
import forge.db.authenticated.tree_engine;
import forge.db.core.record;

#include "details/store_impl.hxx"
#include "details/backend_call.hxx"

namespace forge::db::authenticated {

namespace {

boost::asio::awaitable<std::optional<root>> read_root(forge::db::core::snapshot& active,
                                                      const forge::db::core::family& family,
                                                      const digest& namespace_hash, version_id_t version) {
   const auto encoded = co_await detail::invoke_backend(
       [&] { return active.get(family, detail::version_key(namespace_hash, version)); });
   if (!encoded) {
      co_return std::nullopt;
   }
   auto decoded = detail::decode_root(*encoded);
   if (decoded.version != version) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated version record has the wrong version");
   }
   co_return decoded;
}

detail::get_record_fn snapshot_reader(const std::shared_ptr<forge::db::core::snapshot>& active,
                                      forge::db::core::family family,
                                      std::function<void(const forge::db::core::record_key&)> observer) {
   return [active, family = std::move(family), observer = std::move(observer)](
              forge::db::core::record_key key) -> boost::asio::awaitable<std::optional<bytes>> {
      if (observer) {
         try {
            observer(key);
         } catch (const forge::exceptions::base&) {
            throw;
         } catch (const std::exception& error) {
            FORGE_THROW_EXCEPTION(exceptions::backend_failure, "authenticated read observer failed",
                                  forge::exceptions::ctx("reason", error.what()));
         } catch (...) {
            FORGE_THROW_EXCEPTION(exceptions::backend_failure, "authenticated read observer failed");
         }
      }
      co_return co_await detail::invoke_backend([&] { return active->get(family, std::move(key)); });
   };
}

boost::asio::awaitable<checkpoint_summary> export_owned(auto owner, forge::db::core::snapshot active, root expected,
                                                        checkpoint_sink sink, checkpoint_limits bounds) {
   if (!active.active() || !active.belongs_to(*owner->driver) || !sink || !checkpoint_limits_are_valid(bounds)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_store,
                            "checkpoint export requires a same-origin snapshot and finite bounds");
   }
   auto pinned = std::make_shared<forge::db::core::snapshot>(std::move(active));
   const auto actual = co_await read_root(*pinned, owner->settings.family, owner->namespace_hash, expected.version);
   if (!actual) {
      FORGE_THROW_EXCEPTION(exceptions::version_unavailable, "checkpoint version is unavailable");
   }
   if (*actual != expected) {
      FORGE_THROW_EXCEPTION(exceptions::root_mismatch, "checkpoint does not match the exact retained root tuple");
   }
   co_return co_await detail::export_checkpoint_graphs(
       owner->settings.domain, expected, snapshot_reader(pinned, owner->settings.family, owner->settings.read_observer),
       std::move(sink), owner->settings.bounds, bounds);
}

} // namespace

store::store(std::shared_ptr<forge::db::core::driver> driver, config settings)
    : impl_{std::make_shared<impl>(std::move(driver), std::move(settings))} {}

store::store(std::shared_ptr<impl> implementation) : impl_{std::move(implementation)} {}

boost::asio::awaitable<checkpoint_summary> store::export_checkpoint(forge::db::core::snapshot active, root expected,
                                                                    checkpoint_sink sink,
                                                                    checkpoint_limits bounds) const {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_store, "authenticated store is not initialized");
   }
   // Do not retain `this` in a lazy coroutine: capture the SAME shared owner now.
   detail::validate_checkpoint_policy(impl_->settings.domain, expected, impl_->settings.bounds, bounds);
   return export_owned(impl_, std::move(active), expected, std::move(sink), bounds);
}

transaction store::join_restore(forge::db::core::transaction& active, root expected, checkpoint_limits bounds) const {
   if (!impl_ || !active.active() || !active.belongs_to(*impl_->driver) || !checkpoint_limits_are_valid(bounds)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_store,
                            "checkpoint restore requires a same-origin transaction and finite bounds");
   }
   if (active.captures_mutations()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_store, "checkpoint baseline cannot import revision history");
   }
   detail::validate_checkpoint_policy(impl_->settings.domain, expected, impl_->settings.bounds, bounds);
   return detail::transaction_access::make_restore(active, impl_->driver, impl_->settings.family,
                                                   impl_->settings.domain, impl_->namespace_hash,
                                                   impl_->settings.bounds, expected, bounds);
}

boost::asio::awaitable<std::optional<root>> store::earliest() const {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_store, "authenticated store is not initialized");
   }
   auto active = co_await detail::invoke_backend([&] { return impl_->driver->begin_read(); });
   const auto prefix = detail::version_prefix(impl_->namespace_hash);
   const auto page = co_await detail::invoke_backend([&] {
      return active.scan_page(impl_->settings.family,
                              forge::db::core::record_range{
                                  .begin = prefix,
                                  .prefix = prefix,
                                  .has_end = false,
                              },
                              {.limit = 1U});
   });
   if (page.entries.empty()) {
      co_return std::nullopt;
   }

   const auto version = detail::decode_version_key(page.entries.front().key, impl_->namespace_hash);
   auto result = detail::decode_root(page.entries.front().value);
   if (result.version != version) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated earliest version record is inconsistent");
   }
   co_return result;
}

boost::asio::awaitable<std::optional<root>> store::latest() const {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_store, "authenticated store is not initialized");
   }
   auto active = co_await detail::invoke_backend([&] { return impl_->driver->begin_read(); });
   const auto encoded = co_await detail::invoke_backend(
       [&] { return active.get(impl_->settings.family, detail::latest_key(impl_->namespace_hash)); });
   co_return encoded ? std::optional<root>{detail::decode_root(*encoded)} : std::nullopt;
}

boost::asio::awaitable<std::optional<root>> store::find_root(version_id_t version) const {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_store, "authenticated store is not initialized");
   }
   auto active = co_await detail::invoke_backend([&] { return impl_->driver->begin_read(); });
   co_return co_await read_root(active, impl_->settings.family, impl_->namespace_hash, version);
}

boost::asio::awaitable<std::optional<bytes>> store::get(version_id_t version, std::span<const std::byte> key) const {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_store, "authenticated store is not initialized");
   }
   auto owned = co_await detail::invoke_backend([&] { return impl_->driver->begin_read(); });
   auto active = std::make_shared<forge::db::core::snapshot>(std::move(owned));
   const auto anchor = co_await read_root(*active, impl_->settings.family, impl_->namespace_hash, version);
   if (!anchor) {
      FORGE_THROW_EXCEPTION(exceptions::version_unavailable, "authenticated state version is unavailable",
                            forge::exceptions::ctx("version", version));
   }

   auto engine = detail::tree_engine{
       canonical_tree_domain(impl_->settings.domain, proof_tree::state),
       anchor->state_size == 0 ? std::nullopt : std::optional<digest>{anchor->state_root},
       snapshot_reader(active, impl_->settings.family, impl_->settings.read_observer),
       impl_->settings.bounds,
   };
   co_return co_await engine.get(key);
}

boost::asio::awaitable<verified_range> store::scan_range(version_id_t version, range_request request,
                                                         proof_tree tree) const {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_store, "authenticated store is not initialized");
   }
   auto owned = co_await detail::invoke_backend([&] { return impl_->driver->begin_read(); });
   auto active = std::make_shared<forge::db::core::snapshot>(std::move(owned));
   const auto anchor = co_await read_root(*active, impl_->settings.family, impl_->namespace_hash, version);
   if (!anchor) {
      FORGE_THROW_EXCEPTION(exceptions::version_unavailable, "authenticated state version is unavailable",
                            forge::exceptions::ctx("version", version));
   }

   if (tree != proof_tree::state && tree != proof_tree::changes) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_range, "authenticated range tree is invalid");
   }
   const auto size = tree == proof_tree::state ? anchor->state_size : anchor->change_count;
   const auto tree_root = tree == proof_tree::state ? anchor->state_root : anchor->change_root;
   auto engine = detail::tree_engine{
       canonical_tree_domain(impl_->settings.domain, tree),
       size == 0 ? std::nullopt : std::optional<digest>{tree_root},
       snapshot_reader(active, impl_->settings.family, impl_->settings.read_observer),
       impl_->settings.bounds,
   };
   co_return co_await engine.scan_range(*anchor, std::move(request), tree);
}

boost::asio::awaitable<point_proof> store::prove(version_id_t version, std::span<const std::byte> key,
                                                 bool include_value) const {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_store, "authenticated store is not initialized");
   }
   auto owned = co_await detail::invoke_backend([&] { return impl_->driver->begin_read(); });
   auto active = std::make_shared<forge::db::core::snapshot>(std::move(owned));
   const auto anchor = co_await read_root(*active, impl_->settings.family, impl_->namespace_hash, version);
   if (!anchor) {
      FORGE_THROW_EXCEPTION(exceptions::version_unavailable, "authenticated state version is unavailable",
                            forge::exceptions::ctx("version", version));
   }

   auto engine = detail::tree_engine{
       canonical_tree_domain(impl_->settings.domain, proof_tree::state),
       anchor->state_size == 0 ? std::nullopt : std::optional<digest>{anchor->state_root},
       snapshot_reader(active, impl_->settings.family, impl_->settings.read_observer),
       impl_->settings.bounds,
   };
   co_return co_await engine.prove(*anchor, key, include_value);
}

boost::asio::awaitable<range_proof> store::prove_range(version_id_t version, range_request request,
                                                       proof_tree tree) const {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_store, "authenticated store is not initialized");
   }
   auto owned = co_await detail::invoke_backend([&] { return impl_->driver->begin_read(); });
   auto active = std::make_shared<forge::db::core::snapshot>(std::move(owned));
   const auto anchor = co_await read_root(*active, impl_->settings.family, impl_->namespace_hash, version);
   if (!anchor) {
      FORGE_THROW_EXCEPTION(exceptions::version_unavailable, "authenticated state version is unavailable",
                            forge::exceptions::ctx("version", version));
   }

   if (tree != proof_tree::state && tree != proof_tree::changes) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_range, "authenticated range tree is invalid");
   }
   const auto size = tree == proof_tree::state ? anchor->state_size : anchor->change_count;
   const auto tree_root = tree == proof_tree::state ? anchor->state_root : anchor->change_root;
   auto engine = detail::tree_engine{
       canonical_tree_domain(impl_->settings.domain, tree),
       size == 0 ? std::nullopt : std::optional<digest>{tree_root},
       snapshot_reader(active, impl_->settings.family, impl_->settings.read_observer),
       impl_->settings.bounds,
   };
   co_return co_await engine.prove_range(*anchor, std::move(request), tree);
}

boost::asio::awaitable<prune_result> store::prune_through(forge::db::core::transaction& active,
                                                          version_id_t inclusive_boundary,
                                                          prune_options options) const {
   if (!impl_ || !active.active()) {
      FORGE_THROW_EXCEPTION(exceptions::transaction_closed, "authenticated pruning requires an active transaction");
   }
   if (active.captures_mutations()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_prune,
                            "authenticated pruning cannot run while revision capture is active");
   }
   if (options.max_versions == 0 || options.max_versions >= forge::db::core::max_page_limit ||
       options.max_garbage_records == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_prune, "authenticated prune limits are invalid");
   }

   const auto latest_encoded = co_await detail::invoke_backend(
       [&] { return active.get_for_update(impl_->settings.family, detail::latest_key(impl_->namespace_hash)); });
   if (!latest_encoded) {
      co_return prune_result{.complete = true};
   }
   const auto latest = detail::decode_root(*latest_encoded);
   if (inclusive_boundary >= latest.version) {
      FORGE_THROW_EXCEPTION(
          exceptions::invalid_prune, "authenticated prune boundary must be older than the latest version",
          forge::exceptions::ctx("boundary", inclusive_boundary), forge::exceptions::ctx("latest", latest.version));
   }

   const auto prefix = detail::version_prefix(impl_->namespace_hash);
   const auto page = co_await detail::invoke_backend([&] {
      return active.scan_page(impl_->settings.family,
                              forge::db::core::record_range{
                                  .begin = prefix,
                                  .prefix = prefix,
                                  .has_end = false,
                              },
                              {.limit = options.max_versions + 1U});
   });

   auto get = [&active, family = impl_->settings.family](
                  forge::db::core::record_key key) -> boost::asio::awaitable<std::optional<bytes>> {
      co_return co_await detail::invoke_backend([&] { return active.get_for_update(family, std::move(key)); });
   };
   auto put = [&active, family = impl_->settings.family](forge::db::core::record_key key,
                                                         bytes value) -> boost::asio::awaitable<void> {
      co_await detail::invoke_backend([&] { return active.put(family, std::move(key), std::move(value)); });
   };

   auto result = prune_result{};
   auto versions_complete = true;
   struct prune_candidate {
      forge::db::core::record_key key;
      root value;
   };
   auto candidates = std::vector<prune_candidate>{};
   candidates.reserve(options.max_versions);

   for (const auto& entry : page.entries) {
      const auto version = detail::decode_version_key(entry.key, impl_->namespace_hash);
      if (version > inclusive_boundary) {
         break;
      }
      if (candidates.size() == options.max_versions) {
         versions_complete = false;
         break;
      }
      const auto removed = detail::decode_root(entry.value);
      if (removed.version != version) {
         FORGE_THROW_EXCEPTION(exceptions::corrupt_node, "authenticated version record is inconsistent");
      }
      const auto guard_prefix = detail::retention_guard_prefix(impl_->namespace_hash, version);
      const auto guards = co_await detail::invoke_backend([&] {
         return active.scan_page(impl_->settings.family,
                                 forge::db::core::record_range{
                                     .begin = guard_prefix,
                                     .prefix = guard_prefix,
                                     .has_end = false,
                                 },
                                 {.limit = 1});
      });
      if (!guards.entries.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_prune, "authenticated version is retained by a reversible revision",
                               forge::exceptions::ctx("version", version));
      }
      candidates.push_back(prune_candidate{
          .key = entry.key,
          .value = removed,
      });
   }

   for (const auto& candidate : candidates) {
      const auto& removed = candidate.value;
      if (removed.state_size != 0) {
         co_await detail::release_root(get, put, removed.state_root);
      }
      if (removed.change_count != 0) {
         co_await detail::release_root(get, put, removed.change_root);
      }
      co_await detail::invoke_backend([&] { return active.erase(impl_->settings.family, candidate.key); });
      ++result.versions_pruned;
   }

   const auto collected = co_await detail::invoke_backend(
       [&] { return detail::collect_garbage(active, impl_->settings.family, options.max_garbage_records); });
   result.nodes_collected = collected.nodes;
   result.values_collected = collected.values;
   result.complete = versions_complete && !collected.pending;
   co_return result;
}

boost::asio::awaitable<transaction> store::join(forge::db::core::transaction& active, version_id_t version) const {
   if (!impl_ || !active.active()) {
      FORGE_THROW_EXCEPTION(exceptions::transaction_closed,
                            "authenticated transaction cannot join a closed store transaction");
   }
   const auto encoded = co_await detail::invoke_backend(
       [&] { return active.get(impl_->settings.family, detail::latest_key(impl_->namespace_hash)); });
   auto base = encoded ? std::optional<root>{detail::decode_root(*encoded)} : std::nullopt;
   if (base && version <= base->version) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_version, "authenticated version must advance the current head",
                            forge::exceptions::ctx("version", version),
                            forge::exceptions::ctx("head", base ? base->version : 0));
   }

   co_return detail::transaction_access::make(active, impl_->settings.family, impl_->settings.domain,
                                              impl_->namespace_hash, impl_->settings.bounds, version, std::move(base));
}

} // namespace forge::db::authenticated
