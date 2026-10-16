module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.db.store.api;

import forge.db.authenticated.store;
import forge.db.authenticated.transaction;
import forge.db.authenticated.types;
import forge.db.blob.ref;
import forge.db.blob.snapshot;
import forge.db.blob.store;
import forge.db.blob.transaction;
import forge.db.blob.types;
import forge.db.blob.exceptions;
import forge.db.core.driver;
import forge.db.object.exceptions;
import forge.db.object.hooks;
import forge.db.object.snapshot;
import forge.db.object.store;
import forge.db.object.transaction;
import forge.db.revision.store;
import forge.db.revision.transaction;
import forge.db.revision.types;
import forge.plugins.db.store.exceptions;

namespace forge::plugins::db::store {

closed_store::closed_store(std::shared_ptr<store_handle_state> state) noexcept : state_{std::move(state)} {}
closed_store::closed_store(closed_store&&) noexcept = default;
closed_store& closed_store::operator=(closed_store&& other) noexcept {
   if (this != &other) {
      if (state_ && state_->activation_inflight())
         std::terminate();
      state_ = std::move(other.state_);
   }
   return *this;
}
closed_store::~closed_store() {
   if (state_ && state_->activation_inflight())
      std::terminate(); // Never destroy a borrowed runtime during accepted IO.
}
activation_status closed_store::state() const {
   if (!state_)
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument, "closed store capability is empty");
   return state_->closed_state();
}
boost::asio::awaitable<void> closed_store::activate(std::filesystem::path destination) const {
   return activate_owned(state_, std::move(destination));
}
boost::asio::awaitable<void> closed_store::confirm_durability() const {
   return confirm_owned(state_);
}
boost::asio::awaitable<void> closed_store::activate_owned(std::shared_ptr<store_handle_state> owner,
                                                          std::filesystem::path destination) {
   if (!owner)
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument, "closed store capability is empty");
   co_await owner->activate_closed(std::move(destination));
}
boost::asio::awaitable<void> closed_store::confirm_owned(std::shared_ptr<store_handle_state> owner) {
   if (!owner)
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument, "closed store capability is empty");
   co_await owner->confirm_activation();
}
boost::asio::awaitable<closed_store> store_handle::close_for_activation() const {
   return close_owned(state_);
}
boost::asio::awaitable<closed_store> store_handle::close_owned(std::shared_ptr<store_handle_state> owner) {
   if (!owner)
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument, "store handle is empty");
   co_await owner->close_for_activation();
   owner->claim_activation();
   co_return closed_store{std::move(owner)};
}
boost::asio::awaitable<void> store_handle_state::close_for_activation() const {
   FORGE_THROW_EXCEPTION(exceptions::unsupported_activation, "store has no strict activation owner");
   co_return;
}
void store_handle_state::claim_activation() const {
   FORGE_THROW_EXCEPTION(exceptions::unsupported_activation, "store has no strict activation owner");
}
boost::asio::awaitable<void> store_handle_state::activate_closed(std::filesystem::path) const {
   FORGE_THROW_EXCEPTION(exceptions::unsupported_activation, "store has no strict activation owner");
   co_return;
}
boost::asio::awaitable<void> store_handle_state::confirm_activation() const {
   FORGE_THROW_EXCEPTION(exceptions::unsupported_activation, "store has no strict activation owner");
   co_return;
}
activation_status store_handle_state::closed_state() const {
   FORGE_THROW_EXCEPTION(exceptions::unsupported_activation, "store has no strict activation owner");
}
bool store_handle_state::activation_inflight() const noexcept {
   return false;
}

snapshot::snapshot(forge::db::core::snapshot active, std::string store_name,
                   std::optional<forge::db::object::snapshot> objects, std::optional<forge::db::blob::snapshot> blobs)
    : active_{std::move(active)}, store_name_{std::move(store_name)}, objects_{std::move(objects)},
      blobs_{std::move(blobs)} {}

bool snapshot::active() const noexcept {
   return active_.active();
}

std::string snapshot::name() const {
   return store_name_;
}

forge::db::object::snapshot snapshot::objects() const {
   if (!objects_.has_value()) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable_layer, "db store object layer is not configured",
                            forge::exceptions::ctx("store", store_name_));
   }
   return *objects_;
}

forge::db::blob::snapshot snapshot::blobs() const {
   if (!blobs_.has_value()) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable_layer, "db store blob layer is not configured",
                            forge::exceptions::ctx("store", store_name_));
   }
   return *blobs_;
}

std::shared_ptr<forge::db::revision::store> store_handle_state::require_revisions() const {
   return {};
}

boost::asio::awaitable<snapshot> store_handle_state::begin_read() const {
   auto driver = require_driver();
   auto objects = std::shared_ptr<forge::db::object::store>{};
   auto blobs = std::shared_ptr<forge::db::blob::store>{};

   try {
      objects = require_objects();
   } catch (const exceptions::unavailable_layer&) {
   }
   try {
      blobs = require_blobs();
   } catch (const exceptions::unavailable_layer&) {
   }

   auto active = co_await driver->begin_read();
   auto object_view = std::optional<forge::db::object::snapshot>{};
   auto blob_view = std::optional<forge::db::blob::snapshot>{};
   if (objects) {
      object_view.emplace(objects->join(active));
   }
   if (blobs) {
      blob_view.emplace(blobs->join(active));
   }

   co_return snapshot{std::move(active), name(), std::move(object_view), std::move(blob_view)};
}

transaction::transaction(forge::db::core::transaction active, std::string store_name)
    : core_{std::make_unique<forge::db::core::transaction>(std::move(active))}, store_name_{std::move(store_name)} {}

transaction::transaction(forge::db::object::transaction active, std::string store_name)
    : object_{std::move(active)}, store_name_{std::move(store_name)} {}

void transaction::require_named_store(const std::string& expected) const {
   if (store_name_.empty() || store_name_ != expected) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument, "db store transaction belongs to another named store");
   }
}

bool transaction::active() const noexcept {
   return object_.has_value() || (core_ && core_->active());
}

forge::db::core::transaction& transaction::db_transaction() {
   if (object_.has_value()) {
      return object_->db_transaction();
   }
   if (core_) {
      return *core_;
   }
   FORGE_THROW_EXCEPTION(exceptions::stopped, "db store transaction is closed");
}

boost::asio::awaitable<void> transaction::commit() {
   if (object_.has_value()) {
      co_await object_->commit();
      blob_.reset();
      object_.reset();
      co_return;
   }
   if (core_) {
      co_await core_->commit();
      blob_.reset();
      core_.reset();
   }
}

boost::asio::awaitable<void> transaction::rollback() {
   if (object_.has_value()) {
      co_await object_->rollback();
      blob_.reset();
      object_.reset();
      co_return;
   }
   if (core_) {
      co_await core_->rollback();
      blob_.reset();
      core_.reset();
   }
}

std::string object_handle::name() const {
   if (!state_) {
      return {};
   }
   return state_->name();
}

std::shared_ptr<forge::db::object::store> object_handle::require_setup_store() const {
   if (!state_) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "db store object handle is empty");
   }
   return state_->require_objects();
}

std::shared_ptr<forge::db::object::store> object_handle::require_store() const {
   if (!state_) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "db store object handle is empty");
   }
   (void)state_->require_driver();
   return state_->require_objects();
}

forge::db::object::store::options object_handle::runtime_options() const {
   return require_store()->runtime_options();
}

void object_handle::add_interceptor(std::shared_ptr<forge::db::object::interceptor> value) const {
   require_setup_store()->add_interceptor(std::move(value));
}

void object_handle::add_observer(std::shared_ptr<forge::db::object::observer> value) const {
   require_setup_store()->add_observer(std::move(value));
}

boost::asio::awaitable<forge::db::object::transaction> object_handle::begin_transaction() const {
   co_return co_await require_store()->begin_transaction();
}

boost::asio::awaitable<forge::db::object::snapshot> object_handle::begin_read() const {
   co_return co_await require_store()->begin_read();
}

boost::asio::awaitable<forge::db::object::transaction> object_handle::join(forge::db::core::transaction& active) const {
   co_return co_await require_store()->join(active);
}

boost::asio::awaitable<forge::db::object::transaction> object_handle::join(transaction& active) const {
   active.require_named_store(name());

   auto objects = require_store();
   if (active.object_.has_value()) {
      try {
         co_return co_await objects->join(*active.object_);
      } catch (const forge::db::object::exceptions::invalid_descriptor&) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_argument, "db store transaction belongs to another object store");
      }
   }
   co_return co_await objects->join(active.db_transaction());
}

boost::asio::awaitable<void> object_handle::require_pristine(transaction& active) const {
   auto owner = require_store();
   active.require_named_store(name());
   if (!active.object_) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument,
                            "pristine check requires the store's cached ObjectDB transaction");
   }
   auto objects = co_await join(active);
   if (objects.db_transaction().has_participant("forge.db.revision")) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument, "pristine check cannot use an attached Revision scope");
   }
   co_await owner->require_pristine(objects);
}

boost::asio::awaitable<void>
object_handle::restore_allocations(transaction& active,
                                   std::vector<forge::db::object::store::allocation> values) const {
   auto owner = require_store();
   active.require_named_store(name());
   if (!active.object_) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument,
                            "allocation restore requires the store's cached ObjectDB transaction");
   }
   auto objects = co_await join(active);
   if (objects.db_transaction().has_participant("forge.db.revision")) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument, "allocation restore cannot use an attached Revision scope");
   }
   co_await owner->restore_allocations(objects, std::move(values));
}

std::string blob_handle::name() const {
   if (!state_) {
      return {};
   }
   return state_->name();
}

std::shared_ptr<forge::db::blob::store> blob_handle::require_store() const {
   if (!state_) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "db store blob handle is empty");
   }
   return state_->require_blobs();
}

boost::asio::awaitable<forge::db::blob::transaction> blob_handle::begin_transaction() const {
   co_return co_await require_store()->begin_transaction();
}

boost::asio::awaitable<forge::db::blob::snapshot> blob_handle::begin_read() const {
   co_return co_await require_store()->begin_read();
}

forge::db::blob::transaction blob_handle::join(forge::db::core::transaction& active) const {
   return require_store()->join(active);
}

forge::db::blob::transaction blob_handle::join(transaction& active) const {
   active.require_named_store(name());
   auto blobs = require_store();
   if (active.blob_.has_value()) {
      try {
         return blobs->join(*active.blob_);
      } catch (const forge::db::blob::exceptions::invalid_descriptor&) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_argument, "db store transaction belongs to another blob store");
      }
   }

   active.blob_.emplace(blobs->join(active.db_transaction()));
   return blobs->join(*active.blob_);
}

boost::asio::awaitable<forge::db::blob::ref<forge::db::blob::digest>>
blob_handle::put(std::vector<std::byte> payload) const {
   co_return co_await require_store()->put(std::move(payload));
}

boost::asio::awaitable<forge::db::blob::collect_result>
blob_handle::collect_unreferenced(forge::db::blob::collect_options options) const {
   co_return co_await require_store()->collect_unreferenced(std::move(options));
}

std::string revision_handle::name() const {
   if (!state_) {
      return {};
   }
   return state_->name();
}

std::shared_ptr<forge::db::revision::store> revision_handle::require_store() const {
   if (!state_) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "db store revision handle is empty");
   }
   (void)state_->require_driver();
   auto result = state_->require_revisions();
   if (!result) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable_layer, "db store revision layer is not configured",
                            forge::exceptions::ctx("store", name()));
   }
   return result;
}

boost::asio::awaitable<forge::db::revision::scope> revision_handle::join(transaction& active) const {
   active.require_named_store(name());
   if (active.object_.has_value()) {
      co_return co_await require_store()->join(*active.object_);
   }
   co_return co_await require_store()->join(active.db_transaction());
}

boost::asio::awaitable<void> revision_handle::require_pristine(transaction& active) const {
   auto owner = require_store();
   active.require_named_store(name());
   if (!active.object_) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument,
                            "pristine check requires the store's cached ObjectDB transaction");
   }
   auto objects = co_await object_handle{state_}.join(active);
   if (objects.db_transaction().has_participant("forge.db.revision")) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument, "pristine check cannot use an attached Revision scope");
   }
   co_await owner->require_pristine(objects);
}

boost::asio::awaitable<void> revision_handle::revert(transaction& active,
                                                     forge::db::revision::revision_id_t expected_head) const {
   active.require_named_store(name());
   if (active.object_.has_value()) {
      co_await require_store()->revert(*active.object_, expected_head);
      co_return;
   }
   co_await require_store()->revert(active.db_transaction(), expected_head);
}

boost::asio::awaitable<forge::db::revision::prune_result>
revision_handle::prune_through(transaction& active, forge::db::revision::revision_id_t inclusive_boundary,
                               forge::db::revision::prune_options options) const {
   active.require_named_store(name());
   if (active.object_.has_value()) {
      co_return co_await require_store()->prune_through(*active.object_, inclusive_boundary, options);
   }
   co_return co_await require_store()->prune_through(active.db_transaction(), inclusive_boundary, options);
}

std::string authenticated_handle::name() const {
   if (!state_) {
      return {};
   }
   return state_->name();
}

const forge::db::authenticated::store& authenticated_handle::require_store() const {
   if (!state_) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "db store authenticated handle is empty");
   }
   (void)state_->require_driver();
   return store_;
}

boost::asio::awaitable<std::optional<forge::db::authenticated::root>> authenticated_handle::earliest() const {
   co_return co_await require_store().earliest();
}

boost::asio::awaitable<std::optional<forge::db::authenticated::root>> authenticated_handle::latest() const {
   co_return co_await require_store().latest();
}

boost::asio::awaitable<std::optional<forge::db::authenticated::root>>
authenticated_handle::find_root(forge::db::authenticated::version_id_t version) const {
   co_return co_await require_store().find_root(version);
}

boost::asio::awaitable<std::optional<forge::db::authenticated::bytes>>
authenticated_handle::get(forge::db::authenticated::version_id_t version, std::span<const std::byte> key) const {
   co_return co_await require_store().get(version, key);
}

boost::asio::awaitable<forge::db::authenticated::point_proof>
authenticated_handle::prove(forge::db::authenticated::version_id_t version, std::span<const std::byte> key,
                            bool include_value) const {
   co_return co_await require_store().prove(version, key, include_value);
}

boost::asio::awaitable<forge::db::authenticated::range_proof>
authenticated_handle::prove_range(forge::db::authenticated::version_id_t version,
                                  forge::db::authenticated::range_request request,
                                  forge::db::authenticated::proof_tree tree) const {
   co_return co_await require_store().prove_range(version, std::move(request), tree);
}

boost::asio::awaitable<forge::db::authenticated::verified_range>
authenticated_handle::scan_range(forge::db::authenticated::version_id_t version,
                                 forge::db::authenticated::range_request request,
                                 forge::db::authenticated::proof_tree tree) const {
   co_return co_await require_store().scan_range(version, std::move(request), tree);
}

boost::asio::awaitable<forge::db::authenticated::transaction>
authenticated_handle::join(transaction& active, forge::db::authenticated::version_id_t version) const {
   active.require_named_store(name());
   co_return co_await require_store().join(active.db_transaction(), version);
}

boost::asio::awaitable<forge::db::authenticated::checkpoint_summary>
authenticated_handle::export_checkpoint(snapshot active, forge::db::authenticated::root expected,
                                         forge::db::authenticated::checkpoint_sink sink,
                                         forge::db::authenticated::checkpoint_limits bounds) const {
   const auto& store = require_store();
   if (active.name() != name()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument, "db store snapshot belongs to another named store");
   }
   // Base admission captures the SAME Auth owner and Core snapshot immediately.
   auto operation = store.export_checkpoint(active.active_, expected, std::move(sink), bounds);
   return export_owned(state_, std::move(active), std::move(operation));
}

boost::asio::awaitable<forge::db::authenticated::checkpoint_summary>
authenticated_handle::export_owned(std::shared_ptr<store_handle_state> owner, snapshot active,
                                    boost::asio::awaitable<forge::db::authenticated::checkpoint_summary> operation) {
   // These owned parameters pin managed lifetime and every SAME-snapshot view
   // through terminal completion, including lazy start and suspension in sink.
   (void)owner;
   (void)active;
   co_return co_await std::move(operation);
}

forge::db::authenticated::transaction
authenticated_handle::join_restore(transaction& active, forge::db::authenticated::root expected,
                                    forge::db::authenticated::checkpoint_limits bounds) const {
   active.require_named_store(name());
   return require_store().join_restore(active.db_transaction(), expected, bounds);
}

boost::asio::awaitable<forge::db::authenticated::prune_result>
authenticated_handle::prune_through(transaction& active, forge::db::authenticated::version_id_t inclusive_boundary,
                                    forge::db::authenticated::prune_options options) const {
   active.require_named_store(name());
   co_return co_await require_store().prune_through(active.db_transaction(), inclusive_boundary, options);
}

std::string store_handle::name() const {
   if (!state_) {
      return {};
   }
   return state_->name();
}

std::shared_ptr<forge::db::core::driver> store_handle::require_driver() const {
   if (!state_) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "db store handle is empty");
   }
   return state_->require_driver();
}

boost::asio::awaitable<transaction> store_handle::begin_transaction() const {
   if (!state_) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "db store handle is empty");
   }
   co_return co_await state_->begin_transaction();
}

boost::asio::awaitable<snapshot> store_handle::begin_read() const {
   if (!state_) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "db store handle is empty");
   }
   co_return co_await state_->begin_read();
}

boost::asio::awaitable<void> store_handle::create_checkpoint(std::filesystem::path destination) const {
   co_await require_driver()->create_checkpoint(std::move(destination));
}

object_handle store_handle::objects() const {
   auto handle = object_handle{state_};
   (void)handle.require_setup_store();
   return handle;
}

blob_handle store_handle::blobs() const {
   auto handle = blob_handle{state_};
   (void)handle.require_store();
   return handle;
}

revision_handle store_handle::revisions() const {
   auto handle = revision_handle{state_};
   (void)handle.require_store();
   return handle;
}

authenticated_handle store_handle::authenticated(forge::db::authenticated::store::config settings) const {
   auto driver = require_driver();
   return authenticated_handle{
       state_,
       forge::db::authenticated::store{std::move(driver), std::move(settings)},
   };
}

} // namespace forge::plugins::db::store
