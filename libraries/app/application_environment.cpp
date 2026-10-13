module;

#include <filesystem>
#include <forge/exceptions/macros.hpp>
#include <string>
#include <system_error>
#include <utility>

module forge.app.application_environment;

import forge.app.exceptions;

namespace forge::app {

application_environment::application_environment(std::filesystem::path data_dir, std::string profile)
    : profile_{std::move(profile)} {
   if (data_dir.empty()) {
      return;
   }
   if (data_dir.is_relative()) {
      auto error = std::error_code{};
      data_dir = std::filesystem::absolute(data_dir, error);
      if (error) {
         FORGE_THROW_EXCEPTION(exceptions::config_failed, "cannot resolve the application data directory",
                               forge::exceptions::ctx("error", error.message()));
      }
   }
   // Lexical normalization freezes the base without requiring it to exist.
   data_dir_ = data_dir.lexically_normal();
}

const std::filesystem::path& application_environment::data_dir() const noexcept {
   return data_dir_;
}

const std::string& application_environment::profile() const noexcept {
   return profile_;
}

std::filesystem::path application_environment::resolve_data_path(const std::filesystem::path& path) const {
   if (path.is_absolute()) {
      return path;
   }
   if (data_dir_.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::config_failed, "relative data paths require an application data directory");
   }
   return (data_dir_ / path).lexically_normal();
}

} // namespace forge::app
