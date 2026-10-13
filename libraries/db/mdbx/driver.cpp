module;

#include <forge/exceptions/macros.hpp>
#include <forge/log/macros.hpp>
#include <mdbx.h>

#include <boost/asio/awaitable.hpp>

#include <atomic>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

module forge.db.mdbx.driver;

import forge.asio.affine;
import forge.asio.gate;
import forge.db.core.driver;
import forge.db.core.record;
import forge.db.mdbx.exceptions;
import forge.db.mdbx.diagnostics;
import forge.log.logger;

#include "details/driver_impl.hxx"
#include "details/environment.hxx"

namespace forge::db::mdbx {
namespace {

void validate_config(const config& value) {
   if (value.path.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "MDBX path must not be empty");
   }
   if (value.families.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "MDBX requires at least one family");
   }
   if (value.families.size() > MDBX_MAX_DBI) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "MDBX family count is too large",
                            forge::exceptions::ctx("families", value.families.size()),
                            forge::exceptions::ctx("max", MDBX_MAX_DBI));
   }
   if (value.max_readers == 0 ||
       value.max_readers > static_cast<std::size_t>(std::numeric_limits<unsigned>::max())) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "MDBX max_readers is out of range",
                            forge::exceptions::ctx("max-readers", value.max_readers));
   }

   auto names = std::set<std::string>{};
   for (const auto& family : value.families) {
      if (family.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "MDBX family name must not be empty");
      }
      if (!names.insert(family).second) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "MDBX family names must be unique",
                               forge::exceptions::ctx("family", family));
      }
   }

   const auto ordered = [](const auto& lower, const auto& upper) {
      return !lower.has_value() || !upper.has_value() || *lower <= *upper;
   };
   if (!ordered(value.map.lower_size, value.map.current_size) ||
       !ordered(value.map.current_size, value.map.upper_size) ||
       !ordered(value.map.lower_size, value.map.upper_size)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                            "MDBX geometry must satisfy lower <= current <= upper");
   }

   if (value.map.page_size.has_value()) {
      const auto page_size = *value.map.page_size;
      if (page_size < static_cast<std::uint32_t>(mdbx_limits_pgsize_min()) ||
          page_size > static_cast<std::uint32_t>(mdbx_limits_pgsize_max()) ||
          (page_size & (page_size - 1U)) != 0U) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                               "MDBX page size is unsupported",
                               forge::exceptions::ctx("page-size", page_size));
      }

      const auto aligned = [page_size](const auto& size) {
         return !size.has_value() || *size == 0 || (*size % page_size) == 0;
      };
      if (!aligned(value.map.lower_size) || !aligned(value.map.current_size) ||
          !aligned(value.map.upper_size) || !aligned(value.map.growth_step) ||
          !aligned(value.map.shrink_threshold)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                               "MDBX geometry must align to configured page size",
                               forge::exceptions::ctx("page-size", page_size));
      }
   }
}

} // namespace

driver::driver(std::shared_ptr<detail::driver_impl> impl)
    : impl_{std::move(impl)} {}

driver::~driver() = default;

boost::asio::awaitable<std::shared_ptr<driver>>
driver::open(config value, forge::asio::affine::executor executor) {
   validate_config(value);
   co_return co_await open_managed(std::move(value), std::move(executor), {});
}

boost::asio::awaitable<std::shared_ptr<driver>>
driver::open(config value, forge::asio::affine::lane::options lane_options) {
   validate_config(value);
   auto lane = std::make_shared<forge::asio::affine::lane>(
      std::move(lane_options));
   auto executor = lane->get_executor();
   co_return co_await open_managed(std::move(value), std::move(executor),
                                  std::move(lane));
}

boost::asio::awaitable<std::shared_ptr<driver>>
driver::open_managed(config value,
                     forge::asio::affine::executor executor,
                     std::shared_ptr<forge::asio::affine::lane> lane) {
   if (!executor.valid()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                            "MDBX requires a valid affine executor");
   }

   const auto families = value.families.size();
   const auto durable_sync = value.durability_mode == durability::durable_sync;

   auto opened = co_await executor.execute(
      {.name = "mdbx-open"},
      [value = std::move(value)]() mutable {
         return std::pair{detail::environment::open(value),
                          std::this_thread::get_id()};
      });
   auto impl = std::make_shared<detail::driver_impl>(
      std::move(executor), std::move(opened.first), opened.second,
      std::move(lane));
   diagnostics::drain();
   ilog("forge.db.mdbx", "environment opened",
        ("event", "environment.opened")("families", families)("durable_sync", durable_sync));
   co_return std::shared_ptr<driver>{new driver{std::move(impl)}};
}

boost::asio::awaitable<void> driver::async_flush(bool sync) {
   auto admission = admit_operation();
   co_await impl_->flush(sync);
}

boost::asio::awaitable<std::unique_ptr<forge::db::core::session>>
driver::open_transaction() {
   co_return co_await impl_->open_transaction();
}

boost::asio::awaitable<std::unique_ptr<forge::db::core::session>>
driver::open_snapshot() {
   co_return impl_->open_snapshot();
}

boost::asio::awaitable<void> driver::create_checkpoint_impl(std::filesystem::path destination) {
   co_await impl_->create_checkpoint(destination.string());
}

boost::asio::awaitable<void> driver::close_driver() {
   co_await impl_->close();
   co_await impl_->shutdown_managed_lane();
}

} // namespace forge::db::mdbx
