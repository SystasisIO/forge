#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <exception>
#include <list>
#include <memory>
#include <mutex>
#include <vector>

#include "protocol_slot.hxx"

namespace forge::plugins::net::p2p::node::detail {

class protocol_registry final : public std::enable_shared_from_this<protocol_registry> {
 public:
   explicit protocol_registry(std::size_t max_slots);
   ~protocol_registry();
   [[nodiscard]] forge::api::p2p::publication publish(forge::api::p2p::api_binding binding);
   [[nodiscard]] protocol_registration register_raw(forge::net::p2p::protocol_id protocol,
       forge::net::p2p::node::protocol_handler handler, const protocol_registration* previous = nullptr);
   void configure(std::vector<forge::net::p2p::protocol_id> protocols, std::size_t max_slots);
   void bind_executor(boost::asio::any_io_executor executor);
   void attach(std::shared_ptr<const protocol_operations> operations);
   void close_generation(std::shared_ptr<protocol_slot> slot,
                         std::shared_ptr<protocol_slot::generation> generation) noexcept;
   boost::asio::awaitable<void> async_close_generation(std::shared_ptr<protocol_slot> slot,
       std::shared_ptr<api_publication_generation> generation);
   void request_close() noexcept;
   boost::asio::awaitable<void> async_close();

 private:
   struct slot_record {
      std::shared_ptr<protocol_slot> slot;
      bool attaching = false;
      bool closing = false;
   };
   struct generation_record {
      const api_publication_generation* identity;
      std::weak_ptr<api_publication_generation> value;
   };
   using position = std::list<slot_record>::iterator;
   [[nodiscard]] position find(const forge::net::p2p::protocol_id& protocol) noexcept;
   void require_available(const forge::net::p2p::protocol_id& protocol) const;
   void finish_attach(const std::shared_ptr<protocol_slot>& slot,
                      const std::shared_ptr<const protocol_operations>& operations);
   void retry_closing(const forge::net::p2p::protocol_id& protocol);
   void detach(const std::shared_ptr<protocol_slot>& slot);
   void retire_generation(const api_publication_generation* generation) noexcept;
   void prune_generations_locked() noexcept;

   mutable std::mutex mutex_;
   std::list<slot_record> slots_;
   std::list<generation_record> generations_;
   std::vector<forge::net::p2p::protocol_id> reserved_;
   std::shared_ptr<const protocol_operations> operations_;
   boost::asio::any_io_executor executor_;
   forge::asio::notification close_ready_;
   std::exception_ptr close_failure_;
   bool closed_ = false;
   bool closing_ = false;
   std::size_t max_slots_;
};

[[nodiscard]] std::shared_ptr<protocol_registry> make_protocol_registry(std::size_t max_slots);
[[nodiscard]] forge::api::p2p::publication
publish_api_publication(const std::shared_ptr<protocol_registry>& registry,
                        forge::api::p2p::api_binding binding);
void bind_protocol_registry_executor(
   const std::shared_ptr<protocol_registry>& registry,
   boost::asio::any_io_executor owner_executor);
void attach_protocol_registry(
   const std::shared_ptr<protocol_registry>& registry,
   const std::shared_ptr<forge::net::p2p::node>& node);
void configure_protocol_registry(const std::shared_ptr<protocol_registry>& registry,
                                 std::vector<forge::net::p2p::protocol_id> protocols, std::size_t max_slots);
void request_close_protocol_registry(
   const std::shared_ptr<protocol_registry>& registry) noexcept;
boost::asio::awaitable<void> async_close_protocol_registry(
   const std::shared_ptr<protocol_registry>& registry);
}
