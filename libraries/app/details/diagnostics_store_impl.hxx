#pragma once

namespace forge::app {

struct diagnostics_store::impl {
   mutable std::mutex mutex;
   std::optional<lifecycle_observer> application;
   std::map<std::string, std::pair<std::string, lifecycle_observer>> plugins;
};

} // namespace forge::app
