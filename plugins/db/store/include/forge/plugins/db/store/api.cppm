module;

#include <boost/asio/awaitable.hpp>
#include <forge/api/core/macros.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

export module forge.plugins.db.store.api;

export import forge.plugins.db.store.exceptions;
export import forge.plugins.db.store.types;

import forge.api.core.binding;
import forge.db.authenticated.store;
import forge.db.authenticated.transaction;
import forge.db.authenticated.types;
import forge.db.blob.ref;
import forge.db.blob.snapshot;
import forge.db.blob.store;
import forge.db.blob.transaction;
import forge.db.blob.types;
import forge.db.core.driver;
import forge.db.core.record;
import forge.db.ids.object_id;
import forge.db.object.hooks;
import forge.db.object.index;
import forge.db.object.object;
import forge.db.object.snapshot;
import forge.db.object.store;
import forge.db.object.transaction;
import forge.db.revision.store;
import forge.db.revision.transaction;
import forge.db.revision.types;

export namespace forge::plugins::db::store {

enum class activation_phase { closed, publishing, published_unconfirmed, durable };

// Durability does not imply publication. Consumers must match this exact root
// and their own verified activation intent before declaring product readiness.
struct activation_status {
   activation_phase phase = activation_phase::closed;
   std::filesystem::path root;
   std::uint64_t device = 0;
   std::uint64_t inode = 0;
   bool published = false;
};

class store_handle_state;

class closed_store {
 public:
   closed_store(closed_store&&) noexcept;
   closed_store& operator=(closed_store&&) noexcept;
   closed_store(const closed_store&) = delete;
   closed_store& operator=(const closed_store&) = delete;
   ~closed_store();

   [[nodiscard]] activation_status state() const;
   boost::asio::awaitable<void> activate(std::filesystem::path destination) const;
   boost::asio::awaitable<void> confirm_durability() const;

 private:
   explicit closed_store(std::shared_ptr<store_handle_state> state) noexcept;
   static boost::asio::awaitable<void> activate_owned(std::shared_ptr<store_handle_state>, std::filesystem::path);
   static boost::asio::awaitable<void> confirm_owned(std::shared_ptr<store_handle_state>);
   std::shared_ptr<store_handle_state> state_;
   friend class store_handle;
};

class snapshot {
 public:
   snapshot() = default;

   [[nodiscard]] bool active() const noexcept;
   [[nodiscard]] std::string name() const;
   [[nodiscard]] forge::db::object::snapshot objects() const;
   [[nodiscard]] forge::db::blob::snapshot blobs() const;

 private:
   snapshot(forge::db::core::snapshot active, std::string store_name,
            std::optional<forge::db::object::snapshot> objects, std::optional<forge::db::blob::snapshot> blobs);

   forge::db::core::snapshot active_;
   std::string store_name_;
   std::optional<forge::db::object::snapshot> objects_;
   std::optional<forge::db::blob::snapshot> blobs_;

   friend class store_handle_state;
   friend class authenticated_handle;
};

class transaction {
 public:
   transaction() = default;
   explicit transaction(forge::db::core::transaction active, std::string store_name = {});
   explicit transaction(forge::db::object::transaction active, std::string store_name = {});
   transaction(const transaction&) = delete;
   transaction& operator=(const transaction&) = delete;
   transaction(transaction&&) noexcept = default;
   transaction& operator=(transaction&&) noexcept = default;
   ~transaction() = default;

   [[nodiscard]] bool active() const noexcept;
   [[nodiscard]] forge::db::core::transaction& db_transaction();

   boost::asio::awaitable<void> commit();
   boost::asio::awaitable<void> rollback();

 private:
   void require_named_store(const std::string& expected) const;

   std::unique_ptr<forge::db::core::transaction> core_;
   std::optional<forge::db::object::transaction> object_;
   std::optional<forge::db::blob::transaction> blob_;
   std::string store_name_;

   friend class blob_handle;
   friend class authenticated_handle;
   friend class object_handle;
   friend class revision_handle;
};

class store_handle_state {
 public:
   virtual ~store_handle_state() = default;

   [[nodiscard]] virtual std::string name() const = 0;
   [[nodiscard]] virtual std::shared_ptr<forge::db::object::store> require_objects() const = 0;
   [[nodiscard]] virtual std::shared_ptr<forge::db::blob::store> require_blobs() const = 0;
   [[nodiscard]] virtual std::shared_ptr<forge::db::revision::store> require_revisions() const;
   virtual boost::asio::awaitable<transaction> begin_transaction() const = 0;
   virtual boost::asio::awaitable<snapshot> begin_read() const;

 private:
   virtual boost::asio::awaitable<void> close_for_activation() const;
   virtual void claim_activation() const;
   virtual boost::asio::awaitable<void> activate_closed(std::filesystem::path) const;
   virtual boost::asio::awaitable<void> confirm_activation() const;
   [[nodiscard]] virtual activation_status closed_state() const;
   [[nodiscard]] virtual bool activation_inflight() const noexcept;
   [[nodiscard]] virtual std::shared_ptr<forge::db::core::driver> require_driver() const = 0;

   friend class authenticated_handle;
   friend class object_handle;
   friend class revision_handle;
   friend class store_handle;
   friend class closed_store;
};

class object_handle {
 public:
   object_handle() = default;
   explicit object_handle(std::shared_ptr<store_handle_state> state) : state_{std::move(state)} {}

   [[nodiscard]] explicit operator bool() const noexcept {
      return static_cast<bool>(state_);
   }

   [[nodiscard]] std::string name() const;
   [[nodiscard]] forge::db::object::store::options runtime_options() const;

   boost::asio::awaitable<void> require_pristine(transaction& active) const;
   boost::asio::awaitable<void> restore_allocations(transaction& active,
                                                    std::vector<forge::db::object::store::allocation> values) const;

   template <forge::db::object::application_object_model Object> void register_object() const {
      require_setup_store()->template register_object<Object>();
   }

   void add_interceptor(std::shared_ptr<forge::db::object::interceptor> value) const;
   void add_observer(std::shared_ptr<forge::db::object::observer> value) const;

   boost::asio::awaitable<forge::db::object::transaction> begin_transaction() const;
   boost::asio::awaitable<forge::db::object::snapshot> begin_read() const;
   boost::asio::awaitable<forge::db::object::transaction> join(forge::db::core::transaction& active) const;
   boost::asio::awaitable<forge::db::object::transaction> join(transaction& active) const;

   template <typename SharedTransaction>
      requires requires(SharedTransaction& active) {
         { active.db_transaction() } -> std::same_as<forge::db::core::transaction&>;
      }
   boost::asio::awaitable<forge::db::object::transaction> join(SharedTransaction& active) const {
      co_return co_await join(active.db_transaction());
   }

   template <forge::db::ids::typed_id_like Id>
   boost::asio::awaitable<typename forge::db::object::index_for_id_t<Id>::value_type> get(Id id) const {
      co_return co_await require_store()->get(id);
   }

   template <forge::db::ids::typed_id_like Id>
   boost::asio::awaitable<std::optional<typename forge::db::object::index_for_id_t<Id>::value_type>> find(Id id) const {
      co_return co_await require_store()->find(id);
   }

   template <forge::db::object::object_model Object>
   boost::asio::awaitable<typename Object::value_type> get(forge::db::ids::object_id id) const {
      co_return co_await require_store()->template get<Object>(id);
   }

   template <forge::db::object::object_model Object>
   boost::asio::awaitable<std::optional<typename Object::value_type>> find(forge::db::ids::object_id id) const {
      co_return co_await require_store()->template find<Object>(id);
   }

   template <forge::db::object::application_object_value Value> boost::asio::awaitable<void> insert(Value value) const {
      co_await require_store()->insert(std::move(value));
   }

   template <forge::db::object::application_object_value Value>
   boost::asio::awaitable<void> replace(Value value) const {
      co_await require_store()->replace(std::move(value));
   }

   template <forge::db::ids::typed_id_like Id, typename Fn>
      requires forge::db::object::application_object_model<forge::db::object::index_for_id_t<Id>>
   boost::asio::awaitable<void> modify(Id id, Fn&& fn) const {
      co_await require_store()->modify(id, std::forward<Fn>(fn));
   }

   template <forge::db::ids::typed_id_like Id>
      requires forge::db::object::application_object_model<forge::db::object::index_for_id_t<Id>>
   boost::asio::awaitable<void> erase(Id id) const {
      co_await require_store()->erase(id);
   }

   template <forge::db::object::application_object_model Object>
   boost::asio::awaitable<void> erase(forge::db::ids::object_id id) const {
      co_await require_store()->template erase<Object>(id);
   }

   template <forge::db::object::object_model Object, typename Tag>
   [[nodiscard]] forge::db::object::index_view<Object, Tag> index() const {
      auto state = state_;
      auto view = require_setup_store()->template index<Object, Tag>();
      return view.guarded([state = std::move(state)] { (void)object_handle{state}.require_store(); });
   }

 private:
   [[nodiscard]] std::shared_ptr<forge::db::object::store> require_setup_store() const;
   [[nodiscard]] std::shared_ptr<forge::db::object::store> require_store() const;

   std::shared_ptr<store_handle_state> state_;

   friend class store_handle;
};

class blob_handle {
 public:
   blob_handle() = default;
   explicit blob_handle(std::shared_ptr<store_handle_state> state) : state_{std::move(state)} {}

   [[nodiscard]] explicit operator bool() const noexcept {
      return static_cast<bool>(state_);
   }

   [[nodiscard]] std::string name() const;

   boost::asio::awaitable<forge::db::blob::transaction> begin_transaction() const;
   boost::asio::awaitable<forge::db::blob::snapshot> begin_read() const;
   [[nodiscard]] forge::db::blob::transaction join(forge::db::core::transaction& active) const;
   [[nodiscard]] forge::db::blob::transaction join(transaction& active) const;

   template <typename SharedTransaction>
      requires requires(SharedTransaction& active) {
         { active.db_transaction() } -> std::same_as<forge::db::core::transaction&>;
      }
   [[nodiscard]] forge::db::blob::transaction join(SharedTransaction& active) const {
      return join(active.db_transaction());
   }

   boost::asio::awaitable<forge::db::blob::ref<forge::db::blob::digest>> put(std::vector<std::byte> payload) const;

   template <forge::db::blob::digest_algorithm Digest>
   boost::asio::awaitable<forge::db::blob::ref<Digest>> put_as(std::vector<std::byte> payload) const {
      co_return co_await require_store()->template put_as<Digest>(std::move(payload));
   }

   template <forge::db::blob::digest_algorithm Digest>
   boost::asio::awaitable<void> put(forge::db::blob::ref<Digest> value, std::vector<std::byte> payload) const {
      co_await require_store()->put(std::move(value), std::move(payload));
   }

   template <forge::db::blob::digest_algorithm Digest>
   boost::asio::awaitable<std::vector<std::byte>> get(forge::db::blob::ref<Digest> value) const {
      co_return co_await require_store()->get(std::move(value));
   }

   template <forge::db::blob::digest_algorithm Digest>
   boost::asio::awaitable<bool> has(forge::db::blob::ref<Digest> value) const {
      co_return co_await require_store()->has(std::move(value));
   }

   template <forge::db::blob::digest_algorithm Digest>
   boost::asio::awaitable<forge::db::blob::stat> stat_blob(forge::db::blob::ref<Digest> value) const {
      co_return co_await require_store()->stat_blob(std::move(value));
   }

   template <forge::db::blob::digest_algorithm Digest>
   boost::asio::awaitable<void> erase(forge::db::blob::ref<Digest> value) const {
      co_await require_store()->erase(std::move(value));
   }

   template <forge::db::blob::digest_algorithm Digest>
   boost::asio::awaitable<void> verify(forge::db::blob::ref<Digest> value) const {
      co_await require_store()->verify(std::move(value));
   }

   template <forge::db::blob::digest_algorithm Digest>
   boost::asio::awaitable<void> retain(forge::db::blob::ref<Digest> value, forge::db::blob::owner_ref owner) const {
      co_await require_store()->retain(std::move(value), std::move(owner));
   }

   template <forge::db::blob::digest_algorithm Digest>
   boost::asio::awaitable<void> release(forge::db::blob::ref<Digest> value, forge::db::blob::owner_ref owner) const {
      co_await require_store()->release(std::move(value), std::move(owner));
   }

   template <forge::db::blob::digest_algorithm Digest>
   boost::asio::awaitable<std::uint64_t> ref_count(forge::db::blob::ref<Digest> value) const {
      co_return co_await require_store()->ref_count(std::move(value));
   }

   boost::asio::awaitable<forge::db::blob::collect_result>
   collect_unreferenced(forge::db::blob::collect_options options = {}) const;

 private:
   [[nodiscard]] std::shared_ptr<forge::db::blob::store> require_store() const;

   std::shared_ptr<store_handle_state> state_;

   friend class store_handle;
};

class revision_handle {
 public:
   revision_handle() = default;
   explicit revision_handle(std::shared_ptr<store_handle_state> state) : state_{std::move(state)} {}

   [[nodiscard]] explicit operator bool() const noexcept {
      return static_cast<bool>(state_);
   }

   [[nodiscard]] std::string name() const;

   boost::asio::awaitable<void> require_pristine(transaction& active) const;

   boost::asio::awaitable<forge::db::revision::scope> join(transaction& active) const;
   boost::asio::awaitable<void> revert(transaction& active, forge::db::revision::revision_id_t expected_head) const;
   boost::asio::awaitable<forge::db::revision::prune_result>
   prune_through(transaction& active, forge::db::revision::revision_id_t inclusive_boundary,
                 forge::db::revision::prune_options options) const;

 private:
   [[nodiscard]] std::shared_ptr<forge::db::revision::store> require_store() const;

   std::shared_ptr<store_handle_state> state_;

   friend class store_handle;
};

class authenticated_handle {
 public:
   authenticated_handle() = default;

   [[nodiscard]] explicit operator bool() const noexcept {
      return static_cast<bool>(state_);
   }

   [[nodiscard]] std::string name() const;

   boost::asio::awaitable<std::optional<forge::db::authenticated::root>> earliest() const;
   boost::asio::awaitable<std::optional<forge::db::authenticated::root>> latest() const;
   boost::asio::awaitable<std::optional<forge::db::authenticated::root>>
   find_root(forge::db::authenticated::version_id_t version) const;
   boost::asio::awaitable<std::optional<forge::db::authenticated::bytes>>
   get(forge::db::authenticated::version_id_t version, std::span<const std::byte> key) const;
   boost::asio::awaitable<forge::db::authenticated::point_proof> prove(forge::db::authenticated::version_id_t version,
                                                                       std::span<const std::byte> key,
                                                                       bool include_value = true) const;
   boost::asio::awaitable<forge::db::authenticated::range_proof>
   prove_range(forge::db::authenticated::version_id_t version, forge::db::authenticated::range_request request,
               forge::db::authenticated::proof_tree tree = forge::db::authenticated::proof_tree::state) const;
   boost::asio::awaitable<forge::db::authenticated::verified_range>
   scan_range(forge::db::authenticated::version_id_t version, forge::db::authenticated::range_request request,
              forge::db::authenticated::proof_tree tree = forge::db::authenticated::proof_tree::state) const;

   boost::asio::awaitable<forge::db::authenticated::checkpoint_summary>
   export_checkpoint(snapshot active, forge::db::authenticated::root expected,
                     forge::db::authenticated::checkpoint_sink sink,
                     forge::db::authenticated::checkpoint_limits bounds) const;
   [[nodiscard]] forge::db::authenticated::transaction
   join_restore(transaction& active, forge::db::authenticated::root expected,
                forge::db::authenticated::checkpoint_limits bounds) const;

   boost::asio::awaitable<forge::db::authenticated::transaction>
   join(transaction& active, forge::db::authenticated::version_id_t version) const;
   boost::asio::awaitable<forge::db::authenticated::prune_result>
   prune_through(transaction& active, forge::db::authenticated::version_id_t inclusive_boundary,
                 forge::db::authenticated::prune_options options = {}) const;

 private:
   authenticated_handle(std::shared_ptr<store_handle_state> state, forge::db::authenticated::store value)
       : state_{std::move(state)}, store_{std::move(value)} {}

   [[nodiscard]] const forge::db::authenticated::store& require_store() const;

   static boost::asio::awaitable<forge::db::authenticated::checkpoint_summary>
   export_owned(std::shared_ptr<store_handle_state> owner, snapshot active,
                boost::asio::awaitable<forge::db::authenticated::checkpoint_summary> operation);

   std::shared_ptr<store_handle_state> state_;
   forge::db::authenticated::store store_;

   friend class store_handle;
};

class store_handle {
 public:
   store_handle() = default;
   explicit store_handle(std::shared_ptr<store_handle_state> state) : state_{std::move(state)} {}

   [[nodiscard]] explicit operator bool() const noexcept {
      return static_cast<bool>(state_);
   }

   [[nodiscard]] std::string name() const;

   boost::asio::awaitable<transaction> begin_transaction() const;
   boost::asio::awaitable<snapshot> begin_read() const;
   boost::asio::awaitable<void> create_checkpoint(std::filesystem::path destination) const;
   // Only a configured, owned durable-sync MDBX store supports this operation.
   // Core driver_busy retains this SAME owner with admissions closed; explicit
   // retry is close-only. No token is issued for any unproven close.
   boost::asio::awaitable<closed_store> close_for_activation() const;

   [[nodiscard]] object_handle objects() const;
   [[nodiscard]] blob_handle blobs() const;
   [[nodiscard]] revision_handle revisions() const;
   [[nodiscard]] authenticated_handle authenticated(forge::db::authenticated::store::config settings) const;

 private:
   static boost::asio::awaitable<closed_store> close_owned(std::shared_ptr<store_handle_state>);
   [[nodiscard]] std::shared_ptr<forge::db::core::driver> require_driver() const;

   std::shared_ptr<store_handle_state> state_;

   friend class api;
   friend class store_handle_state;
};

class api : public forge::api::core::contract<api, forge::api::core::surface::local> {
 public:
   virtual ~api() = default;

   virtual boost::asio::awaitable<void> add_store(std::string name, std::shared_ptr<forge::db::core::driver> driver,
                                                  store_options options = {}) = 0;
   virtual boost::asio::awaitable<store_handle> store(std::string name) = 0;
   virtual boost::asio::awaitable<void> flush(std::string name, bool sync = true) = 0;
   virtual boost::asio::awaitable<void> flush_all(bool sync = true) = 0;
   virtual boost::asio::awaitable<::forge::plugins::db::store::status> status() = 0;
};

} // namespace forge::plugins::db::store

namespace store_plugin_api = ::forge::plugins::db::store;

FORGE_EXPORT_API(store_plugin_api::api, FORGE_API_CONTRACT("forge.plugins.db.store", 2, 0))
