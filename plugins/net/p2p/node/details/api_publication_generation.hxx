#pragma once

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>

namespace forge::plugins::net::p2p::node::detail {

class api_publication_generation final : public std::enable_shared_from_this<api_publication_generation> {
 public:
   explicit api_publication_generation(forge::api::p2p::api_binding binding);
   ~api_publication_generation();

   api_publication_generation(const api_publication_generation&) = delete;
   api_publication_generation& operator=(const api_publication_generation&) = delete;

   [[nodiscard]] bool active() const noexcept;
   void set_retirement_handler(std::function<void(const api_publication_generation*)> handler);
   boost::asio::awaitable<void> accept(forge::net::p2p::node::incoming_protocol_stream stream);
   void request_close() noexcept;
   boost::asio::awaitable<void> async_close();

 private:
   struct session_entry {
      std::shared_ptr<forge::api::stream::session> value;
      std::list<std::shared_ptr<session_entry>>::iterator position;
   };

   void release(const std::shared_ptr<session_entry>& entry) noexcept;
   boost::asio::awaitable<void> wait_for_drain();

   forge::api::p2p::api_binding binding_;
   mutable std::mutex mutex_;
   std::list<std::shared_ptr<session_entry>> active_sessions_;
   std::list<std::shared_ptr<session_entry>> closing_sessions_;
   std::shared_ptr<forge::asio::notification> drained_;
   std::function<void(const api_publication_generation*)> retirement_;
   std::size_t active_count_ = 0;
   bool admission_open_ = true;
   bool cancellation_complete_ = false;
};

[[nodiscard]] std::shared_ptr<api_publication_generation>
make_api_publication_generation(forge::api::p2p::api_binding binding);
[[nodiscard]] bool
api_publication_generation_active(const std::shared_ptr<api_publication_generation>& generation) noexcept;
void request_close_api_publication_generation(const std::shared_ptr<api_publication_generation>& generation) noexcept;
void set_api_publication_generation_retirement(
   const std::shared_ptr<api_publication_generation>& generation,
   std::function<void(const api_publication_generation*)> handler);
boost::asio::awaitable<void>
async_close_api_publication_generation(std::shared_ptr<api_publication_generation> generation);
boost::asio::awaitable<void>
accept_api_publication_generation(const std::shared_ptr<api_publication_generation>& generation,
                                  forge::net::p2p::node::incoming_protocol_stream stream);

} // namespace forge::plugins::net::p2p::node::detail
