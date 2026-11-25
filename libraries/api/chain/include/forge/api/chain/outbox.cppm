module;

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

export module forge.api.chain.outbox;

export import forge.api.chain.destination;
export import forge.chain.protocol.transaction_query;

import forge.db.object.registration;
import forge.db.object.snapshot;
import forge.db.object.transaction;

export namespace forge::api::chain {

class outbox {
 public:
   enum class replay : std::uint8_t { reconcile_only = 0, same_packed = 1 };

   struct delivery_policy {
      replay mode = replay::reconcile_only;
      std::string id;
      std::uint32_t version = 1;

      bool operator==(const delivery_policy&) const = default;
   };

   struct delivery_observation {
      forge::chain::protocol::transaction_id transaction;
      forge::chain::protocol::time_point_sec expires;
      std::uint32_t sends = 0;
      forge::chain::protocol::transaction_lifecycle state = forge::chain::protocol::transaction_lifecycle::unknown;
   };

   struct options {
      std::size_t batch_limit = 16;
      std::uint32_t max_sends = 8;
      std::chrono::milliseconds poll_interval{1000};
      std::chrono::milliseconds timeout{30000};
      std::function<replay(const delivery_policy&, const delivery_observation&)> decide;
   };

   struct transport {
      std::function<boost::asio::awaitable<forge::chain::protocol::transaction_submit_response>(
          forge::chain::protocol::chain_id, destination,
          forge::chain::protocol::transaction_submit_request, std::stop_token)> submit;
      // Supplies locally verified status, including a receipt for a finalized transaction.
      std::function<boost::asio::awaitable<forge::chain::protocol::transaction_status_response>(
          forge::chain::protocol::chain_id, destination,
          forge::chain::protocol::transaction_id, std::stop_token)> status;
      // Optional bounded, synchronous lookup in local configuration while the writer is open.
      std::function<bool(const destination&)> configured;
   };

   using admission = std::function<boost::asio::awaitable<void>(
       forge::db::object::transaction&, forge::chain::protocol::transaction_id,
       const forge::chain::protocol::packed_transaction&)>;

   // The supplied store handle owns one ObjectDB, including its writer lane.
   template <typename Store> outbox(Store store, transport io, options settings)
       : outbox(storage{
                    .register_object = [store](forge::db::object::registration model) mutable {
                       store.register_object(std::move(model));
                    },
                    .begin_transaction = [store]() mutable { return store.begin_transaction(); },
                    .begin_read = [store]() mutable { return store.begin_read(); },
                    .join = [store](forge::db::object::transaction& active) mutable { return store.join(active); },
                 },
                 std::move(io), std::move(settings)) {}

   ~outbox();
   outbox(const outbox&) = delete;
   outbox& operator=(const outbox&) = delete;

   // Stages the exact packet in the caller's writer. The caller owns COMMIT.
   boost::asio::awaitable<forge::chain::protocol::transaction_id>
   stage(forge::db::object::transaction& active, forge::chain::protocol::chain_id chain, destination target,
         const forge::chain::protocol::packed_transaction& transaction,
         delivery_policy policy);

   // Commits new admission and the packet together before claiming delivery.
   // Admission runs once for a new packet and uses the supplied writer without committing it.
   // Success returns the acknowledged transaction ID. The final receipt is available through result().
   boost::asio::awaitable<forge::chain::protocol::transaction_id>
   submit(forge::chain::protocol::chain_id chain, destination target,
          forge::chain::protocol::packed_transaction transaction, delivery_policy policy, admission admit = {},
          std::stop_token stop = {});

   boost::asio::awaitable<std::optional<forge::chain::protocol::transaction_status_response>>
   result(forge::chain::protocol::chain_id chain, destination target,
          forge::chain::protocol::transaction_id transaction, std::stop_token stop = {});

   // Performs one bounded reconciliation pass over due records.
   boost::asio::awaitable<void> recover(std::stop_token stop = {});

 private:
   struct storage {
      std::function<void(forge::db::object::registration)> register_object;
      std::function<boost::asio::awaitable<forge::db::object::transaction>()> begin_transaction;
      std::function<boost::asio::awaitable<forge::db::object::snapshot>()> begin_read;
      std::function<boost::asio::awaitable<forge::db::object::transaction>(forge::db::object::transaction&)> join;
   };

   outbox(storage store, transport io, options settings);

   struct impl;
   std::unique_ptr<impl> impl_;
};

} // namespace forge::api::chain
