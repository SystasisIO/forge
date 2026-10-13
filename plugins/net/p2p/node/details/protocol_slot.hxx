#pragma once

#include <atomic>
#include <memory>
#include <mutex>

namespace forge::plugins::net::p2p::node::detail {
class api_publication_generation;
struct protocol_operations;

class protocol_slot final : public std::enable_shared_from_this<protocol_slot> {
 public:
   struct generation {
      forge::net::p2p::node::protocol_handler raw;
      std::shared_ptr<api_publication_generation> api;
   };

   explicit protocol_slot(forge::net::p2p::protocol_id protocol);
   [[nodiscard]] const forge::net::p2p::protocol_id& protocol() const noexcept;
   [[nodiscard]] std::shared_ptr<generation> current() const noexcept;
   [[nodiscard]] std::shared_ptr<generation> replace(std::shared_ptr<generation> next) noexcept;
   void attach(const std::shared_ptr<const protocol_operations>& operations);
   // A throwing remove retains the operations for retry, even after mutation.
   void detach();

 private:
   static boost::asio::awaitable<void> accept(std::shared_ptr<generation> owner,
                                             forge::net::p2p::node::incoming_protocol_stream stream);
   forge::net::p2p::protocol_id protocol_;
   std::shared_ptr<generation> current_;
   std::mutex mutex_;
   std::shared_ptr<const protocol_operations> operations_;
};
}
