module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

export module forge.db.core.driver;

import forge.db.core.record;

export import forge.db.core.participant;

namespace forge::db::core::detail {
class driver_state;
}

export namespace forge::db::core {

class driver;

struct capabilities {
   bool snapshot_reads = false;
   bool writes = true;
   bool savepoints = false;
   bool record_locks = false;
};

class session {
 public:
   virtual ~session() = default;

   [[nodiscard]] virtual capabilities capabilities() const noexcept = 0;
   virtual boost::asio::awaitable<std::optional<std::vector<std::byte>>> get(family column_family,
                                                                             record_key key) = 0;
   virtual boost::asio::awaitable<std::optional<std::vector<std::byte>>>
   get_for_update(family column_family, record_key key);
   virtual boost::asio::awaitable<void> put(family column_family, record_key key, std::vector<std::byte> value) = 0;
   virtual boost::asio::awaitable<void> erase(family column_family, record_key key) = 0;
   virtual boost::asio::awaitable<record_page> scan_page(family column_family,
                                                         record_range range,
                                                         page_request request) = 0;
   virtual boost::asio::awaitable<void> create_savepoint();
   virtual boost::asio::awaitable<void> rollback_to_savepoint();
   virtual boost::asio::awaitable<void> release_savepoint();
   virtual boost::asio::awaitable<void> commit() = 0;
   virtual boost::asio::awaitable<void> rollback() = 0;
};

class transaction {
 public:
   using before_commit_fn = std::function<boost::asio::awaitable<void>()>;
   using after_commit_fn = std::function<boost::asio::awaitable<void>()>;
   using after_rollback_fn = std::function<boost::asio::awaitable<void>()>;

   transaction() = default;
   transaction(std::unique_ptr<session> active, boost::asio::any_io_executor cleanup_executor);
   ~transaction();

   transaction(const transaction&) = delete;
   transaction& operator=(const transaction&) = delete;

   transaction(transaction&&) noexcept;
   transaction& operator=(transaction&&) noexcept;

   [[nodiscard]] bool active() const noexcept;
   [[nodiscard]] bool belongs_to(const driver& owner) const noexcept;
   [[nodiscard]] capabilities capabilities() const noexcept;

   boost::asio::awaitable<std::optional<std::vector<std::byte>>> get(family column_family, record_key key);
   boost::asio::awaitable<std::optional<std::vector<std::byte>>>
   get_for_update(family column_family, record_key key);
   boost::asio::awaitable<void> put(family column_family, record_key key, std::vector<std::byte> value);
   boost::asio::awaitable<void> erase(family column_family, record_key key);
   boost::asio::awaitable<record_page> scan_page(family column_family, record_range range, page_request request);

   void attach_participant(std::shared_ptr<transaction_participant> participant);
   [[nodiscard]] bool has_participant(std::string_view name) const noexcept;
   [[nodiscard]] bool claims_family(const family& column_family) const noexcept;
   [[nodiscard]] bool captures_mutations() const noexcept;

   boost::asio::awaitable<savepoint_id_t> create_savepoint();
   boost::asio::awaitable<void> rollback_to_savepoint(savepoint_id_t savepoint);
   boost::asio::awaitable<void> release_savepoint(savepoint_id_t savepoint);

   void before_commit(before_commit_fn hook);
   void after_commit(after_commit_fn hook);
   void after_rollback(after_rollback_fn hook);

   boost::asio::awaitable<void> commit();
   boost::asio::awaitable<void> rollback();

 private:
   transaction(std::unique_ptr<session> active, boost::asio::any_io_executor cleanup_executor,
               std::shared_ptr<const void> origin);

   boost::asio::awaitable<void> prepare_prewrite_locks();
   boost::asio::awaitable<void>
   mutate(family column_family, record_key key, std::optional<std::vector<std::byte>> after);

   struct impl;
   std::shared_ptr<impl> impl_;

   friend class driver;
};

class snapshot {
 public:
   snapshot() = default;
   explicit snapshot(std::unique_ptr<session> active);

   [[nodiscard]] bool active() const noexcept;
   [[nodiscard]] bool belongs_to(const driver& owner) const noexcept;

   boost::asio::awaitable<std::optional<std::vector<std::byte>>> get(family column_family, record_key key);
   boost::asio::awaitable<record_page> scan_page(family column_family, record_range range, page_request request);

 private:
   snapshot(std::unique_ptr<session> active, std::shared_ptr<const void> origin);

   std::shared_ptr<session> active_;
   std::shared_ptr<const void> origin_;

   friend class driver;
};

class driver {
 public:
   driver();
   virtual ~driver();

   driver(const driver&) = delete;
   driver& operator=(const driver&) = delete;
   driver(driver&&) = delete;
   driver& operator=(driver&&) = delete;

   boost::asio::awaitable<transaction> begin_transaction();
   boost::asio::awaitable<snapshot> begin_read();
   boost::asio::awaitable<void> create_checkpoint(std::filesystem::path destination);
   boost::asio::awaitable<void> async_close();
   virtual boost::asio::awaitable<void> async_flush(bool sync) = 0;

 protected:
   class operation_admission {
    public:
      ~operation_admission();

      operation_admission(const operation_admission&) = delete;
      operation_admission& operator=(const operation_admission&) = delete;
      operation_admission(operation_admission&& other) noexcept;
      operation_admission& operator=(operation_admission&& other) noexcept;

    private:
      explicit operation_admission(std::shared_ptr<detail::driver_state> state) noexcept;
      void release() noexcept;

      std::shared_ptr<detail::driver_state> state_;

      friend class driver;
   };

   [[nodiscard]] operation_admission admit_operation() const;

 private:
   virtual boost::asio::awaitable<std::unique_ptr<session>> open_transaction() = 0;
   virtual boost::asio::awaitable<std::unique_ptr<session>> open_snapshot() = 0;
   virtual boost::asio::awaitable<void> create_checkpoint_impl(std::filesystem::path destination);
   virtual boost::asio::awaitable<void> close_driver();

   std::shared_ptr<const void> snapshot_origin_ = std::make_shared<std::byte>();
   std::shared_ptr<detail::driver_state> state_;

   friend class snapshot;
   friend class transaction;
};

} // namespace forge::db::core
