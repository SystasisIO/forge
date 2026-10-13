#include <concepts>
#include <filesystem>
#include <type_traits>

import forge.app.application_environment;
import forge.app.plugin_lifecycle;
import forge.app.plugin;
import forge.app.diagnostics;
import forge.app.events;

template <typename T>
concept can_stop = requires(T value) { value.request_stop(); };

static_assert(!can_stop<forge::app::lifecycle_observer>);
static_assert(can_stop<forge::app::plugin_lifecycle>);
static_assert(!std::copy_constructible<forge::app::plugin_lifecycle::operation>);

int main() {
   auto environment = forge::app::application_environment{
      std::filesystem::temp_directory_path() / "forge-package-app", "test"};
   if (environment.resolve_data_path("snapshots") != environment.data_dir() / "snapshots") {
      return 1;
   }
   auto owner = forge::app::plugin_lifecycle{};
   auto diagnostics = forge::app::diagnostics_store{};
   auto events = forge::app::event_journal{};
   diagnostics.bind_application(owner.observer());
   diagnostics.bind_plugin("sample", "1.0", owner.observer());
   if (!owner.request_stop()) {
      return 2;
   }
   owner.complete_stop();
   const auto snapshot = diagnostics.snapshot(events);
   return snapshot.lifecycle.phase == forge::app::lifecycle_phase::stopping &&
          snapshot.plugins.size() == 1 &&
          snapshot.plugins.front().lifecycle.stop_requested ? 0 : 3;
}
