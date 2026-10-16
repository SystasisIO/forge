module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/this_coro.hpp>

#include <memory>
#include <mutex>
#include <filesystem>
#include <utility>

module forge.db.core.driver;

import forge.db.core.exceptions;

#include "details/driver_state.hxx"
#include "details/tracked_session.hxx"

namespace forge::db::core {

driver::driver() : state_{std::make_shared<detail::driver_state>()} {}
driver::~driver() = default;

driver::operation_admission::operation_admission(
   std::shared_ptr<detail::driver_state> state) noexcept
    : state_{std::move(state)} {}

driver::operation_admission::~operation_admission() {
   release();
}

driver::operation_admission::operation_admission(operation_admission&& other) noexcept
    : state_{std::move(other.state_)} {}

driver::operation_admission&
driver::operation_admission::operator=(operation_admission&& other) noexcept {
   if (this != &other) {
      release();
      state_ = std::move(other.state_);
   }
   return *this;
}

void driver::operation_admission::release() noexcept {
   if (state_) {
      state_->release_operation();
      state_.reset();
   }
}

boost::asio::awaitable<std::optional<std::vector<std::byte>>>
session::get_for_update(family, record_key) {
   FORGE_THROW_EXCEPTION(exceptions::unsupported_operation, "db session does not support record locks");
}

boost::asio::awaitable<void> session::create_savepoint() {
   FORGE_THROW_EXCEPTION(exceptions::unsupported_operation, "db session does not support savepoints");
}

boost::asio::awaitable<void> session::rollback_to_savepoint() {
   FORGE_THROW_EXCEPTION(exceptions::unsupported_operation, "db session does not support savepoints");
}

boost::asio::awaitable<void> session::release_savepoint() {
   FORGE_THROW_EXCEPTION(exceptions::unsupported_operation, "db session does not support savepoints");
}

boost::asio::awaitable<transaction> driver::begin_transaction() {
   auto admission = state_->admit_open();
   const auto executor = co_await boost::asio::this_coro::executor;
   auto active = co_await open_transaction();
   if (!active) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "db transaction session is null");
   }
   co_return transaction{
      std::make_unique<detail::tracked_session>(std::move(active), std::move(admission)),
      executor, snapshot_origin_};
}

boost::asio::awaitable<snapshot> driver::begin_read() {
   auto admission = state_->admit_open();
   auto active = co_await open_snapshot();
   if (!active) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "db snapshot session is null");
   }
   co_return snapshot{
      std::make_unique<detail::tracked_session>(std::move(active), std::move(admission)),
      snapshot_origin_};
}

boost::asio::awaitable<void> driver::create_checkpoint(std::filesystem::path destination) {
   if (destination.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "db checkpoint destination must not be empty");
   }
   auto admission = admit_operation();
   co_await create_checkpoint_impl(std::move(destination));
}

boost::asio::awaitable<void> driver::create_checkpoint_impl(std::filesystem::path) {
   FORGE_THROW_EXCEPTION(exceptions::unsupported_operation, "db driver does not support durable checkpoints");
}

boost::asio::awaitable<void> driver::async_close() {
   const auto action = state_->admit_close();
   if (action == detail::driver_state::close_action::already_closed) {
      co_return;
   }
   try {
      co_await close_driver();
   } catch (...) {
      state_->fail_close();
      throw;
   }
   state_->finish_close();
}

driver::operation_admission driver::admit_operation() const {
   state_->admit_operation();
   return operation_admission{state_};
}

boost::asio::awaitable<void> driver::close_driver() {
   co_return;
}

} // namespace forge::db::core
