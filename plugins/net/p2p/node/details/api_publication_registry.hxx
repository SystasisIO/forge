#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <list>
#include <memory>
#include <mutex>

namespace forge::plugins::net::p2p::node::detail {

class api_publication_generation;
class api_publication_slot;

class api_publication_registry final : public std::enable_shared_from_this<api_publication_registry> {
 public:
   api_publication_registry();
   ~api_publication_registry();

   api_publication_registry(const api_publication_registry&) = delete;
   api_publication_registry& operator=(const api_publication_registry&) = delete;

   [[nodiscard]] forge::api::p2p::publication publish(forge::api::p2p::api_binding binding);
   void bind_executor(boost::asio::any_io_executor owner_executor);
   void attach(std::shared_ptr<forge::net::p2p::node> node);
   [[nodiscard]] bool contains(const forge::net::p2p::protocol_id& protocol) const noexcept;
   void close_generation(std::shared_ptr<api_publication_slot> slot,
                         std::shared_ptr<api_publication_generation> generation) noexcept;
   boost::asio::awaitable<void> async_close_generation(std::shared_ptr<api_publication_generation> generation);
   void request_close() noexcept;
   boost::asio::awaitable<void> async_close();

 private:
   struct slot_record {
      std::shared_ptr<api_publication_slot> slot;
   };
   struct generation_record {
      const api_publication_generation* identity;
      std::weak_ptr<api_publication_generation> value;
   };

   [[nodiscard]] std::list<slot_record>::iterator
   find_active(const forge::net::p2p::protocol_id& protocol) noexcept;
   [[nodiscard]] std::list<slot_record>::const_iterator
   find_active(const forge::net::p2p::protocol_id& protocol) const noexcept;
   [[nodiscard]] bool contains_locked(const forge::net::p2p::protocol_id& protocol) const noexcept;
   boost::asio::awaitable<void> wait_for_close();
   void retire_generation(const api_publication_generation* generation) noexcept;
   void prune_generations_locked() noexcept;

   mutable std::mutex mutex_;
   std::list<slot_record> active_slots_;
   std::list<slot_record> closing_slots_;
   std::list<generation_record> generations_;
   std::shared_ptr<forge::net::p2p::node> node_;
   boost::asio::any_io_executor owner_executor_;
   forge::asio::notification close_ready_;
   bool closed_ = false;
   bool close_complete_ = false;
};

} // namespace forge::plugins::net::p2p::node::detail
