module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>
#include <forge/log/macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

module forge.plugins.db.store.plugin;

import forge.app.plugin_context;
import forge.asio.compute;
import forge.asio.exceptions;
import forge.exceptions;
import forge.log.logger;
import forge.db.blob.store;
import forge.db.core.driver;
import forge.db.object.store;
import forge.db.revision.store;
import forge.plugins.db.store.exceptions;
import forge.plugins.db.store.types;

#include "details/plugin_impl.hxx"

namespace forge::plugins::db::store {

boost::asio::awaitable<void> plugin::impl::open() {
   auto pending = std::vector<pending_open>{};
   const auto guarded = !opening.prerequisites.empty();
   auto bindings = std::vector<std::shared_ptr<managed_store>>{};
   auto restore_phase = phase::collecting;
   const auto generation = lifecycle.snapshot().generation;
   {
      auto lock = std::scoped_lock{mutex};
      if (opening_failure) {
         std::rethrow_exception(opening_failure);
      }
      const auto state = current.load();
      if (state == phase::ready) {
         co_return;
      }
      if (state == phase::closed) {
         FORGE_THROW_EXCEPTION(exceptions::stopped, "db store plugin is stopping");
      }
      if (!enabled) {
         current.store(phase::ready);
         co_return;
      }
      if (state != phase::collecting) {
         FORGE_THROW_EXCEPTION(exceptions::initialize_failed, "db store plugin is not initialized");
      }

      restore_phase = state;
      current.store(phase::opening);
      pending.reserve(stores.size());
      if (guarded) {
         bindings.reserve(stores.size());
      }
      for (const auto& [name, record] : stores) {
         auto item = pending_open{
             .name = name,
             .options = record->options,
             .initialize_layers = record->initialize_layers,
             .owns_driver = record->owns_driver,
             .driver = record->driver,
         };
         if (!item.driver) {
            const auto configured =
                std::ranges::find_if(settings.stores, [&](const auto& value) { return value.name == name; });
            if (configured == settings.stores.end()) {
               current.store(restore_phase);
               FORGE_THROW_EXCEPTION(exceptions::initialize_failed, "db store configured store is not registered",
                                     forge::exceptions::ctx("store", name));
            }
            item.config = *configured;
         }
         pending.push_back(std::move(item));
         if (guarded) {
            bindings.push_back(record);
         }
      }
   }

   const auto* failed = static_cast<const pending_open*>(nullptr);
   try {
      // Validate the ENTIRE dependency graph before any configured driver open.
      // This is ordering of the existing finite batch, not a runtime queue.
      auto order = std::vector<std::size_t>{};
      auto checks = std::vector<std::optional<std::size_t>>{};
      order.reserve(pending.size());
      if (guarded) {
         if (opening.prerequisites.size() > pending.size()) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_config, "too many store opening prerequisites");
         }
         auto edges = std::vector<std::vector<std::size_t>>(pending.size());
         auto incoming = std::vector<std::size_t>(pending.size());
         checks.resize(pending.size());
         const auto index_of = [&](const std::string& name) {
            const auto found = std::ranges::find(pending, name, &pending_open::name);
            if (found == pending.end()) {
               FORGE_THROW_EXCEPTION(exceptions::invalid_config, "unknown store in opening prerequisite",
                                     forge::exceptions::ctx("store", name));
            }
            return static_cast<std::size_t>(found - pending.begin());
         };
         for (auto index = std::size_t{}; index < opening.prerequisites.size(); ++index) {
            const auto& prerequisite = opening.prerequisites[index];
            if (prerequisite.store.empty() || prerequisite.before.empty() || !prerequisite.check ||
                prerequisite.before.size() > pending.size()) {
               FORGE_THROW_EXCEPTION(exceptions::invalid_config, "invalid store opening prerequisite");
            }
            const auto source = index_of(prerequisite.store);
            if (checks[source] || !pending[source].options.object) {
               FORGE_THROW_EXCEPTION(exceptions::invalid_config, "duplicate prerequisite or missing ObjectDB layer");
            }
            checks[source] = index;
            for (const auto& name : prerequisite.before) {
               const auto target = index_of(name);
               if (source == target || std::ranges::find(edges[source], target) != edges[source].end()) {
                  FORGE_THROW_EXCEPTION(exceptions::invalid_config, "self or duplicate store opening dependency");
               }
               edges[source].push_back(target);
               ++incoming[target];
            }
         }
         for (auto index = std::size_t{}; index < pending.size(); ++index) {
            if (incoming[index] == 0U) {
               order.push_back(index);
            }
         }
         for (auto cursor = std::size_t{}; cursor < order.size(); ++cursor) {
            for (const auto target : edges[order[cursor]]) {
               if (--incoming[target] == 0U) {
                  order.push_back(target);
               }
            }
         }
         if (order.size() != pending.size()) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_config, "cyclic store opening prerequisites");
         }
      } else {
         for (auto index = std::size_t{}; index < pending.size(); ++index) {
            order.push_back(index);
         }
      }
      for (const auto index : order) {
         auto& item = pending[index];
         if (guarded && lifecycle.snapshot().stop_requested) {
            FORGE_THROW_EXCEPTION(exceptions::stopped, "store opening prerequisite was stopped");
         }
         failed = &item;
         if (!item.driver) {
            item.driver = co_await make_configured_driver(*item.config);
            item.owns_driver = true;
         }
         if (!item.driver) {
            FORGE_THROW_EXCEPTION(exceptions::initialize_failed, "db store has no driver",
                                  forge::exceptions::ctx("store", item.name));
         }
         if (item.options.object) {
            const auto settings = forge::db::object::store::config{.family = item.options.object->family};
            auto objects = co_await (
                item.initialize_layers
                    ? forge::db::object::store::open(item.driver, settings, item.options.object->runtime)
                    : forge::db::object::store::open_existing(item.driver, settings, item.options.object->runtime));
            item.objects = std::make_shared<forge::db::object::store>(std::move(objects));
         }
         if (item.options.revision) {
            if (!item.objects) {
               FORGE_THROW_EXCEPTION(exceptions::initialize_failed, "db store revision layer requires object layer",
                                     forge::exceptions::ctx("store", item.name));
            }
            auto revisions = co_await (item.initialize_layers
                                           ? forge::db::revision::store::open(item.driver, *item.objects)
                                           : forge::db::revision::store::open_existing(item.driver, *item.objects));
            item.revisions = std::make_shared<forge::db::revision::store>(std::move(revisions));
         }
         if (item.options.blob) {
            item.blobs =
                std::make_shared<forge::db::blob::store>(item.driver, forge::db::blob::store::config{
                                                                          .data_family = item.options.blob->data_family,
                                                                          .refs_family = item.options.blob->refs_family,
                                                                      });
         }
         if (guarded && checks[index]) {
            try {
               // The descriptor-owned callback borrows this exact Object owner.
               // No impl mutex is held across user code or its awaited work.
               co_await opening.prerequisites[*checks[index]].check(*item.objects);
            } catch (...) {
               prerequisite_failure = std::current_exception();
               throw;
            }
         }
      }

      if (!lifecycle.try_admit(generation, [&] {
             auto lock = std::scoped_lock{mutex};
             const auto state = current.load();
             if (state == phase::closed) {
                FORGE_THROW_EXCEPTION(exceptions::stopped, "db store plugin is stopping");
             }
             if (state != phase::opening) {
                FORGE_THROW_EXCEPTION(exceptions::initialize_failed, "db store plugin initialization state changed");
             }
             for (auto index = std::size_t{}; index < pending.size(); ++index) {
                auto& item = pending[index];
                failed = &item;
                auto record = guarded ? bindings[index] : std::shared_ptr<managed_store>{};
                if (!guarded) {
                   const auto found = stores.find(item.name);
                   if (found == stores.end()) {
                      FORGE_THROW_EXCEPTION(exceptions::initialize_failed, "db store is no longer registered",
                                            forge::exceptions::ctx("store", item.name));
                   }
                   record = found->second;
                }
                record->owns_driver = item.owns_driver;
                record->driver = std::move(item.driver);
                record->objects = std::move(item.objects);
                record->blobs = std::move(item.blobs);
                record->revisions = std::move(item.revisions);
                record->opened = true;
                record->started = false;
             }
             current.store(phase::ready);
          })) {
         FORGE_THROW_EXCEPTION(exceptions::stopped, "db store opening was stopped");
      }
   } catch (...) {
      const auto stopping = lifecycle.snapshot().stop_requested;
      auto lock = std::unique_lock{mutex};
      if (guarded) {
         opening_failure = std::current_exception();
         // Both vectors were fully reserved before opening. Only noexcept
         // shared-owner moves remain; no map insert/lookup/allocation here.
         // Keep failed owners in the SAME records for existing App rollback.
         for (auto index = std::size_t{}; index < pending.size(); ++index) {
            auto& item = pending[index];
            auto& record = bindings[index];
            record->owns_driver = item.owns_driver;
            record->driver = std::move(item.driver);
            record->objects = std::move(item.objects);
            record->blobs = std::move(item.blobs);
            record->revisions = std::move(item.revisions);
            record->opened = false;
            record->started = false;
         }
      }
      if (current.load() == phase::opening) {
         current.store(restore_phase);
      }
      lock.unlock();
      const auto failed_store = failed ? std::string_view{failed->name} : std::string_view{};
      const auto failed_backend =
          failed ? (failed->config ? std::string_view{failed->config->driver} : std::string_view{"custom"})
                 : std::string_view{};
      auto canceled = false;
      try {
         throw;
      } catch (const forge::asio::exceptions::canceled&) {
         canceled = true;
      } catch (...) {
      }
      if (canceled) {
         dlog("forge.db.store", "store composition canceled",
              ("event", "composition.canceled")("store", failed_store)("backend", failed_backend));
      } else if (!stopping) {
         elog("forge.db.store", "store composition failed",
              ("event", "composition.failed")("store", failed_store)("backend", failed_backend));
      }
      throw;
   }
   const auto owned_count =
       static_cast<std::size_t>(std::ranges::count_if(pending, [](const auto& item) { return item.owns_driver; }));
   dlog("forge.db.store", "store composition ready",
        ("event", "composition.ready")("stores", pending.size())("owned_drivers",
                                                                 static_cast<std::uint64_t>(owned_count)));
   co_return;
}

} // namespace forge::plugins::db::store
