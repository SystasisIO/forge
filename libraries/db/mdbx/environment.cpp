module;

#include <forge/exceptions/macros.hpp>
#include <mdbx.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

module forge.db.mdbx.driver;

import :error;
import forge.db.mdbx.diagnostics;
import forge.db.core.record;
import forge.db.core.exceptions;
import forge.db.mdbx.exceptions;

#include "details/environment.hxx"

namespace forge::db::mdbx::detail {
namespace {

intptr_t geometry_value(const std::optional<std::uint64_t>& value,
                        std::string_view field) {
   if (!value.has_value()) {
      return -1;
   }
   if (*value > static_cast<std::uint64_t>(std::numeric_limits<intptr_t>::max())) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "MDBX geometry value is too large",
                            forge::exceptions::ctx("field", field),
                            forge::exceptions::ctx("value", *value));
   }
   return static_cast<intptr_t>(*value);
}

intptr_t page_size_value(const std::optional<std::uint32_t>& value) {
   if (!value.has_value()) {
      return -1;
   }
   return static_cast<intptr_t>(*value);
}

MDBX_env_flags_t environment_flags(durability mode) {
   auto flags = static_cast<unsigned>(MDBX_NOSTICKYTHREADS) |
                static_cast<unsigned>(MDBX_EXCLUSIVE);
   if (mode == durability::safe_nosync) {
      flags |= static_cast<unsigned>(MDBX_SAFE_NOSYNC);
   }
   return static_cast<MDBX_env_flags_t>(flags);
}

MDBX_db_flags_t database_flags(bool create) {
   return create ? MDBX_CREATE : MDBX_DB_DEFAULTS;
}

} // namespace

environment::environment(MDBX_env* native,
                         std::map<std::string, MDBX_dbi> families,
                         std::size_t max_key_size,
                         std::size_t max_value_size)
    : native_{native}, families_{std::move(families)},
      max_key_size_{max_key_size}, max_value_size_{max_value_size} {}

std::shared_ptr<environment> environment::open(const config& value) {
   diagnostics::claim_process();
   const auto path = std::filesystem::path{value.path};
   auto error = std::error_code{};
   const auto exists = std::filesystem::exists(path, error);
   if (error) {
      FORGE_THROW_EXCEPTION(exceptions::open_failed, "cannot inspect MDBX path",
                            forge::exceptions::ctx("path", value.path),
                            forge::exceptions::ctx("error", error.message()));
   }
   const auto data_exists = exists && std::filesystem::exists(path / "mdbx.dat", error);
   if (error) {
      FORGE_THROW_EXCEPTION(exceptions::open_failed, "cannot inspect MDBX data file",
                            forge::exceptions::ctx("path", value.path),
                            forge::exceptions::ctx("error", error.message()));
   }
   if (!data_exists && !value.create_if_missing) {
      FORGE_THROW_EXCEPTION(exceptions::open_failed, "MDBX path does not exist",
                            forge::exceptions::ctx("path", value.path));
   }
   if (!exists) {
      std::filesystem::create_directories(path, error);
      if (error) {
         FORGE_THROW_EXCEPTION(exceptions::open_failed, "cannot create MDBX path",
                               forge::exceptions::ctx("path", value.path),
                               forge::exceptions::ctx("error", error.message()));
      }
   }

   auto* native = static_cast<MDBX_env*>(nullptr);
   require_mdbx_success(mdbx_env_create(&native), "mdbx_env_create");

   try {
      require_mdbx_success(
         mdbx_env_set_maxdbs(native, static_cast<MDBX_dbi>(value.families.size())),
         "mdbx_env_set_maxdbs");
      require_mdbx_success(
         mdbx_env_set_maxreaders(native, static_cast<unsigned>(value.max_readers)),
         "mdbx_env_set_maxreaders");
      require_mdbx_success(
         mdbx_env_set_geometry(
            native,
            geometry_value(value.map.lower_size, "lower_size"),
            geometry_value(value.map.current_size, "current_size"),
            geometry_value(value.map.upper_size, "upper_size"),
            geometry_value(value.map.growth_step, "growth_step"),
            geometry_value(value.map.shrink_threshold, "shrink_threshold"),
            page_size_value(value.map.page_size)),
         "mdbx_env_set_geometry");
      require_mdbx_success(
         mdbx_env_open(native, value.path.c_str(), environment_flags(value.durability_mode), 0640),
         "mdbx_env_open");

      auto* initialization = static_cast<MDBX_txn*>(nullptr);
      require_mdbx_success(
         mdbx_txn_begin(native, nullptr, MDBX_TXN_READWRITE, &initialization),
         "mdbx_txn_begin initialization");

      auto families = std::map<std::string, MDBX_dbi>{};
      try {
         for (const auto& family : value.families) {
            auto dbi = MDBX_dbi{};
            require_mdbx_success(
               mdbx_dbi_open(initialization, family.c_str(),
                             database_flags(value.create_missing_families), &dbi),
               "mdbx_dbi_open");
            families.emplace(family, dbi);
         }
         require_mdbx_success(mdbx_txn_commit(initialization),
                              "mdbx_txn_commit initialization");
         initialization = nullptr;
      } catch (...) {
         if (initialization != nullptr) {
            static_cast<void>(mdbx_txn_abort(initialization));
         }
         throw;
      }

      const auto max_key_size = mdbx_env_get_maxkeysize_ex(native, MDBX_DB_DEFAULTS);
      const auto max_value_size = mdbx_env_get_maxvalsize_ex(native, MDBX_DB_DEFAULTS);
      if (max_key_size < 0 || max_value_size < 0) {
         FORGE_THROW_EXCEPTION(exceptions::open_failed,
                               "MDBX record limits are unavailable");
      }
      auto result = std::shared_ptr<environment>{new environment{
         native, std::move(families), static_cast<std::size_t>(max_key_size),
         static_cast<std::size_t>(max_value_size)}};
      return result;
   } catch (...) {
      static_cast<void>(mdbx_env_close_ex(native, true));
      throw;
   }
}

environment::~environment() {
   close_noexcept();
}

MDBX_env* environment::native() const noexcept {
   return native_;
}

MDBX_dbi environment::resolve(const forge::db::core::family& family) const {
   const auto found = families_.find(family.name);
   if (found == families_.end()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_family, "MDBX family is not configured",
                            forge::exceptions::ctx("family", family.name));
   }
   return found->second;
}

bool environment::open() const noexcept {
   return native_ != nullptr;
}

void environment::validate_key(const forge::db::core::record_key& key) const {
   if (key.bytes().size() > max_key_size_) {
      FORGE_THROW_EXCEPTION(exceptions::key_too_large, "MDBX key is too large",
                            forge::exceptions::ctx("size", key.bytes().size()),
                            forge::exceptions::ctx("max", max_key_size_));
   }
}

void environment::validate_value(const std::vector<std::byte>& value) const {
   if (value.size() > max_value_size_) {
      FORGE_THROW_EXCEPTION(exceptions::value_too_large, "MDBX value is too large",
                            forge::exceptions::ctx("size", value.size()),
                            forge::exceptions::ctx("max", max_value_size_));
   }
}

void environment::validate_range(
   const forge::db::core::record_range& range,
   const forge::db::core::page_request& request) const {
   validate_key(range.begin);
   validate_key(range.end);
   validate_key(range.prefix);
   if (request.after.has_value()) {
      validate_key(request.after->boundary);
   }
}

void environment::flush(bool sync) {
   if (native_ == nullptr) {
      FORGE_THROW_EXCEPTION(forge::db::core::exceptions::driver_closed,
                            "MDBX environment is closed");
   }
   const auto code = mdbx_env_sync_ex(native_, sync, !sync);
   if (code != MDBX_BUSY) {
      require_mdbx_success(code, "mdbx_env_sync_ex");
   }
}

void environment::close() {
   if (native_ == nullptr) {
      return;
   }
   require_mdbx_success(mdbx_env_sync_ex(native_, true, false),
                        "mdbx_env_sync_ex close");
   auto* closing = std::exchange(native_, nullptr);
   const auto code = mdbx_env_close_ex(closing, false);
   if (!mdbx_success(code)) {
      if (code == MDBX_BUSY) {
         native_ = closing;
      } else {
         families_.clear();
      }
      require_mdbx_success(code, "mdbx_env_close_ex");
   }
   families_.clear();
}

void environment::close_noexcept() noexcept {
   if (native_ == nullptr) {
      return;
   }
   auto* closing = std::exchange(native_, nullptr);
   static_cast<void>(mdbx_env_close_ex(closing, true));
   families_.clear();
}

} // namespace forge::db::mdbx::detail
