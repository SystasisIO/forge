module;

#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

module forge.app.diagnostics;

#include "details/diagnostics_store_impl.hxx"

namespace forge::app {

diagnostics_store::diagnostics_store() : impl_{std::make_shared<impl>()} {}
diagnostics_store::~diagnostics_store() = default;

void diagnostics_store::bind_application(lifecycle_observer lifecycle) {
   auto replacement = std::optional{std::move(lifecycle)};
   {
      const auto lock = std::scoped_lock{impl_->mutex};
      impl_->application.swap(replacement);
   }
}

void diagnostics_store::bind_plugin(std::string id, std::string version, lifecycle_observer lifecycle) {
   auto replacement = std::make_pair(std::move(version), std::move(lifecycle));
   {
      const auto lock = std::scoped_lock{impl_->mutex};
      const auto found = impl_->plugins.find(id);
      if (found == impl_->plugins.end()) {
         impl_->plugins.emplace(std::move(id), std::move(replacement));
      } else {
         found->second.swap(replacement);
      }
   }
}

void diagnostics_store::clear_plugins() {
   auto retired = decltype(impl_->plugins){};
   {
      const auto lock = std::scoped_lock{impl_->mutex};
      impl_->plugins.swap(retired);
   }
}

application_diagnostics_snapshot diagnostics_store::snapshot(const event_journal& events) const {
   auto application = std::optional<lifecycle_observer>{};
   auto plugins = decltype(impl_->plugins){};
   {
      const auto lock = std::scoped_lock{impl_->mutex};
      application = impl_->application;
      plugins = impl_->plugins;
   }
   auto out = application_diagnostics_snapshot{};
   if (application) {
      out.lifecycle = application->snapshot();
   }
   for (const auto& [id, observed] : plugins) {
      out.plugins.push_back({id, observed.first, observed.second.snapshot()});
   }
   out.events = events.metrics();
   out.critical_events = events.critical_events();
   out.recent_events = events.recent_events();
   return out;
}

} // namespace forge::app
