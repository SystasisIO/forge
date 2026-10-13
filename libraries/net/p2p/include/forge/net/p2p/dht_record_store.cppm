module;

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

export module forge.net.p2p.dht.record_store;

export import forge.net.p2p.dht;
import forge.multiformats.multiaddr;
import forge.net.p2p.identity;

export namespace forge::net::p2p::dht {

class record_store {
 public:
   class persistence;

   struct value_record {
      dht::record record;
      std::chrono::system_clock::time_point expires_at{};
   };

   enum class put_outcome : std::uint8_t {
      incoming_stored,
      existing_preferred,
   };

   struct put_result {
      value_record selected;
      put_outcome outcome = put_outcome::existing_preferred;
   };

   struct provider_record {
      dht::key key;
      peer_id provider;
      std::vector<forge::multiformats::multiaddr> endpoints;
      std::chrono::system_clock::time_point provider_expires_at{};
      std::chrono::system_clock::time_point addresses_expires_at{};
      bool local_owned = false;
   };

   struct provider_key {
      dht::key key;
      peer_id provider;

      [[nodiscard]] friend bool operator==(const provider_key&, const provider_key&) noexcept = default;
   };

   struct mutation_batch {
      std::vector<value_record> value_upserts;
      std::vector<dht::key> value_removals;
      std::vector<provider_record> provider_upserts;
      std::vector<provider_key> provider_removals;
   };

   struct apply_result {
      bool durability_confirmed = true;
      std::string durability_failure;
   };

   enum class hydration_kind : std::uint8_t {
      values,
      providers,
   };

   struct hydration_request {
      hydration_kind kind = hydration_kind::values;
      std::optional<std::vector<std::byte>> cursor;
      std::size_t limit = 256;
   };

   struct hydration_page {
      std::vector<value_record> values;
      std::vector<provider_record> providers;
      std::optional<std::vector<std::byte>> cursor;
   };

   struct prune_result {
      std::vector<dht::key> values;
      std::vector<provider_key> providers;
      std::vector<provider_record> provider_address_updates;
      bool may_have_more = false;
      apply_result durability;
   };

   struct persistence_status {
      std::uint64_t failure_count = 0;
      bool degraded = false;
      bool durability_uncertain = false;
      bool closing = false;
      bool closed = false;
      std::string last_failure;
   };

   struct options {
      std::shared_ptr<persistence> persistence;
      public_key_resolver public_keys;
      // Operational quotas belong to record_store, not its persistence backend.
      std::size_t max_values = 4'096;
      std::size_t max_providers = 16'384;
      std::size_t max_local_providers = 1'024;
      std::size_t max_providers_per_key = 20;
      std::size_t max_total_bytes = 64 * 1024 * 1024;
      std::size_t max_record_bytes = 1024 * 1024;
      std::size_t hydration_page_limit = 256;
      std::size_t prune_page_limit = 256;
      std::size_t max_persistence_waiters = 256;
      std::size_t max_hydration_pages = 4'096;
   };

   explicit record_store(dht::profile profile);
   record_store(dht::profile profile, options options_value);
   ~record_store();

   record_store(const record_store&) = delete;
   record_store& operator=(const record_store&) = delete;

   record_store(record_store&&) noexcept;
   record_store& operator=(record_store&&) noexcept;

   [[nodiscard]] static std::shared_ptr<persistence> make_memory_persistence();
   // Node defaults derive per-key and record-byte bounds from the owning profile.
   [[nodiscard]] static options options_for(const dht::profile& profile);

   boost::asio::awaitable<put_result>
   async_put(value_record incoming, std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
   boost::asio::awaitable<std::optional<put_result>>
   async_put_received(value_record incoming,
                      std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
   boost::asio::awaitable<void>
   async_upsert_provider(provider_record value,
                         std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
   boost::asio::awaitable<void> async_remove_provider(provider_key key);
   boost::asio::awaitable<void>
   async_hydrate(std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
   boost::asio::awaitable<prune_result>
   async_prune_expired(std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
   boost::asio::awaitable<void> async_flush();
   boost::asio::awaitable<void> async_close();

   [[nodiscard]] std::optional<value_record>
   find_value(const dht::key& key, std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) const;
   [[nodiscard]] std::vector<provider_record>
   find_providers(const dht::key& key, std::size_t limit,
                  std::chrono::system_clock::time_point now = std::chrono::system_clock::now()) const;
   [[nodiscard]] persistence_status persistence_state() const;

 private:
   struct impl;
   std::shared_ptr<impl> impl_;
};

class record_store::persistence {
 public:
   virtual ~persistence();

   virtual boost::asio::awaitable<hydration_page> async_hydrate(hydration_request request) = 0;
   // A false durability confirmation means the mutation committed, but the
   // requested durable flush failed. Throwing means no commit is known.
   virtual boost::asio::awaitable<apply_result> async_apply(mutation_batch batch) = 0;
   virtual boost::asio::awaitable<prune_result> async_prune_expired(std::chrono::system_clock::time_point now,
                                                                    std::size_t limit) = 0;
   virtual boost::asio::awaitable<void> async_flush() = 0;
   virtual boost::asio::awaitable<void> async_close() = 0;
};

} // namespace forge::net::p2p::dht

export namespace forge::net::p2p {

// Scalar-only preflight: does not copy or invoke callbacks or persistence.
void validate(const dht::record_store::options& options);
// Declaration-only profile check permits preflight before value policies are bound.
void validate(const dht::profile& profile, const dht::record_store::options& options);

} // namespace forge::net::p2p
