module;

#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <vector>

export module forge.app.diagnostics;

import forge.app.events;
import forge.app.plugin_lifecycle;

export namespace forge::app {

struct plugin_diagnostics {
   std::string id;
   std::string version;
   lifecycle_snapshot lifecycle;
};

struct application_diagnostics_snapshot {
   lifecycle_snapshot lifecycle;
   std::vector<plugin_diagnostics> plugins;
   event_journal_metrics events;
   std::vector<event_record> critical_events;
   std::vector<event_record> recent_events;
};

class diagnostics_store {
 public:
   diagnostics_store();
   ~diagnostics_store();

   diagnostics_store(const diagnostics_store&) = delete;
   diagnostics_store& operator=(const diagnostics_store&) = delete;

   void bind_application(lifecycle_observer lifecycle);
   void bind_plugin(std::string id, std::string version, lifecycle_observer lifecycle);
   void clear_plugins();
   [[nodiscard]] application_diagnostics_snapshot snapshot(const event_journal& events) const;

 private:
   struct impl;
   std::shared_ptr<impl> impl_;
};

} // namespace forge::app
