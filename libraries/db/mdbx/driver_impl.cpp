module;

#include <forge/exceptions/macros.hpp>
#include <forge/log/macros.hpp>
#include <mdbx.h>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

#include <atomic>
#include <functional>
#include <future>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

module forge.db.mdbx.driver;

import :error;
import forge.db.mdbx.diagnostics;
import forge.log.logger;
import forge.asio.affine;
import forge.asio.gate;
import forge.db.core.driver;
import forge.db.core.exceptions;
import forge.db.core.record;

#include "details/driver_impl.hxx"
#include "details/environment.hxx"
#include "details/snapshot_session.hxx"
#include "details/transaction_session.hxx"

namespace forge::db::mdbx::detail {

driver_impl::driver_impl(forge::asio::affine::executor executor,
                         std::shared_ptr<environment> environment,
                         std::thread::id affine_thread,
                         std::shared_ptr<forge::asio::affine::lane> lane_owner)
    : lane_owner_{std::move(lane_owner)},
      executor_{std::move(executor)},
      environment_{std::move(environment)},
      writer_gate_{std::make_shared<forge::asio::gate>()},
      affine_thread_{affine_thread} {}

driver_impl::~driver_impl() {
   close_sync();
}

const forge::asio::affine::executor& driver_impl::executor() const noexcept {
   return executor_;
}

const std::shared_ptr<environment>& driver_impl::environment_handle() const noexcept {
   return environment_;
}

boost::asio::awaitable<std::unique_ptr<forge::db::core::session>>
driver_impl::open_transaction() {
   auto ticket = co_await writer_gate_->acquire();
   auto* transaction = co_await executor_.execute(
      {.name = "mdbx-begin-write"},
      [environment = environment_]() {
         auto* transaction = static_cast<MDBX_txn*>(nullptr);
         require_mdbx_success(
            mdbx_txn_begin(environment->native(), nullptr,
                           static_cast<MDBX_txn_flags_t>(MDBX_TXN_TRY),
                           &transaction),
            "mdbx_txn_begin write");
         return transaction;
      });
   co_return std::make_unique<transaction_session>(
      shared_from_this(), std::move(ticket), transaction);
}

std::unique_ptr<forge::db::core::session> driver_impl::open_snapshot() {
   auto* anchor = static_cast<MDBX_txn*>(nullptr);
   require_mdbx_success(
      mdbx_txn_begin(environment_->native(), nullptr, MDBX_TXN_RDONLY, &anchor),
      "mdbx_txn_begin snapshot");
   return std::make_unique<snapshot_session>(shared_from_this(), anchor);
}

boost::asio::awaitable<void> driver_impl::create_checkpoint(std::string destination) {
   require_open();
   auto ticket = co_await writer_gate_->acquire();
   require_open();
   co_await executor_.execute(
      {.name = "mdbx-create-checkpoint"},
      [environment = environment_, destination = std::move(destination)] {
         const auto path = std::filesystem::path{destination};
         const auto parent = path.parent_path();
         auto error = std::error_code{};
         if (!parent.empty() && !std::filesystem::create_directories(parent, error) && error) {
            FORGE_THROW_EXCEPTION(exceptions::open_failed, "cannot create MDBX checkpoint parent directory",
                                  forge::exceptions::ctx("path", parent.string()),
                                  forge::exceptions::ctx("error", error.message()));
         }
         if (!std::filesystem::create_directory(path, error)) {
            FORGE_THROW_EXCEPTION(exceptions::open_failed, "MDBX checkpoint destination already exists or is invalid",
                                  forge::exceptions::ctx("path", path.string()),
                                  forge::exceptions::ctx("error", error.message()));
         }
         try {
            const auto data = path / "mdbx.dat";
            require_mdbx_success(
               mdbx_env_copy(environment->native(), data.c_str(), MDBX_CP_COMPACT),
               "mdbx_env_copy checkpoint");
         } catch (...) {
            std::filesystem::remove_all(path, error);
            throw;
         }
      });
   ticket.release();
   diagnostics::drain();
   ilog("forge.db.mdbx", "checkpoint created", ("event", "checkpoint.created"));
}

boost::asio::awaitable<void> driver_impl::flush(bool sync) {
   require_open();
   auto ticket = co_await writer_gate_->acquire();
   require_open();
   co_await executor_.execute(
      {.name = sync ? "mdbx-flush-sync" : "mdbx-flush-poll"},
      [environment = environment_, sync] { environment->flush(sync); });
   ticket.release();
   diagnostics::drain();
   dlog("forge.db.mdbx", "environment flush request completed",
        ("event", "environment.flush_request_completed")("sync", sync));
}

boost::asio::awaitable<void> driver_impl::close() {
   if (closed_.load(std::memory_order_acquire)) {
      co_return;
   }
   auto ticket = co_await writer_gate_->acquire();
   if (closed_.load(std::memory_order_acquire)) {
      co_return;
   }
   co_await executor_.execute(
      {.name = "mdbx-close"},
      [environment = environment_] { environment->close(); });
   closed_.store(true, std::memory_order_release);
   ticket.release();
   diagnostics::drain();
   ilog("forge.db.mdbx", "environment closed", ("event", "environment.closed"));
}

boost::asio::awaitable<void> driver_impl::shutdown_managed_lane() {
   if (lane_owner_) {
      co_await lane_owner_->shutdown();
   }
}

void driver_impl::abort_sync(std::vector<MDBX_txn*> transactions) noexcept {
   run_sync(
      "mdbx-dropped-rollback",
      [transactions = std::move(transactions)]() mutable {
         for (auto iterator = transactions.rbegin();
              iterator != transactions.rend(); ++iterator) {
            if (*iterator != nullptr) {
               static_cast<void>(mdbx_txn_abort(*iterator));
               *iterator = nullptr;
            }
         }
      });
}

void driver_impl::require_open() const {
   if (closed_.load(std::memory_order_acquire)) {
      FORGE_THROW_EXCEPTION(forge::db::core::exceptions::driver_closed,
                            "MDBX driver is closed");
   }
}

void driver_impl::close_sync() noexcept {
   if (environment_ == nullptr || !environment_->open()) {
      return;
   }
   run_sync("mdbx-destructor-close", [environment = environment_] {
      try {
         environment->close();
      } catch (...) {
         environment->close_noexcept();
      }
   });
}

void driver_impl::run_sync(std::string name,
                           std::function<void()> operation) noexcept {
   auto owned = std::make_shared<std::function<void()>>(std::move(operation));
   if (std::this_thread::get_id() == affine_thread_) {
      try {
         (*owned)();
      } catch (...) {
      }
      return;
   }

   try {
      auto context = boost::asio::io_context{};
      auto future = boost::asio::co_spawn(
         context,
         executor_.execute({.name = std::move(name)},
                           [owned] { (*owned)(); }),
         boost::asio::use_future);
      context.run();
      future.get();
   } catch (...) {
      try {
         (*owned)();
      } catch (...) {
      }
   }
}

} // namespace forge::db::mdbx::detail
